// Speaks through the registered SAPI 5 stack.
//
// The top of the test pyramid, and the one thing the other tools cannot show:
// that registration worked and that ordinary applications can find and use the
// voices. It goes through ISpVoice exactly as any SAPI application would.
//
//   pico_speak --list
//   pico_speak "Hello there."
//   pico_speak --voice "Pico German" --rate 2 "Guten Tag."
//   pico_speak --events "One two three."

#include <windows.h>

#include <sapi.h>
#include <sperror.h>

#include <cstdio>
#include <string>

namespace {

//: The display name SAPI shows for a token. sphelper.h has SpGetDescription for
//: this, but that header drags in ATL and a deprecated version check, and the
//: name is simply the default value of the token key.
HRESULT GetTokenDescription(ISpObjectToken* token, LPWSTR* description) {
	return token->GetStringValue(nullptr, description);
}

void PrintUsage() {
	std::printf(
		"Speaks text through the registered SAPI 5 voices.\n\n"
		"Usage: pico_speak [options] <text>\n\n"
		"  --list            list every SAPI 5 voice on this machine\n"
		"  --voice NAME      voice to use; matched as a substring of its name\n"
		"  --rate N          SAPI rate, -10..10\n"
		"  --volume N        SAPI volume, 0..100\n"
		"  --events          report word and sentence events as they fire\n"
		"  --all-pico        speak the text once with each Pico voice\n");
}

//: Prints the name of every voice SAPI knows about, marking the ones this
//: engine provides.
HRESULT ListVoices() {
	ISpObjectTokenCategory* category = nullptr;
	HRESULT hr = CoCreateInstance(CLSID_SpObjectTokenCategory, nullptr, CLSCTX_ALL,
								  IID_ISpObjectTokenCategory, reinterpret_cast<void**>(&category));
	if (FAILED(hr)) {
		return hr;
	}
	hr = category->SetId(SPCAT_VOICES, FALSE);
	if (FAILED(hr)) {
		category->Release();
		return hr;
	}
	IEnumSpObjectTokens* tokens = nullptr;
	hr = category->EnumTokens(nullptr, nullptr, &tokens);
	category->Release();
	if (FAILED(hr)) {
		return hr;
	}

	ULONG index = 0;
	ISpObjectToken* token = nullptr;
	while (tokens->Next(1, &token, nullptr) == S_OK) {
		LPWSTR description = nullptr;
		if (SUCCEEDED(GetTokenDescription(token, &description)) && description != nullptr) {
			LPWSTR picoVoice = nullptr;
			const bool isPico =
				SUCCEEDED(token->GetStringValue(L"PicoVoice", &picoVoice)) && picoVoice != nullptr;
			std::wprintf(L"  %2lu. %-34ls %ls\n", ++index, description,
						 isPico ? L"[pico]" : L"");
			if (picoVoice != nullptr) {
				CoTaskMemFree(picoVoice);
			}
			CoTaskMemFree(description);
		}
		token->Release();
		token = nullptr;
	}
	tokens->Release();
	if (index == 0) {
		std::printf("  (no SAPI 5 voices are registered)\n");
	}
	return S_OK;
}

//: Finds the first voice whose description contains `nameFragment`.
HRESULT FindVoice(const std::wstring& nameFragment, ISpObjectToken** outToken) {
	*outToken = nullptr;
	ISpObjectTokenCategory* category = nullptr;
	HRESULT hr = CoCreateInstance(CLSID_SpObjectTokenCategory, nullptr, CLSCTX_ALL,
								  IID_ISpObjectTokenCategory, reinterpret_cast<void**>(&category));
	if (FAILED(hr)) {
		return hr;
	}
	hr = category->SetId(SPCAT_VOICES, FALSE);
	if (FAILED(hr)) {
		category->Release();
		return hr;
	}
	IEnumSpObjectTokens* tokens = nullptr;
	hr = category->EnumTokens(nullptr, nullptr, &tokens);
	category->Release();
	if (FAILED(hr)) {
		return hr;
	}

	ISpObjectToken* token = nullptr;
	while (tokens->Next(1, &token, nullptr) == S_OK) {
		LPWSTR description = nullptr;
		if (SUCCEEDED(GetTokenDescription(token, &description)) && description != nullptr) {
			const bool matches = wcsstr(description, nameFragment.c_str()) != nullptr;
			CoTaskMemFree(description);
			if (matches) {
				*outToken = token;
				tokens->Release();
				return S_OK;
			}
		}
		token->Release();
		token = nullptr;
	}
	tokens->Release();
	return SPERR_NOT_FOUND;
}

//: Speaks and, when asked, reports the events as SAPI delivers them.
HRESULT SpeakWith(ISpVoice* voice, const std::wstring& text, bool reportEvents) {
	if (!reportEvents) {
		return voice->Speak(text.c_str(), SPF_DEFAULT, nullptr);
	}
	voice->SetInterest(SPFEI(SPEI_WORD_BOUNDARY) | SPFEI(SPEI_SENTENCE_BOUNDARY) |
						   SPFEI(SPEI_END_INPUT_STREAM),
					   SPFEI(SPEI_WORD_BOUNDARY) | SPFEI(SPEI_SENTENCE_BOUNDARY) |
						   SPFEI(SPEI_END_INPUT_STREAM));
	HRESULT hr = voice->Speak(text.c_str(), SPF_DEFAULT | SPF_ASYNC, nullptr);
	if (FAILED(hr)) {
		return hr;
	}
	bool finished = false;
	while (!finished) {
		if (voice->WaitForNotifyEvent(5000) != S_OK) {
			break;
		}
		SPEVENT event = {};
		while (voice->GetEvents(1, &event, nullptr) == S_OK) {
			const double seconds = event.ullAudioStreamOffset / 32000.0;
			switch (event.eEventId) {
				case SPEI_WORD_BOUNDARY:
					std::wprintf(L"  %6.2f s  word     at character %lld (%llu long)\n", seconds,
								 static_cast<long long>(event.lParam),
								 static_cast<unsigned long long>(event.wParam));
					break;
				case SPEI_SENTENCE_BOUNDARY:
					std::wprintf(L"  %6.2f s  sentence at character %lld\n", seconds,
								 static_cast<long long>(event.lParam));
					break;
				case SPEI_END_INPUT_STREAM:
					std::wprintf(L"  %6.2f s  end of stream\n", seconds);
					finished = true;
					break;
				default:
					break;
			}
		}
	}
	return voice->WaitUntilDone(30000);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
	std::wstring voiceName;
	std::wstring text;
	long rate = 0;
	long volume = 100;
	bool listOnly = false;
	bool reportEvents = false;
	bool allPico = false;

	for (int i = 1; i < argc; ++i) {
		const std::wstring argument = argv[i];
		const bool hasValue = i + 1 < argc;
		if (argument == L"--list") {
			listOnly = true;
		} else if (argument == L"--events") {
			reportEvents = true;
		} else if (argument == L"--all-pico") {
			allPico = true;
		} else if (argument == L"-h" || argument == L"--help") {
			PrintUsage();
			return 0;
		} else if (argument == L"--voice" && hasValue) {
			voiceName = argv[++i];
		} else if (argument == L"--rate" && hasValue) {
			rate = _wtol(argv[++i]);
		} else if (argument == L"--volume" && hasValue) {
			volume = _wtol(argv[++i]);
		} else {
			if (!text.empty()) {
				text += L' ';
			}
			text += argument;
		}
	}

	HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	if (FAILED(hr)) {
		std::fwprintf(stderr, L"CoInitializeEx failed: 0x%08lX\n", hr);
		return 1;
	}

	if (listOnly) {
		std::printf("SAPI 5 voices:\n");
		hr = ListVoices();
		CoUninitialize();
		return SUCCEEDED(hr) ? 0 : 1;
	}

	if (text.empty()) {
		text = L"The quick brown fox jumps over the lazy dog.";
	}

	ISpVoice* voice = nullptr;
	hr = CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_ALL, IID_ISpVoice,
						  reinterpret_cast<void**>(&voice));
	if (FAILED(hr) || voice == nullptr) {
		std::fwprintf(stderr, L"could not create a SAPI voice: 0x%08lX\n", hr);
		CoUninitialize();
		return 1;
	}
	voice->SetRate(rate);
	voice->SetVolume(static_cast<USHORT>(volume));

