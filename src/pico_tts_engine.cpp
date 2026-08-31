#include "pico_tts_engine.hpp"

#include <algorithm>

#include "pico_log.hpp"
#include "pico_paths.hpp"

namespace pico {

namespace {

//: Longest run of silence honoured for a single SPVA_Silence fragment. SAPI
//: expresses the duration in milliseconds as a 32 bit value, and an application
//: asking for hours of it would otherwise sit in the write loop indefinitely.
constexpr ULONG kMaxSilenceMs = 60 * 1000;

//: Written in one go rather than a sample at a time.
constexpr size_t kSilenceBlockSamples = 4096;

bool WantsEvent(ULONGLONG interest, SPEVENTENUM event) {
	return (interest & SPFEI(event)) != 0;
}

//: Maps each UTF-16 index in `text` to the byte offset of the same position in
//: its UTF-8 encoding, with one extra entry for the end. Used to turn the word
//: spans, which are in wide characters, into the byte ranges pico is fed.
std::vector<size_t> Utf16ToUtf8Offsets(const std::wstring& text) {
	std::vector<size_t> offsets(text.size() + 1, 0);
	size_t byteOffset = 0;
	for (size_t i = 0; i < text.size(); ++i) {
		offsets[i] = byteOffset;
		const wchar_t unit = text[i];
		if (unit < 0x80) {
			byteOffset += 1;
		} else if (unit < 0x800) {
			byteOffset += 2;
		} else if (unit >= 0xD800 && unit <= 0xDBFF && i + 1 < text.size() &&
				   text[i + 1] >= 0xDC00 && text[i + 1] <= 0xDFFF) {
			// A surrogate pair is one four byte sequence. The low half is
			// recorded at the same place the pair ends, which is the only offset
			// a span could legitimately refer to.
			offsets[i + 1] = byteOffset;
			byteOffset += 4;
			++i;
		} else {
			byteOffset += 3;
		}
	}
	offsets[text.size()] = byteOffset;
	return offsets;
}

}  // namespace

PicoTTSEngine::PicoTTSEngine() {
	LogInit();
}

PicoTTSEngine::~PicoTTSEngine() {
	if (token_ != nullptr) {
		token_->Release();
		token_ = nullptr;
	}
}

// ---------------------------------------------------------------------------
// IUnknown
// ---------------------------------------------------------------------------

STDMETHODIMP PicoTTSEngine::QueryInterface(REFIID riid, void** ppv) {
	if (ppv == nullptr) {
		return E_POINTER;
	}
	*ppv = nullptr;
	if (riid == IID_IUnknown || riid == IID_ISpTTSEngine) {
		*ppv = static_cast<ISpTTSEngine*>(this);
	} else if (riid == IID_ISpObjectWithToken) {
		*ppv = static_cast<ISpObjectWithToken*>(this);
	} else {
		return E_NOINTERFACE;
	}
	AddRef();
	return S_OK;
}

STDMETHODIMP_(ULONG) PicoTTSEngine::AddRef() {
	return static_cast<ULONG>(InterlockedIncrement(&referenceCount_));
}

STDMETHODIMP_(ULONG) PicoTTSEngine::Release() {
	const LONG remaining = InterlockedDecrement(&referenceCount_);
	if (remaining == 0) {
		delete this;
		return 0;
	}
	return static_cast<ULONG>(remaining);
}

// ---------------------------------------------------------------------------
// ISpObjectWithToken
// ---------------------------------------------------------------------------

STDMETHODIMP PicoTTSEngine::SetObjectToken(ISpObjectToken* pToken) {
	if (pToken == nullptr) {
		return E_INVALIDARG;
	}
	std::lock_guard<std::mutex> lock(mutex_);
	if (token_ != nullptr) {
		token_->Release();
	}
	token_ = pToken;
	token_->AddRef();
	// The lingware is several megabytes, and SAPI creates this object whenever it
	// enumerates voices, so opening it is left until something is actually
	// spoken. Only the identity is settled here.
	return ResolveVoiceFromToken();
}

STDMETHODIMP PicoTTSEngine::GetObjectToken(ISpObjectToken** ppToken) {
	if (ppToken == nullptr) {
		return E_POINTER;
	}
	std::lock_guard<std::mutex> lock(mutex_);
	*ppToken = token_;
	if (token_ != nullptr) {
		token_->AddRef();
		return S_OK;
	}
	return S_FALSE;
}

HRESULT PicoTTSEngine::ResolveVoiceFromToken() {
	voice_ = nullptr;
	if (token_ == nullptr) {
		return E_UNEXPECTED;
	}

	// The registration writes the voice id into the token, which is the direct
	// answer when it is there.
	LPWSTR value = nullptr;
	if (SUCCEEDED(token_->GetStringValue(L"PicoVoice", &value)) && value != nullptr) {
		voice_ = FindVoice(value);
		CoTaskMemFree(value);
	}

	// Failing that, fall back to the language attribute SAPI itself relies on, so
	// that a hand written or migrated token still resolves to something.
	if (voice_ == nullptr) {
		ISpDataKey* attributes = nullptr;
		if (SUCCEEDED(token_->OpenKey(L"Attributes", &attributes)) && attributes != nullptr) {
			LPWSTR language = nullptr;
			if (SUCCEEDED(attributes->GetStringValue(L"Language", &language)) && language != nullptr) {
				const LANGID langId = static_cast<LANGID>(wcstoul(language, nullptr, 16));
				voice_ = FindVoiceForLanguage(langId);
				CoTaskMemFree(language);
			}
			attributes->Release();
		}
	}

	if (voice_ == nullptr) {
		PICO_LOG_WARN("token names no voice this engine knows; falling back to %ls",
					  DefaultVoice().id);
		voice_ = &DefaultVoice();
	}
	PICO_LOG_INFO("token resolved to voice %ls", voice_->id);
	return S_OK;
}

HRESULT PicoTTSEngine::EnsureVoiceOpen() {
	if (voice_ == nullptr) {
		voice_ = &DefaultVoice();
	}
	if (engine_.IsOpen() && engine_.Voice() == voice_) {
		return S_OK;
	}
	const HRESULT hr = engine_.Open(*voice_);
	if (FAILED(hr)) {
		PICO_LOG_ERROR("could not open voice %ls: %s", voice_->id, engine_.LastError().c_str());
	}
	return hr;
}

// ---------------------------------------------------------------------------
// ISpTTSEngine
// ---------------------------------------------------------------------------

STDMETHODIMP PicoTTSEngine::GetOutputFormat(const GUID* /*pTargetFormatId*/,
											const WAVEFORMATEX* /*pTargetWaveFormatEx*/,
											GUID* pOutputFormatId,
											WAVEFORMATEX** ppCoMemOutputWaveFormatEx) {
	if (pOutputFormatId == nullptr || ppCoMemOutputWaveFormatEx == nullptr) {
		return E_POINTER;
	}
	// Pico's lingware is built for one rate only, so the request is answered with
	// what the engine actually produces and SAPI converts if the application
	// wanted something else.
	WAVEFORMATEX* format = static_cast<WAVEFORMATEX*>(CoTaskMemAlloc(sizeof(WAVEFORMATEX)));
	if (format == nullptr) {
		return E_OUTOFMEMORY;
	}
	format->wFormatTag = WAVE_FORMAT_PCM;
	format->nChannels = kChannels;
	format->nSamplesPerSec = kSampleRate;
	format->wBitsPerSample = kBitsPerSample;
	format->nBlockAlign = kChannels * kBytesPerSample;
	format->nAvgBytesPerSec = kSampleRate * format->nBlockAlign;
	format->cbSize = 0;

	*pOutputFormatId = SPDFID_WaveFormatEx;
	*ppCoMemOutputWaveFormatEx = format;
	return S_OK;
}

bool PicoTTSEngine::CheckActions(SpeakContext& context) {
	const DWORD actions = context.site->GetActions();
	if ((actions & SPVES_ABORT) != 0) {
		context.aborted = true;
		return false;
	}
	if ((actions & SPVES_RATE) != 0) {
		long rate = 0;
		if (SUCCEEDED(context.site->GetRate(&rate))) {
			context.sapiRate = rate;
		}
	}
	if ((actions & SPVES_VOLUME) != 0) {
		USHORT volume = 100;
		if (SUCCEEDED(context.site->GetVolume(&volume))) {
			// Applied to the samples as they are written, so a change reaches the
			// listener within the next block rather than the next utterance.
			context.gain = GainFromSapiVolume(volume);
		}
	}
	if ((actions & SPVES_SKIP) != 0) {
		SPVSKIPTYPE skipType = SPVST_SENTENCE;
		long count = 0;
		if (SUCCEEDED(context.site->GetSkipInfo(&skipType, &count))) {
			if (skipType == SPVST_SENTENCE && count > 0) {
				context.sentencesToSkip = count;
				// Whatever pico is holding belongs to the sentence being
				// abandoned, so it must not be spoken once output resumes.
				engine_.Reset();
				context.site->CompleteSkip(count);
			} else {
				// Everything already written has been played, so there is nothing
				// to rewind to. Reporting zero is how an engine says so.
				context.site->CompleteSkip(0);
			}
		}
	}
	return true;
}

HRESULT PicoTTSEngine::WriteAudio(std::vector<int16_t>& samples, SpeakContext& context) {
	if (samples.empty()) {
		return S_OK;
	}
	ApplyGain(samples.data(), samples.size(), context.gain);
	ULONG written = 0;
	const HRESULT hr = context.site->Write(samples.data(),
										   static_cast<ULONG>(samples.size() * sizeof(int16_t)),
										   &written);
	context.audioOffset += written;
	samples.clear();
	return hr;
}

void PicoTTSEngine::EmitWordEvents(const std::vector<PendingWord>& words, size_t sampleCount,
								   SpeakContext& context) {
	if (words.empty() || sampleCount == 0) {
		return;
	}
	const bool wantsWord = WantsEvent(context.eventInterest, SPEI_WORD_BOUNDARY);
	const bool wantsSentence = WantsEvent(context.eventInterest, SPEI_SENTENCE_BOUNDARY);
	if (!wantsWord && !wantsSentence) {
		return;
	}

	size_t totalWeight = 0;
	for (const PendingWord& word : words) {
		totalWeight += word.weight;
	}
	if (totalWeight == 0) {
		return;
	}

	std::vector<SPEVENT> events;
	events.reserve(words.size() * 2);
	size_t consumedWeight = 0;
	for (const PendingWord& word : words) {
		// Where this word starts, as a share of the audio the sentence produced.
		// Pico gives no marks of its own, so within a sentence this is an
		// estimate; it is exact at the sentence boundaries, which is where the
		// error would otherwise accumulate.
		const size_t sampleOffset = sampleCount * consumedWeight / totalWeight;
		const ULONGLONG byteOffset =
			context.audioOffset + static_cast<ULONGLONG>(sampleOffset) * sizeof(int16_t);
		consumedWeight += word.weight;

		if (wantsSentence && word.startsSentence) {
			SPEVENT event = {};
			event.eEventId = SPEI_SENTENCE_BOUNDARY;
			event.elParamType = SPET_LPARAM_IS_UNDEFINED;
			event.ullAudioStreamOffset = byteOffset;
			event.lParam = static_cast<LPARAM>(word.sourceOffset);
			event.wParam = static_cast<WPARAM>(word.sourceLength);
			events.push_back(event);
		}
		if (wantsWord) {
			SPEVENT event = {};
			event.eEventId = SPEI_WORD_BOUNDARY;
			event.elParamType = SPET_LPARAM_IS_UNDEFINED;
			event.ullAudioStreamOffset = byteOffset;
			event.lParam = static_cast<LPARAM>(word.sourceOffset);
			event.wParam = static_cast<WPARAM>(word.sourceLength);
			events.push_back(event);
		}
	}
	if (!events.empty()) {
		context.site->AddEvents(events.data(), static_cast<ULONG>(events.size()));
	}
}

HRESULT PicoTTSEngine::WriteSilence(ULONG milliseconds, SpeakContext& context) {
	const ULONG capped = std::min(milliseconds, kMaxSilenceMs);
	size_t remaining = static_cast<size_t>(kSampleRate) * capped / 1000;
	// Generated here rather than asked of pico: <break> would have to be spliced
	// into the markup, and zero samples are both exact and free.
	std::vector<int16_t> block(std::min(remaining, kSilenceBlockSamples), 0);
	while (remaining > 0) {
		if (!CheckActions(context)) {
			return S_OK;
		}
		const size_t thisBlock = std::min(remaining, block.size());
		ULONG written = 0;
		const HRESULT hr = context.site->Write(block.data(),
											   static_cast<ULONG>(thisBlock * sizeof(int16_t)),
											   &written);
		if (FAILED(hr)) {
			return hr;
		}
		context.audioOffset += written;
		remaining -= thisBlock;
	}
	return S_OK;
}

HRESULT PicoTTSEngine::WriteBookmark(const SPVTEXTFRAG& fragment, SpeakContext& context) {
	if (!WantsEvent(context.eventInterest, SPEI_TTS_BOOKMARK)) {
		return S_OK;
	}
	// The fragment's text is the bookmark's name. It has to outlive AddEvents,
	// which copies it, so a local string is enough.
	const std::wstring name(fragment.pTextStart, fragment.ulTextLen);
	SPEVENT event = {};
	event.eEventId = SPEI_TTS_BOOKMARK;
	event.elParamType = SPET_LPARAM_IS_STRING;
	event.ullAudioStreamOffset = context.audioOffset;
	event.lParam = reinterpret_cast<LPARAM>(name.c_str());
	// SAPI also reports the name read as a number, for applications that use
	// numeric bookmarks.
	event.wParam = static_cast<WPARAM>(_wtol(name.c_str()));
	context.site->AddEvents(&event, 1);
	return S_OK;
}

HRESULT PicoTTSEngine::SpeakFragment(const SPVTEXTFRAG& fragment, SpeakContext& context) {
	std::wstring text(fragment.pTextStart, fragment.ulTextLen);
	const bool spellOut = fragment.State.eAction == SPVA_SpellOut;
	if (spellOut) {
		text = SpellOutText(text);
	}
	// Escaping substitutes one character for another, so offsets into the result
	// still index the text SAPI gave us and can be reported as they are.
	const std::wstring escaped = EscapeTextW(text);
	const std::vector<WordSpan> spans = SplitWords(escaped);
	if (spans.empty()) {
		return S_OK;
	}

	// Audio arrives from pico in small pieces; it is gathered here until a
	// sentence's worth is complete, at which point the words fed since the last
	// time any arrived are spread across it.
	std::vector<int16_t> collected;
	auto sink = [&collected, &context](const int16_t* samples, size_t count) {
		// Also checked here, not only between words: pico releases a whole
		// sentence at once, so a check that only ran between words would not
		// notice an abort until the sentence it is in the middle of had been
		// synthesised in full.
		if ((context.site->GetActions() & SPVES_ABORT) != 0) {
			context.aborted = true;
			return false;
		}
		collected.insert(collected.end(), samples, samples + count);
		return true;
	};

	std::vector<PendingWord> pending;
	std::string openTags;
	std::string closeTags;
	Prosody current;  // What the tags currently in effect say.

	auto flushCollected = [&]() -> HRESULT {
		if (collected.empty()) {
			return S_OK;
		}
		EmitWordEvents(pending, collected.size(), context);
		pending.clear();
		return WriteAudio(collected, context);
	};

	const std::string utf8 = ToUtf8(escaped);
	// Spans are expressed in wide characters but pico is fed bytes, so the two
	// are related once here rather than re-encoding a substring per word.
	const std::vector<size_t> byteOffsets = Utf16ToUtf8Offsets(escaped);

	HRESULT hr = S_OK;
	for (const WordSpan& span : spans) {
		if (!CheckActions(context)) {
			break;
		}
		if (context.sentencesToSkip > 0) {
			// Words are dropped until the requested number of sentence openings
			// have gone by, and the one that lands on zero is where speaking
			// resumes. Nothing is fed to pico meanwhile, so skipped text costs
			// no synthesis at all.
			pending.clear();
			collected.clear();
			if (span.startsSentence) {
				--context.sentencesToSkip;
			}
			if (context.sentencesToSkip > 0) {
				continue;
			}
		}

		// Rate and pitch live in the text, so a change is applied by closing the
		// tags in force and opening new ones between words.
		Prosody desired;
		desired.speed = SpeedFromSapiRate(context.sapiRate + fragment.State.RateAdj);
		desired.pitch = PitchFromSapiPitch(fragment.State.PitchAdj.MiddleAdj);
		if (!(desired == current)) {
			if (!closeTags.empty()) {
				hr = engine_.PutText(closeTags.data(), closeTags.size(), sink);
				if (FAILED(hr)) {
					return hr;
				}
			}
			const std::string wrapped = WrapProsody(std::string(), desired);
			// WrapProsody puts the opening tags before the (empty) body and the
			// closing ones after, so splitting it in two gives both halves.
			const size_t split = wrapped.find("</");
			openTags = split == std::string::npos ? wrapped : wrapped.substr(0, split);
			closeTags = split == std::string::npos ? std::string() : wrapped.substr(split);
			current = desired;
			if (!openTags.empty()) {
				hr = engine_.PutText(openTags.data(), openTags.size(), sink);
				if (FAILED(hr)) {
					return hr;
				}
			}
		}

		PendingWord word = {};
		word.sourceOffset = spellOut ? fragment.ulTextSrcOffset
									 : static_cast<ULONG>(fragment.ulTextSrcOffset + span.wordBegin);
		word.sourceLength = spellOut ? fragment.ulTextLen
									 : static_cast<ULONG>(span.wordEnd - span.wordBegin);
		word.weight = std::max<size_t>(1, span.wordEnd - span.wordBegin);
		word.startsSentence = span.startsSentence;
		pending.push_back(word);

		const size_t begin = byteOffsets[span.feedBegin];
		const size_t end = byteOffsets[span.feedEnd];
		hr = engine_.PutText(utf8.data() + begin, end - begin, sink);
		if (FAILED(hr)) {
			return hr;
		}
		if (context.aborted) {
			// The half-synthesised sentence in `collected` belongs to an
			// utterance nobody is listening to any more.
			break;
		}
		// Pico releases a sentence only once it is complete, so audio appearing
		// here means the words gathered since the last one are now accounted for.
		hr = flushCollected();
		if (FAILED(hr)) {
			return hr;
		}
	}

	if (!context.aborted && !closeTags.empty()) {
		hr = engine_.PutText(closeTags.data(), closeTags.size(), sink);
		if (FAILED(hr)) {
			return hr;
		}
	}
	if (!context.aborted) {
		// Releases the sentence pico is still holding.
		hr = engine_.Flush(sink);
		if (FAILED(hr)) {
			return hr;
		}
		hr = flushCollected();
		if (FAILED(hr)) {
			return hr;
		}
	} else {
		engine_.Reset();
	}
	return S_OK;
}

STDMETHODIMP PicoTTSEngine::Speak(DWORD /*dwSpeakFlags*/, REFGUID /*rguidFormatId*/,
								  const WAVEFORMATEX* /*pWaveFormatEx*/,
								  const SPVTEXTFRAG* pTextFragList,
								  ISpTTSEngineSite* pOutputSite) {
	if (pOutputSite == nullptr) {
		return E_INVALIDARG;
	}
	if (pTextFragList == nullptr) {
		return S_OK;
	}

	std::lock_guard<std::mutex> lock(mutex_);
	HRESULT hr = EnsureVoiceOpen();
	if (FAILED(hr)) {
		return hr;
	}

	SpeakContext context;
	context.site = pOutputSite;
	if (FAILED(pOutputSite->GetEventInterest(&context.eventInterest))) {
		context.eventInterest = 0;
	}
	if (FAILED(pOutputSite->GetRate(&context.sapiRate))) {
		context.sapiRate = 0;
	}
	USHORT volume = 100;
	if (SUCCEEDED(pOutputSite->GetVolume(&volume))) {
		context.gain = GainFromSapiVolume(volume);
	}

	for (const SPVTEXTFRAG* fragment = pTextFragList; fragment != nullptr;
		 fragment = fragment->pNext) {
		if (!CheckActions(context)) {
			break;
		}
		switch (fragment->State.eAction) {
			case SPVA_Speak:
			case SPVA_SpellOut:
			// Pico takes phonemes in SAMPA and SAPI supplies them in its own
			// universal phone set; no mapping between the two is attempted, so
			// the fragment's text is spoken instead. That is what SAPI asks for
			// when an engine cannot honour the pronunciation.
			case SPVA_Pronounce:
				hr = SpeakFragment(*fragment, context);
				break;
			case SPVA_Silence:
				hr = WriteSilence(fragment->State.SilenceMSecs, context);
				break;
			case SPVA_Bookmark:
				hr = WriteBookmark(*fragment, context);
				break;
			case SPVA_ParseUnknownTag:
				// A tag no one claimed. Saying its contents would be worse than
				// staying quiet.
				hr = S_OK;
				break;
			default:
				hr = S_OK;
				break;
		}
		if (FAILED(hr)) {
			PICO_LOG_ERROR("fragment failed: %s", FormatHResult(hr).c_str());
			return hr;
		}
		if (context.aborted) {
			break;
		}
	}
	return S_OK;
}

}  // namespace pico
