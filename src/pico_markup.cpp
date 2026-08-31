#include "pico_markup.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <cwctype>

#include "pico_paths.hpp"

namespace pico {

namespace {

int Clamp(int value, int low, int high) {
	return value < low ? low : (value > high ? high : value);
}

//: Every tag pico's tokenizer recognises, from the TOK_MARKUP_KW_ list in
//: picodefs.h. "s" and "p" are the abbreviations for sentence and paragraph.
const wchar_t* const kPicoTags[] = {
	L"ignore",   L"speed",    L"pitch", L"volume", L"voice", L"preproccontext", L"mark",
	L"play",     L"usesig",   L"genfile", L"sentence", L"s",  L"paragraph",     L"p",
	L"break",    L"spell",    L"phoneme", L"item",  L"speaker",
};

//: True when the word ending at `wordEnd` closed a sentence, so that the next
//: one opens a new one. Trailing quotes and brackets are stepped over first, so
//: that a sentence ending in `he said."` still counts.
bool EndsSentence(const std::wstring& text, size_t wordEnd) {
	size_t index = wordEnd;
	while (index > 0) {
		const wchar_t character = text[index - 1];
		if (character == L'"' || character == L'\'' || character == L')' || character == L']' ||
			character == L'}' || character == L'”' || character == L'’') {
			--index;
			continue;
		}
		return character == L'.' || character == L'!' || character == L'?' || character == L'…';
	}
	return false;
}

bool IsTagDelimiter(wchar_t character) {
	return character == L'>' || character == L'/' || character == L' ' || character == L'\t' ||
		   character == L'\r' || character == L'\n';
}

//: True when the '<' at `index` opens something pico would read as one of its
//: own tags: the bracket, an optional '/', optional spaces, a keyword, and then
//: a delimiter. Matched case insensitively, which errs towards neutralising a
//: character that would only have been dropped anyway.
bool StartsPicoTag(const std::wstring& text, size_t index) {
	size_t cursor = index + 1;
	if (cursor < text.size() && text[cursor] == L'/') {
		++cursor;
	}
	while (cursor < text.size() && (text[cursor] == L' ' || text[cursor] == L'\t')) {
		++cursor;
	}
	for (const wchar_t* tag : kPicoTags) {
		const size_t length = wcslen(tag);
		if (text.size() - cursor < length) {
			continue;
		}
		if (_wcsnicmp(text.c_str() + cursor, tag, length) != 0) {
			continue;
		}
		// A bare "<speedy>" is not the speed tag, so the keyword has to end here.
		const size_t after = cursor + length;
		if (after == text.size() || IsTagDelimiter(text[after])) {
			return true;
		}
	}
	return false;
}

//: Both ends of the SAPI range are treated as a constant factor on the neutral
//: value, so a step is the same proportional change wherever it is taken from.
int ScaleAroundDefault(long sapiValue, int neutral, double factorAtExtreme, int low, int high) {
	const double steps = static_cast<double>(Clamp(static_cast<int>(sapiValue), -10, 10)) / 10.0;
	const double scaled = neutral * std::pow(factorAtExtreme, steps);
	return Clamp(static_cast<int>(std::lround(scaled)), low, high);
}

}  // namespace

int SpeedFromSapiRate(long rate) {
	return ScaleAroundDefault(rate, kPicoSpeedDefault, 3.0, kPicoSpeedMin, kPicoSpeedMax);
}

int PitchFromSapiPitch(long pitch) {
	// A factor of two either way is exactly pico's 50..200, so neither end clips.
	return ScaleAroundDefault(pitch, kPicoPitchDefault, 2.0, kPicoPitchMin, kPicoPitchMax);
}

float GainFromSapiVolume(unsigned long volume) {
	if (volume >= 100) {
		return 1.0f;
	}
	return static_cast<float>(volume) / 100.0f;
}

void ApplyGain(int16_t* samples, size_t count, float gain) {
	if (gain >= 1.0f) {
		return;
	}
	if (gain <= 0.0f) {
		std::fill(samples, samples + count, static_cast<int16_t>(0));
		return;
	}
	for (size_t i = 0; i < count; ++i) {
		const float scaled = static_cast<float>(samples[i]) * gain;
		// Saturate: wrapping would turn a loud passage into a burst of noise.
		const float clamped = std::max(-32768.0f, std::min(32767.0f, scaled));
		samples[i] = static_cast<int16_t>(std::lround(clamped));
	}
}

std::string EscapeText(const std::wstring& text) {
	// Pico's tokenizer reads '<' as the start of a markup tag. When what follows
	// is not a tag it recognises it gives up and speaks the characters, so an
	// ordinary "5 < 10" is pronounced correctly and needs no help. When what
	// follows *is* one of its own tags the consequences are much worse: at best
	// the tag takes effect, so a document being read aloud can change the
	// listener's rate or voice, and <spell> reliably crashes the engine -- which,
	// in a screen reader reading a web page, takes the application with it.
	//
	// So only the dangerous shape is neutralised. Substituting the fullwidth
	// look-alike stops the tokenizer recognising a tag; pico then drops the
	// character, which loses nothing that was going to be spoken anyway.
	return ToUtf8(EscapeTextW(text));
}

std::wstring EscapeTextW(const std::wstring& text) {
	std::wstring substituted;
	substituted.reserve(text.size());
	for (size_t i = 0; i < text.size(); ++i) {
		if (text[i] == L'<' && StartsPicoTag(text, i)) {
			substituted.push_back(L'\uFF1C');  // fullwidth <
		} else {
			substituted.push_back(text[i]);
		}
	}
	return substituted;
}

std::vector<WordSpan> SplitWords(const std::wstring& text) {
	std::vector<WordSpan> spans;
	size_t cursor = 0;
	while (cursor < text.size()) {
		while (cursor < text.size() && iswspace(text[cursor])) {
			++cursor;
		}
		if (cursor >= text.size()) {
			break;
		}
		WordSpan span = {};
		span.wordBegin = cursor;
		while (cursor < text.size() && !iswspace(text[cursor])) {
			++cursor;
		}
		span.wordEnd = cursor;
		span.startsSentence = spans.empty() || EndsSentence(text, spans.back().wordEnd);
		spans.push_back(span);
	}
	if (spans.empty()) {
		return spans;
	}
	// Each span feeds from where the previous one stopped through to the start of
	// the next word, so the whitespace that follows a full stop is pushed in
	// while that sentence is still the one being accounted for. Pico needs to see
	// past the punctuation before it releases the sentence, and this is what
	// keeps the audio it then produces attributed to the right words.
	spans.front().feedBegin = 0;
	for (size_t i = 0; i + 1 < spans.size(); ++i) {
		spans[i].feedEnd = spans[i + 1].wordBegin;
		spans[i + 1].feedBegin = spans[i].feedEnd;
	}
	spans.back().feedEnd = text.size();
	return spans;
}

std::string WrapProsody(const std::string& escapedUtf8, const Prosody& prosody) {
	// Tags are only emitted when they differ from pico's own default, so that
	// ordinary speech carries no markup at all.
	std::string opening;
	std::string closing;
	if (prosody.pitch != kPicoPitchDefault) {
		opening += "<pitch level=\"" + std::to_string(prosody.pitch) + "\">";
		closing = "</pitch>" + closing;
	}
	if (prosody.speed != kPicoSpeedDefault) {
		opening += "<speed level=\"" + std::to_string(prosody.speed) + "\">";
		closing = "</speed>" + closing;
	}
	if (opening.empty()) {
		return escapedUtf8;
	}
	return opening + escapedUtf8 + closing;
}

std::wstring SpellOutText(const std::wstring& text) {
	std::wstring spelled;
	spelled.reserve(text.size() * 3);
	for (const wchar_t character : text) {
		if (iswspace(character)) {
			continue;
		}
		if (!spelled.empty()) {
			// A comma rather than a space: it gives pico a phrase break to put
			// between the characters, without which a run of letters is read as
			// though it were a word.
			spelled += L", ";
		}
		spelled.push_back(character);
	}
	return spelled;
}

}  // namespace pico
