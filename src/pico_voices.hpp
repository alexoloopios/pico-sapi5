// The catalogue of voices this engine offers.
//
// SVOX Pico ships one speaker per language, so a "voice" here is a pair of
// lingware files: the text analysis resource (_ta) and the signal generation
// resource (_sg). Everything the rest of the port needs to know about a voice --
// what to call it, what to register, which files to load -- lives in this table.

#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace pico {

struct VoiceDesc {
	//: Stable identifier, also the value stored in the voice token so that
	//: SetObjectToken can map a token back to this entry.
	const wchar_t* id;
	//: Registry key name under ...\Speech\Voices\Tokens.
	const wchar_t* tokenName;
	//: What the user sees in a voice list.
	const wchar_t* displayName;
	//: SAPI writes the language as a lower case hexadecimal LCID string.
	const wchar_t* langHex;
	LANGID langId;
	//: BCP 47 name, for the SAPI 5.4 "Language" attribute readers and logging.
	const wchar_t* localeName;
	//: Name given to the voice definition inside pico. Must stay under
	//: PICO_MAX_VOICE_NAME_SIZE (32) bytes.
	const char* picoVoiceName;
	const char* taResource;
	const char* sgResource;
	const wchar_t* gender;
	const wchar_t* age;
};

//: The six voices, in the order they are registered and enumerated.
const std::vector<VoiceDesc>& AllVoices();

//: Look a voice up by its id, case insensitively. Null if there is no such voice.
const VoiceDesc* FindVoice(const std::wstring& id);

//: The voice whose language best matches `langId`, or null. An exact LCID match
//: wins over one that only agrees on the primary language, so a request for
//: en-GB gets British English rather than whichever English comes first.
const VoiceDesc* FindVoiceForLanguage(LANGID langId);

//: The voice to fall back on when nothing else is known. Never null.
const VoiceDesc& DefaultVoice();

}  // namespace pico
