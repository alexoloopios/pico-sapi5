#include "pico_engine.hpp"

#include "pico_log.hpp"
#include "pico_paths.hpp"

extern "C" {
#include "picoapi.h"
#include "picodefs.h"
}

namespace pico {

namespace {

//: Arena handed to pico_initialize. The engine suballocates its lingware and
//: working buffers from this and never touches the CRT heap afterwards.
constexpr size_t kMemorySize = 3 * 1024 * 1024;

//: pico_getData takes its buffer size as a signed 16 bit integer, so this has to
//: stay well under 32767. Pico emits audio in very small pieces regardless.
constexpr pico_Int16 kOutBufferSize = 4096;

//: pico_putTextUtf8 takes its length as a signed 16 bit integer too, so long
//: input is pushed in slices.
constexpr size_t kMaxTextChunk = 16384;

//: Rewinds `end` to the nearest UTF-8 character boundary at or before it, so a
//: slice never cuts a multi-byte sequence in half.
size_t AlignToUtf8Boundary(const std::string& text, size_t end) {
	if (end >= text.size()) {
		return text.size();
	}
	size_t limit = end;
	// Continuation bytes are 10xxxxxx; step back off them to the lead byte.
	while (limit > 0 && (static_cast<unsigned char>(text[limit]) & 0xC0) == 0x80) {
		--limit;
	}
	return limit == 0 ? end : limit;
}

}  // namespace

Engine::Engine() = default;

Engine::~Engine() {
	Close();
}

HRESULT Engine::Fail(int status, bool fromEngine, const char* what, HRESULT hr) {
	pico_Retstring message = {};
	if (fromEngine && engine_ != nullptr) {
		pico_getEngineStatusMessage(static_cast<pico_Engine>(engine_), status, message);
	} else if (system_ != nullptr) {
		pico_getSystemStatusMessage(static_cast<pico_System>(system_), status, message);
	}
	lastError_ = message[0] != 0 ? message : "no detail available";
	PICO_LOG_ERROR("%s failed: pico status %d (%s)", what, status, lastError_.c_str());
	return hr;
}

HRESULT Engine::OpenSystem() {
	if (system_ != nullptr) {
		return S_OK;
	}
	memory_.assign(kMemorySize / sizeof(uint64_t), 0);
	pico_System system = nullptr;
	const pico_Status status =
		pico_initialize(memory_.data(), static_cast<pico_Uint32>(kMemorySize), &system);
	if (status != PICO_OK) {
		// Without a system there is nothing to ask for a message, so report the
		// code alone rather than going through Fail.
		lastError_ = "pico_initialize failed";
		PICO_LOG_ERROR("pico_initialize failed with status %d", status);
		memory_.clear();
		memory_.shrink_to_fit();
		return E_FAIL;
	}
	system_ = system;
	return S_OK;
}

HRESULT Engine::Open(const VoiceDesc& voice) {
	// The whole pico system is torn down, not just the voice that was loaded.
	// Pico suballocates lingware from the fixed arena given to pico_initialize,
	// and releasing one voice's resources leaves that arena fragmented: loading
	// a larger voice afterwards then fails with "out of memory creating new
	// engine" while most of the arena is in fact free. Going from the 1.2 MB of
	// French to the 1.4 MB of American English does it every time.
	//
	// Reinitialising costs only a memset of the arena; the expensive part of a
	// switch is reading the lingware, which has to happen either way.
	Close();

	HRESULT hr = OpenSystem();
	if (FAILED(hr)) {
		return hr;
	}
	pico_System system = static_cast<pico_System>(system_);

	const std::wstring& langDirectory = LangDirectory();
	if (langDirectory.empty()) {
		PICO_LOG_ERROR("cannot locate the lingware directory");
		lastError_ = "the lingware directory could not be located";
		return E_FAIL;
	}

	// Both resources are loaded, then named, then bound to a voice definition
	// that pico_newEngine is asked for by name.
	struct ResourceSlot {
		const char* fileName;
		pico_Resource* out;
		pico_Retstring name;
	};
	ResourceSlot resources[2] = {
		{voice.taResource, reinterpret_cast<pico_Resource*>(&langResource_), {}},
		{voice.sgResource, reinterpret_cast<pico_Resource*>(&speakerResource_), {}},
	};

	for (ResourceSlot& resource : resources) {
		const std::wstring fullPath = langDirectory + FromUtf8(resource.fileName);
		const std::string encoded = EncodePathForPico(fullPath);
		pico_Status status = pico_loadResource(
			system, reinterpret_cast<const pico_Char*>(encoded.c_str()), resource.out);
		if (status != PICO_OK) {
			const HRESULT failure = Fail(status, false, "pico_loadResource", E_FAIL);
			PICO_LOG_ERROR("  while loading %ls", fullPath.c_str());
			CloseVoice();
			return failure;
		}
		status = pico_getResourceName(system, *resource.out, resource.name);
		if (status != PICO_OK) {
			const HRESULT failure = Fail(status, false, "pico_getResourceName", E_FAIL);
			CloseVoice();
			return failure;
		}
	}

	const pico_Char* voiceName = reinterpret_cast<const pico_Char*>(voice.picoVoiceName);
	pico_Status status = pico_createVoiceDefinition(system, voiceName);
	if (status != PICO_OK) {
		const HRESULT failure = Fail(status, false, "pico_createVoiceDefinition", E_FAIL);
		CloseVoice();
		return failure;
	}
	// Recorded before the resources are attached so that a failure part way
	// through still releases the definition.
	voiceDefined_ = true;
	voice_ = &voice;

	for (ResourceSlot& resource : resources) {
		status = pico_addResourceToVoiceDefinition(
			system, voiceName, reinterpret_cast<const pico_Char*>(resource.name));
		if (status != PICO_OK) {
			const HRESULT failure = Fail(status, false, "pico_addResourceToVoiceDefinition", E_FAIL);
			CloseVoice();
			return failure;
		}
	}

	pico_Engine engine = nullptr;
	status = pico_newEngine(system, voiceName, &engine);
	if (status != PICO_OK) {
		const HRESULT failure = Fail(status, false, "pico_newEngine", E_FAIL);
		CloseVoice();
		return failure;
	}
	engine_ = engine;
	PICO_LOG_INFO("opened voice %ls (%s, %s)", voice.id, voice.taResource, voice.sgResource);
	return S_OK;
}

void Engine::CloseVoice() {
	pico_System system = static_cast<pico_System>(system_);
	if (system == nullptr) {
		engine_ = nullptr;
		langResource_ = nullptr;
		speakerResource_ = nullptr;
		voiceDefined_ = false;
		voice_ = nullptr;
		return;
	}
	if (engine_ != nullptr) {
		pico_Engine engine = static_cast<pico_Engine>(engine_);
		pico_disposeEngine(system, &engine);
		engine_ = nullptr;
	}
	if (voiceDefined_ && voice_ != nullptr) {
		pico_releaseVoiceDefinition(system, reinterpret_cast<const pico_Char*>(voice_->picoVoiceName));
	}
	voiceDefined_ = false;
	// Unloaded even when the engine was never created: a failed Open may have
	// got this far, and pico's resource table is small enough that leaking
	// entries across retries would eventually exhaust it.
	if (langResource_ != nullptr) {
		pico_Resource resource = static_cast<pico_Resource>(langResource_);
		pico_unloadResource(system, &resource);
		langResource_ = nullptr;
	}
	if (speakerResource_ != nullptr) {
		pico_Resource resource = static_cast<pico_Resource>(speakerResource_);
		pico_unloadResource(system, &resource);
		speakerResource_ = nullptr;
	}
	voice_ = nullptr;
}

void Engine::Close() {
	CloseVoice();
	if (system_ != nullptr) {
		pico_System system = static_cast<pico_System>(system_);
		pico_terminate(&system);
		system_ = nullptr;
	}
	memory_.clear();
	memory_.shrink_to_fit();
}

void Engine::Reset() {
	if (engine_ != nullptr) {
		pico_resetEngine(static_cast<pico_Engine>(engine_), PICO_RESET_SOFT);
	}
}

HRESULT Engine::Pump(const char* utf8, size_t length, const PcmSink& sink, bool* stopped) {
	pico_Engine engine = static_cast<pico_Engine>(engine_);
	const std::string text(utf8, length);

	size_t offset = 0;
	char outBuffer[kOutBufferSize];

	while (offset < length && !*stopped) {
		size_t chunkEnd = offset + kMaxTextChunk;
		if (chunkEnd < length) {
			chunkEnd = AlignToUtf8Boundary(text, chunkEnd);
		} else {
			chunkEnd = length;
		}
		const pico_Int16 chunkSize = static_cast<pico_Int16>(chunkEnd - offset);

		pico_Int16 bytesPut = 0;
		const pico_Status putStatus = pico_putTextUtf8(
			engine, reinterpret_cast<const pico_Char*>(utf8) + offset, chunkSize, &bytesPut);
		if (putStatus < 0) {
			return Fail(putStatus, true, "pico_putTextUtf8", E_FAIL);
		}
		if (bytesPut <= 0) {
			// Nothing was taken and nothing will be: bail out rather than spin.
			PICO_LOG_WARN("pico_putTextUtf8 accepted no input, abandoning the utterance");
			break;
		}
		offset += static_cast<size_t>(bytesPut);

		// Drain everything this text produced before pushing more in.
		pico_Status step = PICO_STEP_BUSY;
		while (step == PICO_STEP_BUSY && !*stopped) {
			pico_Int16 bytesReceived = 0;
			pico_Int16 dataType = 0;
			step = pico_getData(engine, outBuffer, kOutBufferSize, &bytesReceived, &dataType);
			if (step < 0) {
				return Fail(step, true, "pico_getData", E_FAIL);
			}
			if (bytesReceived > 0) {
				// An odd byte count would desynchronise the sample stream, so
				// round down rather than trusting it blindly.
				const size_t sampleCount = static_cast<size_t>(bytesReceived) / sizeof(int16_t);
				if (sampleCount > 0 &&
					!sink(reinterpret_cast<const int16_t*>(outBuffer), sampleCount)) {
					*stopped = true;
				}
			}
		}
	}
	return S_OK;
}

HRESULT Engine::PutText(const char* utf8, size_t length, const PcmSink& sink) {
	if (engine_ == nullptr) {
		return E_FAIL;
	}
	if (length == 0) {
		return S_OK;
	}
	bool stopped = false;
	const HRESULT hr = Pump(utf8, length, sink, &stopped);
	if (FAILED(hr)) {
		return hr;
	}
	if (stopped) {
		// Abandoning mid-utterance leaves text inside the engine that would
		// otherwise be spoken at the start of the next one.
		Reset();
		return S_FALSE;
	}
	return S_OK;
}

HRESULT Engine::Flush(const PcmSink& sink) {
	if (engine_ == nullptr) {
		return E_FAIL;
	}
	// A single zero byte is what tells pico there is no more input coming, so
	// the sentence it is still holding gets released.
	static const char kTerminator[1] = {'\0'};
	bool stopped = false;
	const HRESULT hr = Pump(kTerminator, sizeof(kTerminator), sink, &stopped);
	if (FAILED(hr)) {
		return hr;
	}
	if (stopped) {
		Reset();
		return S_FALSE;
	}
	return S_OK;
}

HRESULT Engine::Speak(const std::string& utf8, const PcmSink& sink) {
	const HRESULT hr = PutText(utf8.data(), utf8.size(), sink);
	if (hr != S_OK) {
		return hr;
	}
	return Flush(sink);
}

}  // namespace pico
