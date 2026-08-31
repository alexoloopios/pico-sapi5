// Drives the engine DLL through its COM class object without registering it.
//
// This is the middle of the test pyramid. It exercises the whole ISpTTSEngine
// contract -- fragments, prosody, events, aborting, skipping -- against a site
// written here, so the SAPI layer can be checked on a machine where nobody has
// administrative rights and without disturbing the voices already installed.
//
//   pico_sapitest --dll ..\PicoSAPI5.dll
//   pico_sapitest --dll ..\PicoSAPI5.dll --voice de-DE --text "Guten Tag."

#include <windows.h>

#include <sapi.h>
#include <sapiddk.h>
#include <sperror.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cwctype>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void Check(bool condition, const char* what) {
	std::printf("  [%s] %s\n", condition ? "pass" : "FAIL", what);
	if (!condition) {
		++g_failures;
	}
}

//: A recorded event, kept so the ordering and offsets can be asserted after the
//: call rather than while it is running.
struct RecordedEvent {
	SPEVENTENUM id;
	ULONGLONG audioOffset;
	LPARAM lParam;
	WPARAM wParam;
	std::wstring text;
};

// ---------------------------------------------------------------------------
// A minimal ISpTTSEngineSite. It collects the audio and the events, and can be
// told to abort or to change rate and volume part way through, which is how the
// engine's response to each of those is checked.
// ---------------------------------------------------------------------------
class TestSite : public ISpTTSEngineSite {
public:
	std::vector<int16_t> audio;
	std::vector<RecordedEvent> events;
	ULONGLONG eventInterest = SPFEI(SPEI_WORD_BOUNDARY) | SPFEI(SPEI_SENTENCE_BOUNDARY) |
							  SPFEI(SPEI_TTS_BOOKMARK);
	long rate = 0;
	USHORT volume = 100;

	//: Abort once this many bytes have been written. Zero never aborts.
	ULONG abortAfterBytes = 0;
	//: Raise these once, the next time the engine asks what it should be doing.
	bool pendingRateChange = false;
	bool pendingVolumeChange = false;
	//: Sentences to report for a skip, requested once.
	long pendingSkip = 0;

	STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
		if (ppv == nullptr) {
			return E_POINTER;
		}
		if (riid == IID_IUnknown || riid == IID_ISpTTSEngineSite || riid == IID_ISpEventSink) {
			*ppv = static_cast<ISpTTSEngineSite*>(this);
			AddRef();
			return S_OK;
		}
		*ppv = nullptr;
		return E_NOINTERFACE;
	}
	STDMETHODIMP_(ULONG) AddRef() override { return 2; }
	STDMETHODIMP_(ULONG) Release() override { return 1; }

	// ISpEventSink
	STDMETHODIMP AddEvents(const SPEVENT* pEventArray, ULONG ulCount) override {
		for (ULONG i = 0; i < ulCount; ++i) {
			RecordedEvent recorded = {};
			recorded.id = static_cast<SPEVENTENUM>(pEventArray[i].eEventId);
			recorded.audioOffset = pEventArray[i].ullAudioStreamOffset;
			recorded.lParam = pEventArray[i].lParam;
			recorded.wParam = pEventArray[i].wParam;
			if (pEventArray[i].elParamType == SPET_LPARAM_IS_STRING &&
				pEventArray[i].lParam != 0) {
				recorded.text = reinterpret_cast<const wchar_t*>(pEventArray[i].lParam);
			}
			events.push_back(recorded);
		}
		return S_OK;
	}
	STDMETHODIMP GetEventInterest(ULONGLONG* pullEventInterest) override {
		*pullEventInterest = eventInterest;
		return S_OK;
	}

	// ISpTTSEngineSite
	STDMETHODIMP_(DWORD) GetActions() override {
		DWORD actions = SPVES_CONTINUE;
		if (abortAfterBytes != 0 && audio.size() * sizeof(int16_t) >= abortAfterBytes) {
			actions |= SPVES_ABORT;
		}
		if (pendingRateChange) {
			actions |= SPVES_RATE;
		}
		if (pendingVolumeChange) {
			actions |= SPVES_VOLUME;
		}
		if (pendingSkip != 0) {
			actions |= SPVES_SKIP;
		}
		return actions;
	}
	STDMETHODIMP Write(const void* pBuff, ULONG cb, ULONG* pcbWritten) override {
		const int16_t* samples = static_cast<const int16_t*>(pBuff);
		audio.insert(audio.end(), samples, samples + cb / sizeof(int16_t));
		if (pcbWritten != nullptr) {
			*pcbWritten = cb;
		}
		return S_OK;
	}
	STDMETHODIMP GetRate(long* pRateAdjust) override {
		pendingRateChange = false;
		*pRateAdjust = rate;
		return S_OK;
	}
	STDMETHODIMP GetVolume(USHORT* pusVolume) override {
		pendingVolumeChange = false;
		*pusVolume = volume;
		return S_OK;
	}
	STDMETHODIMP GetSkipInfo(SPVSKIPTYPE* peType, long* plNumItems) override {
		*peType = SPVST_SENTENCE;
		*plNumItems = pendingSkip;
		return S_OK;
	}
	STDMETHODIMP CompleteSkip(long /*ulNumSkipped*/) override {
		pendingSkip = 0;
		return S_OK;
	}
};

