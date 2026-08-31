// Turning SAPI's parameters and text into pico markup.
//
// Pico takes its prosody from a small set of tags embedded in the input --
// <speed level="n">, <pitch level="n"> -- rather than from an API call, so every
// parameter that varies has to be rendered into the text handed to the engine.
// Volume is the exception: SAPI lets an application change it while an utterance
// is already playing, and re-synthesising to honour that would be both slow and
// audible, so it is applied as a gain on the PCM instead. See ApplyGain.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "pico_engine.hpp"

namespace pico {

//: The prosody in pico's own units, ready to be written as tags.
struct Prosody {
	int speed = kPicoSpeedDefault;
	int pitch = kPicoPitchDefault;

	bool operator==(const Prosody& other) const {
		return speed == other.speed && pitch == other.pitch;
	}
};

//: SAPI rate runs -10..+10 around a neutral 0. Each end is treated as a factor
//: of three on the speaking rate, which is roughly what the Microsoft voices do
//: and lands inside pico's 20..500 range at both extremes.
int SpeedFromSapiRate(long rate);

//: SAPI pitch also runs -10..+10, and an octave either way maps exactly onto
//: pico's 50..200, so the two ranges line up without clipping.
int PitchFromSapiPitch(long pitch);

//: SAPI volume is a percentage. Returned as a linear amplitude scale.
float GainFromSapiVolume(unsigned long volume);

//: Scales samples in place, saturating rather than wrapping so that a clipped
//: peak stays a peak instead of inverting into a crackle.
void ApplyGain(int16_t* samples, size_t count, float gain);

//: Rewrites text so pico's tokenizer sees words rather than markup. See the
//: implementation for what pico does with the characters that matter.
std::string EscapeText(const std::wstring& text);

//: The wide form of the same. One character is substituted for one, so an offset
//: into the result still indexes the caller's original text -- which is what
//: lets a word boundary event report a position in the input SAPI handed over.
std::wstring EscapeTextW(const std::wstring& text);

//: One word, together with the run of text that has to be pushed into pico to
//: reach the end of it. Feeding the spans of a string in order reproduces that
//: string exactly, which matters because pico's tokenizer must see the same
//: bytes it would have seen had the text been given to it in one piece.
struct WordSpan {
	//: What to feed for this step: the word plus the whitespace around it.
	size_t feedBegin;
	size_t feedEnd;
	//: The word itself, which is what a boundary event reports.
	size_t wordBegin;
	size_t wordEnd;
	//: True when this word opens a sentence, so a sentence boundary event can be
	//: attached to it.
	bool startsSentence;
};

//: Splits `text` into word spans covering all of it, including any leading and
//: trailing whitespace. Empty when the text holds no words at all.
std::vector<WordSpan> SplitWords(const std::wstring& text);

//: Wraps already-escaped text in the tags for `prosody`.
std::string WrapProsody(const std::string& escapedUtf8, const Prosody& prosody);

//: Separates `text` into individual characters so that each is named rather than
//: run together into a word. Used for SPVA_SpellOut.
//:
//: Pico has a <spell> tag of its own that would do this more idiomatically, but
//: it faults the engine -- see the note in EscapeText -- so the splitting is
//: done here instead.
std::wstring SpellOutText(const std::wstring& text);

}  // namespace pico
