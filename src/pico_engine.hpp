// A C++ wrapper around the pico C API.
//
// Pico is fed text and pumped for audio: pico_putTextUtf8 accepts a slice of the
// input, then pico_getData is called until it reports PICO_STEP_IDLE, meaning it
// wants more text. This class hides that loop, the resource and voice-definition
// bookkeeping, and the fact that several of the API's sizes are signed 16 bit.
//
// It knows nothing about SAPI, so it can be driven equally well by the engine
// DLL and by the command line tools under tools/.

#pragma once

#include <windows.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "pico_voices.hpp"

namespace pico {

//: Pico's lingware is built for one sample rate only.
constexpr int kSampleRate = 16000;
constexpr int kChannels = 1;
constexpr int kBitsPerSample = 16;
constexpr int kBytesPerSample = kBitsPerSample / 8;

//: Ranges the pico markup accepts, from picopr.c. Anything outside them makes
//: the tokenizer raise a warning and ignore the tag, so the mapping in
//: pico_markup.cpp clamps to these.
constexpr int kPicoSpeedMin = 20;
constexpr int kPicoSpeedMax = 500;
constexpr int kPicoSpeedDefault = 100;
constexpr int kPicoPitchMin = 50;
constexpr int kPicoPitchMax = 200;
constexpr int kPicoPitchDefault = 100;

class Engine {
public:
	Engine();
	~Engine();

	Engine(const Engine&) = delete;
	Engine& operator=(const Engine&) = delete;

	//: Loads the voice's two lingware files and creates a pico engine for it.
	//: Reloading a different voice is done by calling Open again; the previous
	//: one is torn down first.
	HRESULT Open(const VoiceDesc& voice);
	void Close();

	bool IsOpen() const { return engine_ != nullptr; }
	const VoiceDesc* Voice() const { return voice_; }

	//: Receives PCM as pico produces it. Returning false abandons the utterance,
	//: which leaves half consumed text in the engine, so Speak resets before
	//: returning in that case.
	using PcmSink = std::function<bool(const int16_t* samples, size_t sampleCount)>;

	//: Synthesizes one piece of pico markup. `utf8` must already be escaped and
	//: wrapped by the helpers in pico_markup.hpp.
	//: S_OK when it ran to completion, S_FALSE when the sink stopped it.
	HRESULT Speak(const std::string& utf8, const PcmSink& sink);

	//: Pushes a piece of text into the engine and drains whatever audio it
	//: produces in response.
	//:
	//: Pico holds a sentence back until it is complete, so a call that ends mid
	//: sentence yields nothing and the one that completes it yields the whole
	//: sentence at once. The SAPI layer relies on exactly that: it feeds a word
	//: at a time and treats the audio that eventually appears as belonging to
	//: the words fed since the last time any did, which is how word boundary
	//: events get their positions without pico reporting any.
	HRESULT PutText(const char* utf8, size_t length, const PcmSink& sink);

	//: Signals end of input, which makes pico release the final sentence, and
	//: drains it. Every sequence of PutText calls has to be closed with this or
	//: the last sentence is never spoken.
	HRESULT Flush(const PcmSink& sink);

	//: Throws away anything half spoken. Called after an abort so the next
	//: utterance starts from a clean state.
	void Reset();

	//: Human readable form of the last pico status this object saw, for logging.
	const std::string& LastError() const { return lastError_; }

private:
	HRESULT OpenSystem();
	void CloseVoice();
	//: Feeds `length` bytes starting at `utf8` and pumps pico dry. `stopped` is
	//: set if the sink asked to stop, in which case the engine is reset.
	HRESULT Pump(const char* utf8, size_t length, const PcmSink& sink, bool* stopped);
	//: Records the message pico associates with `status` and returns `hr`.
	HRESULT Fail(int status, bool fromEngine, const char* what, HRESULT hr);

	void* system_ = nullptr;
	void* engine_ = nullptr;
	void* langResource_ = nullptr;
	void* speakerResource_ = nullptr;
	//: pico writes into a caller-supplied arena rather than calling malloc. Held
	//: as 64 bit words so the block is aligned for everything pico suballocates
	//: from it, and it has to outlive the system that was handed it.
	std::vector<uint64_t> memory_;
	//: Set once a voice definition has been created, so Close knows to release it.
	bool voiceDefined_ = false;
	const VoiceDesc* voice_ = nullptr;
	std::string lastError_;
};

}  // namespace pico
