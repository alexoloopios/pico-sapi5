// The SAPI 5 engine object.
//
// SAPI hands the engine a linked list of text fragments, each carrying its own
// state -- action, rate, pitch, volume, language -- and expects PCM written back
// to the site along with events describing where in that audio each word,
// sentence and bookmark falls. This class is the whole of that contract.
//
// Two things about pico shape the implementation:
//
//  * Prosody is set with tags inside the text rather than through an API call,
//    so rate and pitch have to be rendered into what is fed to the engine and
//    can only change where a tag can be opened. Volume is applied as a gain on
//    the samples instead, which is what lets it follow an application moving the
//    slider while an utterance is already playing.
//
//  * Pico emits no marks of its own: pico_getData only ever returns audio, so
//    there is nothing to say which sample a given word begins at. What it does
//    do is hold a sentence back until it is complete, which the word boundary
//    machinery below turns into alignment -- see SpeakFragment.

#pragma once

#include <windows.h>

#include <sapi.h>
#include <sapiddk.h>

#include <mutex>
#include <string>
#include <vector>

#include "pico_engine.hpp"
#include "pico_markup.hpp"
#include "pico_voices.hpp"

namespace pico {

class __declspec(uuid("8C240E79-2921-486E-84BD-176E35AEB505")) PicoTTSEngine
	: public ISpTTSEngine,
	  public ISpObjectWithToken {
public:
	PicoTTSEngine();
	virtual ~PicoTTSEngine();

	// IUnknown
	STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
	STDMETHODIMP_(ULONG) AddRef() override;
	STDMETHODIMP_(ULONG) Release() override;

	// ISpTTSEngine
	STDMETHODIMP Speak(DWORD dwSpeakFlags, REFGUID rguidFormatId, const WAVEFORMATEX* pWaveFormatEx,
					   const SPVTEXTFRAG* pTextFragList, ISpTTSEngineSite* pOutputSite) override;
	STDMETHODIMP GetOutputFormat(const GUID* pTargetFormatId, const WAVEFORMATEX* pTargetWaveFormatEx,
								 GUID* pOutputFormatId,
								 WAVEFORMATEX** ppCoMemOutputWaveFormatEx) override;

	// ISpObjectWithToken
	STDMETHODIMP SetObjectToken(ISpObjectToken* pToken) override;
	STDMETHODIMP GetObjectToken(ISpObjectToken** ppToken) override;

private:
	//: One word of the fragment being spoken, waiting for the audio that will
	//: carry it. Offsets are into the original input text, which is what a
	//: SAPI event reports.
	struct PendingWord {
		ULONG sourceOffset;
		ULONG sourceLength;
		//: Share of the audio this word is assumed to occupy. Character count is
		//: a coarse proxy for duration but a monotonic one, which is what
		//: matters for a caret that has to keep moving forwards.
		size_t weight;
		bool startsSentence;
	};

	//: State that lives for one Speak call.
	struct SpeakContext {
		ISpTTSEngineSite* site = nullptr;
		//: Bytes written so far in this call. SAPI rebases these onto the real
		//: output stream, so counting from zero here is correct.
		ULONGLONG audioOffset = 0;
		ULONGLONG eventInterest = 0;
		//: Re-read whenever the site reports a change.
		long sapiRate = 0;
		float gain = 1.0f;
		bool aborted = false;
		//: Sentences still to be passed over before output resumes, from a skip
		//: the site asked for. Only forward skips can be honoured: what has
		//: already been written has already been played.
		long sentencesToSkip = 0;
	};

	HRESULT EnsureVoiceOpen();
	HRESULT ResolveVoiceFromToken();

	//: Picks up rate and volume changes and reports whether the site wants to
	//: stop. Called often enough that an abort is acted on within a sentence.
	bool CheckActions(SpeakContext& context);

	HRESULT SpeakFragment(const SPVTEXTFRAG& fragment, SpeakContext& context);
	HRESULT WriteSilence(ULONG milliseconds, SpeakContext& context);
	HRESULT WriteBookmark(const SPVTEXTFRAG& fragment, SpeakContext& context);

	//: Writes one block of samples, applying the current gain, and advances the
	//: audio offset.
	HRESULT WriteAudio(std::vector<int16_t>& samples, SpeakContext& context);

	//: Distributes `words` across `sampleCount` samples starting at the current
	//: offset and queues the boundary events for them.
	void EmitWordEvents(const std::vector<PendingWord>& words, size_t sampleCount,
						SpeakContext& context);

	LONG referenceCount_ = 1;
	ISpObjectToken* token_ = nullptr;
	const VoiceDesc* voice_ = nullptr;
	Engine engine_;
	//: SAPI serialises Speak against itself, but SetObjectToken and Speak can
	//: still meet, and the engine handle must not be swapped underneath a
	//: synthesis in progress.
	std::mutex mutex_;
};

}  // namespace pico
