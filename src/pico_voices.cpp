#include "pico_voices.hpp"

namespace pico {

namespace {

// All six Pico speakers are female adult voices; the lingware contains no others.
const std::vector<VoiceDesc> kVoices = {
	{L"en-US", L"PicoEnUS", L"Pico American English", L"409", 0x0409, L"en-US",
	 "PicoVoiceEnUS", "en-US_ta.bin", "en-US_lh0_sg.bin", L"Female", L"Adult"},
	{L"en-GB", L"PicoEnGB", L"Pico British English", L"809", 0x0809, L"en-GB",
	 "PicoVoiceEnGB", "en-GB_ta.bin", "en-GB_kh0_sg.bin", L"Female", L"Adult"},
	{L"de-DE", L"PicoDeDE", L"Pico German", L"407", 0x0407, L"de-DE",
	 "PicoVoiceDeDE", "de-DE_ta.bin", "de-DE_gl0_sg.bin", L"Female", L"Adult"},
	{L"es-ES", L"PicoEsES", L"Pico Spanish", L"c0a", 0x0c0a, L"es-ES",
	 "PicoVoiceEsES", "es-ES_ta.bin", "es-ES_zl0_sg.bin", L"Female", L"Adult"},
	{L"fr-FR", L"PicoFrFR", L"Pico French", L"40c", 0x040c, L"fr-FR",
	 "PicoVoiceFrFR", "fr-FR_ta.bin", "fr-FR_nk0_sg.bin", L"Female", L"Adult"},
	{L"it-IT", L"PicoItIT", L"Pico Italian", L"410", 0x0410, L"it-IT",
	 "PicoVoiceItIT", "it-IT_ta.bin", "it-IT_cm0_sg.bin", L"Female", L"Adult"},
};

}  // namespace

const std::vector<VoiceDesc>& AllVoices() {
	return kVoices;
}

const VoiceDesc* FindVoice(const std::wstring& id) {
	for (const VoiceDesc& voice : kVoices) {
		if (_wcsicmp(voice.id, id.c_str()) == 0) {
			return &voice;
		}
	}
	return nullptr;
}

const VoiceDesc* FindVoiceForLanguage(LANGID langId) {
	for (const VoiceDesc& voice : kVoices) {
		if (voice.langId == langId) {
			return &voice;
		}
	}
	for (const VoiceDesc& voice : kVoices) {
		if (PRIMARYLANGID(voice.langId) == PRIMARYLANGID(langId)) {
			return &voice;
		}
	}
	return nullptr;
}

const VoiceDesc& DefaultVoice() {
	return kVoices[0];
}

}  // namespace pico
