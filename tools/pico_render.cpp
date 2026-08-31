// Renders text to a WAV file by driving the pico engine directly.
//
// This is the bottom of the test pyramid: it touches no COM and no SAPI, so when
// something sounds wrong it answers whether the fault is in the engine wrapper
// or in the layer above it. It is also how the sample files under samples/ are
// produced.
//
//   pico_render --list
//   pico_render -v en-GB -r 3 -o hello.wav "Hello there."
//   pico_render --raw -o probe.wav "5 < 10 and 20 > 3"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "pico_engine.hpp"
#include "pico_markup.hpp"
#include "pico_paths.hpp"
#include "pico_voices.hpp"

namespace {

void PrintUsage() {
	std::printf(
		"Renders text with the SVOX Pico engine.\n\n"
		"Usage: pico_render [options] <text>\n\n"
		"  -v, --voice ID     voice to speak with (default en-US)\n"
		"  -o, --out PATH     WAV file to write (default out.wav)\n"
		"  -r, --rate N       SAPI rate, -10..10 (default 0)\n"
		"  -p, --pitch N      SAPI pitch, -10..10 (default 0)\n"
		"  -V, --volume N     SAPI volume, 0..100 (default 100)\n"
		"      --raw          feed the text to pico unescaped, to see how it\n"
		"                     treats characters that look like markup\n"
		"      --list         list the available voices and exit\n");
}

void PrintVoices() {
	std::printf("%-8s  %-24s  %-6s  %s\n", "ID", "Name", "LCID", "Lingware");
	for (const pico::VoiceDesc& voice : pico::AllVoices()) {
		std::printf("%-8ls  %-24ls  %-6ls  %s + %s\n", voice.id, voice.displayName, voice.langHex,
					voice.taResource, voice.sgResource);
	}
}

//: Writes a canonical 16 kHz mono 16-bit PCM RIFF file.
bool WriteWav(const std::wstring& path, const std::vector<int16_t>& samples) {
	FILE* file = nullptr;
	if (_wfopen_s(&file, path.c_str(), L"wb") != 0 || file == nullptr) {
		return false;
	}
	const uint32_t dataBytes = static_cast<uint32_t>(samples.size() * sizeof(int16_t));
	const uint32_t byteRate = pico::kSampleRate * pico::kChannels * pico::kBytesPerSample;
	const uint16_t blockAlign = pico::kChannels * pico::kBytesPerSample;

	auto put32 = [file](uint32_t value) { std::fwrite(&value, 4, 1, file); };
	auto put16 = [file](uint16_t value) { std::fwrite(&value, 2, 1, file); };

	std::fwrite("RIFF", 1, 4, file);
	put32(36 + dataBytes);
	std::fwrite("WAVEfmt ", 1, 8, file);
	put32(16);                                       // PCM chunk size
	put16(1);                                        // WAVE_FORMAT_PCM
	put16(static_cast<uint16_t>(pico::kChannels));
	put32(static_cast<uint32_t>(pico::kSampleRate));
	put32(byteRate);
	put16(blockAlign);
	put16(static_cast<uint16_t>(pico::kBitsPerSample));
	std::fwrite("data", 1, 4, file);
	put32(dataBytes);
	if (dataBytes > 0) {
		std::fwrite(samples.data(), 1, dataBytes, file);
	}
	const bool ok = std::ferror(file) == 0;
	std::fclose(file);
	return ok;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
	std::wstring voiceId = L"en-US";
	std::wstring outPath = L"out.wav";
	std::wstring text;
	long rate = 0;
	long pitch = 0;
	unsigned long volume = 100;
	bool raw = false;

	for (int i = 1; i < argc; ++i) {
		const std::wstring argument = argv[i];
		const bool hasValue = i + 1 < argc;
		if (argument == L"--list") {
			PrintVoices();
			return 0;
		} else if (argument == L"-h" || argument == L"--help") {
			PrintUsage();
			return 0;
		} else if (argument == L"--raw") {
			raw = true;
		} else if ((argument == L"-v" || argument == L"--voice") && hasValue) {
			voiceId = argv[++i];
		} else if ((argument == L"-o" || argument == L"--out") && hasValue) {
			outPath = argv[++i];
		} else if ((argument == L"-r" || argument == L"--rate") && hasValue) {
			rate = _wtol(argv[++i]);
		} else if ((argument == L"-p" || argument == L"--pitch") && hasValue) {
			pitch = _wtol(argv[++i]);
		} else if ((argument == L"-V" || argument == L"--volume") && hasValue) {
			volume = static_cast<unsigned long>(_wtol(argv[++i]));
		} else {
			if (!text.empty()) {
				text += L' ';
			}
			text += argument;
		}
	}

	if (text.empty()) {
		PrintUsage();
		return 2;
	}

	const pico::VoiceDesc* voice = pico::FindVoice(voiceId);
	if (voice == nullptr) {
		std::fwprintf(stderr, L"error: no such voice '%ls'. Try --list.\n", voiceId.c_str());
		return 2;
	}

	pico::Engine engine;
	HRESULT hr = engine.Open(*voice);
	if (FAILED(hr)) {
		std::fwprintf(stderr, L"error: could not open the voice: %hs\n", engine.LastError().c_str());
		return 1;
	}

	pico::Prosody prosody;
	prosody.speed = pico::SpeedFromSapiRate(rate);
	prosody.pitch = pico::PitchFromSapiPitch(pitch);

	const std::string body = raw ? pico::ToUtf8(text) : pico::EscapeText(text);
	const std::string markup = raw ? body : pico::WrapProsody(body, prosody);

	std::vector<int16_t> samples;
	hr = engine.Speak(markup, [&samples](const int16_t* data, size_t count) {
		samples.insert(samples.end(), data, data + count);
		return true;
	});
	if (FAILED(hr)) {
		std::fwprintf(stderr, L"error: synthesis failed: %hs\n", engine.LastError().c_str());
		return 1;
	}

	pico::ApplyGain(samples.data(), samples.size(), pico::GainFromSapiVolume(volume));

	if (!WriteWav(outPath, samples)) {
		std::fwprintf(stderr, L"error: could not write %ls\n", outPath.c_str());
		return 1;
	}

	const double seconds = static_cast<double>(samples.size()) / pico::kSampleRate;
	std::wprintf(L"wrote %ls: %zu samples, %.2f s (voice %ls, speed %d, pitch %d)\n", outPath.c_str(),
				 samples.size(), seconds, voice->id, prosody.speed, prosody.pitch);
	return 0;
}