// ---------------------------------------------------------------------------
// Just enough of ISpObjectToken to carry the one value the engine reads from it.
// Everything the engine does not call is left unimplemented on purpose, so that
// a future call to any of it shows up as a failure rather than as silence.
// ---------------------------------------------------------------------------
class TestToken : public ISpObjectToken {
public:
	explicit TestToken(std::wstring voiceId) : voiceId_(std::move(voiceId)) {}

	STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
		if (ppv == nullptr) {
			return E_POINTER;
		}
		if (riid == IID_IUnknown || riid == IID_ISpObjectToken || riid == IID_ISpDataKey) {
			*ppv = static_cast<ISpObjectToken*>(this);
			AddRef();
			return S_OK;
		}
		*ppv = nullptr;
		return E_NOINTERFACE;
	}
	STDMETHODIMP_(ULONG) AddRef() override { return 2; }
	STDMETHODIMP_(ULONG) Release() override { return 1; }

	// ISpDataKey -- only the string read matters here.
	STDMETHODIMP GetStringValue(LPCWSTR pszValueName, LPWSTR* ppszValue) override {
		if (pszValueName == nullptr || _wcsicmp(pszValueName, L"PicoVoice") != 0) {
			return SPERR_NOT_FOUND;
		}
		const size_t bytes = (voiceId_.size() + 1) * sizeof(wchar_t);
		*ppszValue = static_cast<LPWSTR>(CoTaskMemAlloc(bytes));
		if (*ppszValue == nullptr) {
			return E_OUTOFMEMORY;
		}
		memcpy(*ppszValue, voiceId_.c_str(), bytes);
		return S_OK;
	}
	STDMETHODIMP OpenKey(LPCWSTR, ISpDataKey**) override { return SPERR_NOT_FOUND; }

	STDMETHODIMP SetData(LPCWSTR, ULONG, const BYTE*) override { return E_NOTIMPL; }
	STDMETHODIMP GetData(LPCWSTR, ULONG*, BYTE*) override { return E_NOTIMPL; }
	STDMETHODIMP SetStringValue(LPCWSTR, LPCWSTR) override { return E_NOTIMPL; }
	STDMETHODIMP SetDWORD(LPCWSTR, DWORD) override { return E_NOTIMPL; }
	STDMETHODIMP GetDWORD(LPCWSTR, DWORD*) override { return E_NOTIMPL; }
	STDMETHODIMP CreateKey(LPCWSTR, ISpDataKey**) override { return E_NOTIMPL; }
	STDMETHODIMP DeleteKey(LPCWSTR) override { return E_NOTIMPL; }
	STDMETHODIMP DeleteValue(LPCWSTR) override { return E_NOTIMPL; }
	STDMETHODIMP EnumKeys(ULONG, LPWSTR*) override { return E_NOTIMPL; }
	STDMETHODIMP EnumValues(ULONG, LPWSTR*) override { return E_NOTIMPL; }

	// ISpObjectToken
	STDMETHODIMP SetId(LPCWSTR, LPCWSTR, BOOL) override { return E_NOTIMPL; }
	STDMETHODIMP GetId(LPWSTR*) override { return E_NOTIMPL; }
	STDMETHODIMP GetCategory(ISpObjectTokenCategory**) override { return E_NOTIMPL; }
	STDMETHODIMP CreateInstance(IUnknown*, DWORD, REFIID, void**) override { return E_NOTIMPL; }
	STDMETHODIMP GetStorageFileName(REFCLSID, LPCWSTR, LPCWSTR, ULONG, LPWSTR*) override {
		return E_NOTIMPL;
	}
	STDMETHODIMP RemoveStorageFileName(REFCLSID, LPCWSTR, BOOL) override { return E_NOTIMPL; }
	STDMETHODIMP Remove(const CLSID*) override { return E_NOTIMPL; }
	STDMETHODIMP IsUISupported(LPCWSTR, void*, ULONG, IUnknown*, BOOL*) override {
		return E_NOTIMPL;
	}
	STDMETHODIMP DisplayUI(HWND, LPCWSTR, LPCWSTR, void*, ULONG, IUnknown*) override {
		return E_NOTIMPL;
	}
	STDMETHODIMP MatchesAttributes(LPCWSTR, BOOL*) override { return E_NOTIMPL; }