	int failures = 0;
	if (allPico) {
		// Every registered voice belonging to this engine, in turn. This is the
		// check that says the whole set really is installed and usable.
		const wchar_t* const names[] = {L"Pico American English", L"Pico British English",
										L"Pico German",           L"Pico Spanish",
										L"Pico French",           L"Pico Italian"};
		for (const wchar_t* name : names) {
			ISpObjectToken* token = nullptr;
			if (FAILED(FindVoice(name, &token)) || token == nullptr) {
				std::wprintf(L"  MISSING  %ls\n", name);
				++failures;
				continue;
			}
			hr = voice->SetVoice(token);
			token->Release();
			if (FAILED(hr)) {
				std::wprintf(L"  FAILED   %ls (SetVoice 0x%08lX)\n", name, hr);
				++failures;
				continue;
			}
			std::wprintf(L"  speaking %ls\n", name);
			hr = SpeakWith(voice, text, reportEvents);
			if (FAILED(hr)) {
				std::wprintf(L"  FAILED   %ls (Speak 0x%08lX)\n", name, hr);
				++failures;
			}
		}
	} else {
		if (!voiceName.empty()) {
			ISpObjectToken* token = nullptr;
			hr = FindVoice(voiceName, &token);
			if (FAILED(hr) || token == nullptr) {
				std::fwprintf(stderr, L"no voice matching '%ls'. Try --list.\n", voiceName.c_str());
				voice->Release();
				CoUninitialize();
				return 1;
			}
			hr = voice->SetVoice(token);
			token->Release();
			if (FAILED(hr)) {
				std::fwprintf(stderr, L"SetVoice failed: 0x%08lX\n", hr);
				voice->Release();
				CoUninitialize();
				return 1;
			}
		}
		hr = SpeakWith(voice, text, reportEvents);
		if (FAILED(hr)) {
			std::fwprintf(stderr, L"Speak failed: 0x%08lX\n", hr);
			++failures;
		}
	}

	voice->Release();
	CoUninitialize();
	if (failures > 0) {
		std::printf("\n%d failure%s\n", failures, failures == 1 ? "" : "s");
	}
	return failures == 0 ? 0 : 1;
}