private:
	std::wstring voiceId_;
};

//: Builds a one-fragment list. The caller owns the text.
SPVTEXTFRAG MakeFragment(const wchar_t* text, size_t length, SPVACTIONS action = SPVA_Speak) {
	SPVTEXTFRAG fragment = {};
	fragment.pNext = nullptr;
	fragment.pTextStart = text;
	fragment.ulTextLen = static_cast<ULONG>(length);
	fragment.ulTextSrcOffset = 0;
	fragment.State.eAction = action;
	fragment.State.LangID = 0x409;
	fragment.State.Volume = 100;
	return fragment;
}

bool WriteWav(const std::wstring& path, const std::vector<int16_t>& samples) {
	FILE* file = nullptr;
	if (_wfopen_s(&file, path.c_str(), L"wb") != 0 || file == nullptr) {
		return false;
	}
	const uint32_t dataBytes = static_cast<uint32_t>(samples.size() * sizeof(int16_t));
	auto put32 = [file](uint32_t v) { std::fwrite(&v, 4, 1, file); };
	auto put16 = [file](uint16_t v) { std::fwrite(&v, 2, 1, file); };
	std::fwrite("RIFF", 1, 4, file);
	put32(36 + dataBytes);
	std::fwrite("WAVEfmt ", 1, 8, file);
	put32(16);
	put16(1);
	put16(1);
	put32(16000);
	put32(32000);
	put16(2);
	put16(16);
	std::fwrite("data", 1, 4, file);
	put32(dataBytes);
	std::fwrite(samples.data(), 1, dataBytes, file);
	std::fclose(file);
	return true;
}

const char* EventName(SPEVENTENUM id) {
	switch (id) {
		case SPEI_WORD_BOUNDARY: return "word";
		case SPEI_SENTENCE_BOUNDARY: return "sentence";
		case SPEI_TTS_BOOKMARK: return "bookmark";
		default: return "other";
	}
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
	std::wstring dllPath = L"PicoSAPI5.dll";
	std::wstring voiceId = L"en-US";
	std::wstring text = L"The first sentence is here. The second one follows it.";

	for (int i = 1; i < argc; ++i) {
		const std::wstring argument = argv[i];
		const bool hasValue = i + 1 < argc;
		if (argument == L"--dll" && hasValue) {
			dllPath = argv[++i];
		} else if (argument == L"--voice" && hasValue) {
			voiceId = argv[++i];
		} else if (argument == L"--text" && hasValue) {
			text = argv[++i];
		}
	}

	HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	if (FAILED(hr)) {
		std::fwprintf(stderr, L"CoInitializeEx failed\n");
		return 1;
	}

	HMODULE module = LoadLibraryW(dllPath.c_str());
	if (module == nullptr) {
		std::fwprintf(stderr, L"could not load %ls (error %lu)\n", dllPath.c_str(), GetLastError());
		return 1;
	}
	using GetClassObjectFn = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**);
	auto getClassObject =
		reinterpret_cast<GetClassObjectFn>(GetProcAddress(module, "DllGetClassObject"));
	if (getClassObject == nullptr) {
		std::fwprintf(stderr, L"the DLL exports no DllGetClassObject\n");
		return 1;
	}

	// The class id is the one the engine declares; asking the DLL for its own
	// class object is what makes this work without the class being registered.
	const CLSID clsid = {0x8C240E79,
						 0x2921,
						 0x486E,
						 {0x84, 0xBD, 0x17, 0x6E, 0x35, 0xAE, 0xB5, 0x05}};

	IClassFactory* factory = nullptr;
	hr = getClassObject(clsid, IID_IClassFactory, reinterpret_cast<void**>(&factory));
	if (FAILED(hr) || factory == nullptr) {
		std::fwprintf(stderr, L"DllGetClassObject failed: 0x%08lX\n", hr);
		return 1;
	}

	ISpTTSEngine* engine = nullptr;
	hr = factory->CreateInstance(nullptr, IID_ISpTTSEngine, reinterpret_cast<void**>(&engine));
	factory->Release();
	if (FAILED(hr) || engine == nullptr) {
		std::fwprintf(stderr, L"CreateInstance failed: 0x%08lX\n", hr);
		return 1;
	}

	std::printf("== output format ==\n");
	{
		GUID formatId = {};
		WAVEFORMATEX* format = nullptr;
		hr = engine->GetOutputFormat(nullptr, nullptr, &formatId, &format);
		Check(SUCCEEDED(hr) && format != nullptr, "GetOutputFormat succeeds");
		if (format != nullptr) {
			std::printf("     %lu Hz, %u bit, %u channel\n", format->nSamplesPerSec,
						format->wBitsPerSample, format->nChannels);
			Check(format->nSamplesPerSec == 16000, "reports pico's 16 kHz rate");
			Check(format->wBitsPerSample == 16 && format->nChannels == 1, "16 bit mono");
			Check(formatId == SPDFID_WaveFormatEx, "reports SPDFID_WaveFormatEx");
			CoTaskMemFree(format);
		}
	}

	std::printf("== voice selection through the token ==\n");
	{
		ISpObjectWithToken* withToken = nullptr;
		hr = engine->QueryInterface(IID_ISpObjectWithToken, reinterpret_cast<void**>(&withToken));
		Check(SUCCEEDED(hr) && withToken != nullptr, "engine offers ISpObjectWithToken");
		if (withToken != nullptr) {
			TestToken token(voiceId);
			Check(SUCCEEDED(withToken->SetObjectToken(&token)), "SetObjectToken accepts the token");
			ISpObjectToken* readBack = nullptr;
			Check(withToken->GetObjectToken(&readBack) == S_OK && readBack == &token,
				  "GetObjectToken returns what was set");
			withToken->Release();
		}
	}

	std::printf("== speaking, with events ==\n");
	{
		TestSite site;
		SPVTEXTFRAG fragment = MakeFragment(text.c_str(), text.size());
		hr = engine->Speak(0, GUID_NULL, nullptr, &fragment, &site);
		Check(SUCCEEDED(hr), "Speak succeeds");
		Check(!site.audio.empty(), "audio was produced");
		std::printf("     %zu samples (%.2f s), %zu events\n", site.audio.size(),
					site.audio.size() / 16000.0, site.events.size());

		const ULONGLONG totalBytes = site.audio.size() * sizeof(int16_t);
		bool monotonic = true;
		bool inRange = true;
		int words = 0;
		int sentences = 0;
		ULONGLONG previous = 0;
		for (const RecordedEvent& event : site.events) {
			if (event.audioOffset < previous) {
				monotonic = false;
			}
			previous = event.audioOffset;
			if (event.audioOffset > totalBytes) {
				inRange = false;
			}
			if (event.id == SPEI_WORD_BOUNDARY) {
				++words;
			}
			if (event.id == SPEI_SENTENCE_BOUNDARY) {
				++sentences;
			}
		}
		// Counted from the text rather than written down, so the check still means
		// something when --text is used.
		int expectedWords = 0;
		bool inWord = false;
		for (const wchar_t character : text) {
			const bool isSpace = iswspace(character) != 0;
			if (!isSpace && !inWord) {
				++expectedWords;
			}
			inWord = !isSpace;
		}
		std::printf("     %d word boundaries for %d words, %d sentence boundaries\n", words,
					expectedWords, sentences);
		Check(words == expectedWords, "one word boundary per word");
		Check(sentences >= 1, "at least one sentence boundary");
		Check(monotonic, "event offsets never go backwards");
		Check(inRange, "event offsets stay inside the audio written");

		for (const RecordedEvent& event : site.events) {
			if (event.id != SPEI_SENTENCE_BOUNDARY) {
				continue;
			}
			std::printf("     sentence at %.2f s, source offset %lld\n",
						event.audioOffset / 32000.0, static_cast<long long>(event.lParam));
		}
		WriteWav(L"sapitest_speak.wav", site.audio);
	}

	std::printf("== rate and volume ==\n");
	{
		TestSite fast;
		fast.rate = 10;
		SPVTEXTFRAG fragment = MakeFragment(text.c_str(), text.size());
		engine->Speak(0, GUID_NULL, nullptr, &fragment, &fast);

		TestSite slow;
		slow.rate = -10;
		engine->Speak(0, GUID_NULL, nullptr, &fragment, &slow);

		std::printf("     rate +10: %.2f s, rate -10: %.2f s\n", fast.audio.size() / 16000.0,
					slow.audio.size() / 16000.0);
		Check(fast.audio.size() < slow.audio.size() / 2, "rate +10 is far shorter than rate -10");

		TestSite quiet;
		quiet.volume = 20;
		engine->Speak(0, GUID_NULL, nullptr, &fragment, &quiet);
		TestSite loud;
		loud.volume = 100;
		engine->Speak(0, GUID_NULL, nullptr, &fragment, &loud);
		int16_t quietPeak = 0;
		int16_t loudPeak = 0;
		for (const int16_t sample : quiet.audio) {
			quietPeak = std::max(quietPeak, static_cast<int16_t>(std::abs(static_cast<int>(sample))));
		}
		for (const int16_t sample : loud.audio) {
			loudPeak = std::max(loudPeak, static_cast<int16_t>(std::abs(static_cast<int>(sample))));
		}
		std::printf("     peak at volume 20: %d, at volume 100: %d\n", quietPeak, loudPeak);
		Check(quietPeak < loudPeak / 2, "volume 20 is much quieter than volume 100");
	}

	std::printf("== pitch ==\n");
	{
		TestSite high;
		SPVTEXTFRAG fragment = MakeFragment(text.c_str(), text.size());
		fragment.State.PitchAdj.MiddleAdj = 10;
		engine->Speak(0, GUID_NULL, nullptr, &fragment, &high);
		Check(!high.audio.empty(), "pitch +10 still produces audio");
		WriteWav(L"sapitest_pitch_high.wav", high.audio);

		TestSite low;
		fragment.State.PitchAdj.MiddleAdj = -10;
		engine->Speak(0, GUID_NULL, nullptr, &fragment, &low);
		Check(!low.audio.empty(), "pitch -10 still produces audio");
		WriteWav(L"sapitest_pitch_low.wav", low.audio);
	}

	std::printf("== aborting ==\n");
	{
		SPVTEXTFRAG fragment = MakeFragment(text.c_str(), text.size());

		// What the same text produces when it is allowed to finish, so the
		// comparison below does not depend on which voice is being tested.
		TestSite full;
		engine->Speak(0, GUID_NULL, nullptr, &fragment, &full);

		TestSite site;
		site.abortAfterBytes = 8000;  // a quarter of a second
		hr = engine->Speak(0, GUID_NULL, nullptr, &fragment, &site);
		Check(SUCCEEDED(hr), "Speak returns success after an abort");
		std::printf("     stopped after %.2f s of a %.2f s utterance\n",
					site.audio.size() / 16000.0, full.audio.size() / 16000.0);
		Check(site.audio.size() < full.audio.size() * 3 / 4, "abort stopped the utterance early");

		// The engine must be usable straight afterwards, with nothing left over
		// from the abandoned utterance.
		TestSite after;
		engine->Speak(0, GUID_NULL, nullptr, &fragment, &after);
		std::printf("     next utterance: %.2f s\n", after.audio.size() / 16000.0);
		Check(after.audio.size() == full.audio.size(),
			  "the engine recovers and speaks the whole thing again");
	}

	std::printf("== bookmarks and silence ==\n");
	{
		TestSite site;
		const std::wstring first = L"Before the mark.";
		const std::wstring mark = L"42";
		const std::wstring second = L"After the mark.";

		SPVTEXTFRAG fragments[3] = {};
		fragments[0] = MakeFragment(first.c_str(), first.size());
		fragments[1] = MakeFragment(mark.c_str(), mark.size(), SPVA_Bookmark);
		fragments[2] = MakeFragment(second.c_str(), second.size());
		fragments[0].pNext = &fragments[1];
		fragments[1].pNext = &fragments[2];

		hr = engine->Speak(0, GUID_NULL, nullptr, fragments, &site);
		Check(SUCCEEDED(hr), "a fragment list with a bookmark succeeds");
		bool sawBookmark = false;
		for (const RecordedEvent& event : site.events) {
			if (event.id == SPEI_TTS_BOOKMARK) {
				sawBookmark = true;
				Check(event.text == L"42", "the bookmark reports its name");
				Check(event.wParam == 42, "the bookmark reports its numeric value");
				Check(event.audioOffset > 0 && event.audioOffset < site.audio.size() * 2,
					  "the bookmark falls between the two fragments");
			}
		}
		Check(sawBookmark, "a bookmark event was raised");

		TestSite silent;
		SPVTEXTFRAG silence = MakeFragment(L"", 0, SPVA_Silence);
		silence.State.SilenceMSecs = 500;
		hr = engine->Speak(0, GUID_NULL, nullptr, &silence, &silent);
		Check(SUCCEEDED(hr), "a silence fragment succeeds");
		Check(silent.audio.size() == 8000, "500 ms of silence is 8000 samples");
		bool allZero = true;
		for (const int16_t sample : silent.audio) {
			if (sample != 0) {
				allZero = false;
			}
		}
		Check(allZero, "the silence really is silent");
	}

	std::printf("== switching voices repeatedly ==\n");
	{
		// Each voice holds two pico resources and a voice definition, and pico's
		// resource table has room for 64. A switch that failed to release the
		// previous voice would therefore keep working for about thirty changes
		// and then stop -- long after anyone would connect the two.
		ISpObjectWithToken* withToken = nullptr;
		hr = engine->QueryInterface(IID_ISpObjectWithToken, reinterpret_cast<void**>(&withToken));
		if (SUCCEEDED(hr) && withToken != nullptr) {
			const wchar_t* const voices[] = {L"en-US", L"de-DE", L"fr-FR", L"it-IT",
											 L"es-ES", L"en-GB"};
			const std::wstring shortText = L"One two three.";

			// Every ordered pair, and the outgoing voice is spoken with before the
			// switch rather than merely loaded. Both parts matter. The voices
			// differ in size by more than half a megabyte, and the failure this
			// guards against -- pico's arena too fragmented to load the next
			// voice -- needs a larger voice to follow a smaller one that has
			// actually been used, because it is the working allocations of
			// synthesis, not the lingware alone, that leave the holes. A cycle of
			// bare loads misses it entirely.
			auto speakWith = [&](const wchar_t* voice, const std::wstring& what) -> bool {
				TestToken token(voice);
				if (FAILED(withToken->SetObjectToken(&token))) {
					return false;
				}
				TestSite site;
				SPVTEXTFRAG fragment = MakeFragment(what.c_str(), what.size());
				return SUCCEEDED(engine->Speak(0, GUID_NULL, nullptr, &fragment, &site)) &&
					   !site.audio.empty();
			};

			int switches = 0;
			int failed = 0;
			for (const wchar_t* from : voices) {
				for (const wchar_t* to : voices) {
					if (from == to) {
						continue;
					}
					speakWith(from, text);
					++switches;
					if (!speakWith(to, shortText)) {
						std::printf("     silent after switching %ls -> %ls\n", from, to);
						++failed;
					}
				}
			}
			std::printf("     %d voice changes, %d silent\n", switches, failed);
			Check(failed == 0, "every voice still speaks after switching from every other");

			// Leave the engine on the voice the rest of the run expects.
			TestToken restore(voiceId);
			withToken->SetObjectToken(&restore);
			withToken->Release();
		}
	}

	std::printf("== pico markup arriving as text ==\n");
	{
		// Text handed to a SAPI engine is not necessarily trustworthy: a screen
		// reader reading a page passes on whatever is there. Pico takes its
		// prosody from tags in the text, so a tag that survived to the engine
		// would change the listener's settings, and <spell> faults it outright.
		auto speakFor = [&](const wchar_t* what) -> size_t {
			TestSite site;
			const std::wstring owned = what;
			SPVTEXTFRAG fragment = MakeFragment(owned.c_str(), owned.size());
			engine->Speak(0, GUID_NULL, nullptr, &fragment, &site);
			return site.audio.size();
		};

		// Spoken as words, the tag and the plain text take about the same time.
		// Obeyed, the tag would make the utterance roughly five times longer.
		const size_t control = speakFor(L"alpha speed level 20 bravo charlie delta");
		const size_t injected = speakFor(L"alpha <speed level=\"20\"> bravo charlie delta");
		std::printf("     control %.2f s, with a speed tag in the text %.2f s\n",
					control / 16000.0, injected / 16000.0);
		Check(injected < control * 2, "an embedded speed tag does not change the rate");

		const size_t spellInjected = speakFor(L"alpha <spell>bravo</spell> charlie delta");
		Check(spellInjected > 0, "an embedded spell tag neither crashes nor silences the engine");

		// The bracket itself still has to reach the listener: pico pronounces it
		// correctly, and neutralising every one of them would lose that.
		const size_t plain = speakFor(L"alpha bravo charlie");
		const size_t withBracket = speakFor(L"alpha < bravo charlie");
		std::printf("     without a bracket %.2f s, with one %.2f s\n", plain / 16000.0,
					withBracket / 16000.0);
		Check(withBracket > plain, "an ordinary angle bracket is still spoken");
	}

	std::printf("== spelling out ==\n");
	{
		TestSite site;
		const std::wstring word = L"cat";
		SPVTEXTFRAG fragment = MakeFragment(word.c_str(), word.size(), SPVA_SpellOut);
		hr = engine->Speak(0, GUID_NULL, nullptr, &fragment, &site);
		Check(SUCCEEDED(hr), "a spell-out fragment succeeds");

		TestSite spoken;
		SPVTEXTFRAG plain = MakeFragment(word.c_str(), word.size());
		engine->Speak(0, GUID_NULL, nullptr, &plain, &spoken);
		std::printf("     spelled: %.2f s, spoken: %.2f s\n", site.audio.size() / 16000.0,
					spoken.audio.size() / 16000.0);
		Check(site.audio.size() > spoken.audio.size(), "spelling takes longer than speaking");
		WriteWav(L"sapitest_spell.wav", site.audio);
	}

	engine->Release();
	FreeLibrary(module);
	CoUninitialize();

	std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL CHECKS PASSED" : "FAILURES",
				g_failures, g_failures == 1 ? "" : "s");
	return g_failures == 0 ? 0 : 1;
}
