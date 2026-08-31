#include "pico_registry.hpp"

#include <objbase.h>

#include <string>
#include <vector>

#include "pico_log.hpp"
#include "pico_paths.hpp"
#include "pico_tts_engine.hpp"
#include "pico_voices.hpp"

namespace pico {

namespace {

//: Where SAPI 5 looks for voices, relative to the hive being written. The second
//: is the store the speech components that shipped with Windows 10 and later
//: read, Narrator among them; writing both is what makes the voices available to
//: the modern stack as well as to classic SAPI applications.
const wchar_t* const kVoiceRoots[] = {
	L"SOFTWARE\\Microsoft\\Speech\\Voices\\Tokens",
	L"SOFTWARE\\Microsoft\\Speech_OneCore\\Voices\\Tokens",
};

const wchar_t kVendor[] = L"SVOX";
const wchar_t kVersion[] = L"1.0";

//: HKEY_CLASSES_ROOT is a merged view and must not be written to directly, so
//: the machine hive's real location for class registrations is named instead.
const wchar_t kClassesPrefix[] = L"SOFTWARE\\Classes\\CLSID\\";

std::wstring GuidToString(REFGUID guid) {
	wchar_t buffer[64] = {};
	StringFromGUID2(guid, buffer, ARRAYSIZE(buffer));
	return buffer;
}

//: Path of the DLL being registered, so the registration points at wherever it
//: actually is rather than at an install location assumed up front.
std::wstring ThisModulePath() {
	HMODULE module = nullptr;
	if (!GetModuleHandleExW(
			GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCWSTR>(&ThisModulePath), &module)) {
		return std::wstring();
	}
	std::wstring path(MAX_PATH, L'\0');
	for (;;) {
		const DWORD written = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
		if (written == 0) {
			return std::wstring();
		}
		if (written < path.size()) {
			path.resize(written);
			return path;
		}
		path.resize(path.size() * 2);
	}
}

LSTATUS SetValue(HKEY key, const wchar_t* name, const std::wstring& value) {
	return RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
						  static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
}

LSTATUS CreateKey(HKEY hive, const std::wstring& subKey, HKEY* key) {
	return RegCreateKeyExW(hive, subKey.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE,
						   nullptr, key, nullptr);
}

//: Deletes a key and everything under it. Absence counts as success:
//: unregistering an installation that was only partly registered has to work.
LSTATUS DeleteTree(HKEY hive, const std::wstring& subKey) {
	const LSTATUS status = RegDeleteTreeW(hive, subKey.c_str());
	return status == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : status;
}

HRESULT RegisterClassObject(const std::wstring& clsid, const std::wstring& modulePath) {
	const std::wstring classKey = kClassesPrefix + clsid;
	HKEY key = nullptr;
	LSTATUS status = CreateKey(HKEY_LOCAL_MACHINE, classKey, &key);
	if (status != ERROR_SUCCESS) {
		PICO_LOG_ERROR("could not create %ls (error %ld)", classKey.c_str(), status);
		return HRESULT_FROM_WIN32(status);
	}
	SetValue(key, nullptr, L"Pico SAPI5 Voice Engine");
	RegCloseKey(key);

	status = CreateKey(HKEY_LOCAL_MACHINE, classKey + L"\\InprocServer32", &key);
	if (status != ERROR_SUCCESS) {
		return HRESULT_FROM_WIN32(status);
	}
	SetValue(key, nullptr, modulePath);
	// "Both" lets SAPI use the object from whichever apartment it created it in,
	// which is what an in-process engine is expected to support.
	SetValue(key, L"ThreadingModel", L"Both");
	RegCloseKey(key);
	return S_OK;
}

HRESULT RegisterVoice(const wchar_t* root, const VoiceDesc& voice, const std::wstring& clsid) {
	const std::wstring tokenKey = std::wstring(root) + L"\\" + voice.tokenName;
	HKEY key = nullptr;
	LSTATUS status = CreateKey(HKEY_LOCAL_MACHINE, tokenKey, &key);
	if (status != ERROR_SUCCESS) {
		PICO_LOG_ERROR("could not create %ls (error %ld)", tokenKey.c_str(), status);
		return HRESULT_FROM_WIN32(status);
	}
	SetValue(key, nullptr, voice.displayName);
	SetValue(key, L"CLSID", clsid);
	// Read back by SetObjectToken. Keeping the mapping in the token means the
	// engine never has to guess which voice a token stands for.
	SetValue(key, L"PicoVoice", voice.id);
	// SAPI looks for a value named after the language's LCID when it wants a
	// localised display name.
	SetValue(key, voice.langHex, voice.displayName);
	RegCloseKey(key);

	status = CreateKey(HKEY_LOCAL_MACHINE, tokenKey + L"\\Attributes", &key);
	if (status != ERROR_SUCCESS) {
		return HRESULT_FROM_WIN32(status);
	}
	SetValue(key, L"Name", voice.displayName);
	SetValue(key, L"Language", voice.langHex);
	SetValue(key, L"Gender", voice.gender);
	SetValue(key, L"Age", voice.age);
	SetValue(key, L"Vendor", kVendor);
	SetValue(key, L"Version", kVersion);
	// Applications filter on this when they ask SAPI for a voice that can handle
	// a particular locale.
	SetValue(key, L"Locale", voice.langHex);
	RegCloseKey(key);
	return S_OK;
}

}  // namespace

HRESULT RegisterServer() {
	LogInit();
	const std::wstring modulePath = ThisModulePath();
	if (modulePath.empty()) {
		PICO_LOG_ERROR("could not determine the path of the module being registered");
		return E_FAIL;
	}
	const std::wstring clsid = GuidToString(__uuidof(PicoTTSEngine));

	HRESULT hr = RegisterClassObject(clsid, modulePath);
	if (FAILED(hr)) {
		return hr;
	}
	for (const wchar_t* root : kVoiceRoots) {
		for (const VoiceDesc& voice : AllVoices()) {
			hr = RegisterVoice(root, voice, clsid);
			if (FAILED(hr)) {
				// The OneCore store is not present on every edition. Failing to
				// write the classic one is fatal; failing the other is not.
				if (root == kVoiceRoots[0]) {
					return hr;
				}
				PICO_LOG_WARN("could not register %ls under %ls, continuing", voice.id, root);
				break;
			}
		}
	}
	PICO_LOG_INFO("registered %zu voices from %ls", AllVoices().size(), modulePath.c_str());
	return S_OK;
}

HRESULT UnregisterServer() {
	LogInit();
	const std::wstring clsid = GuidToString(__uuidof(PicoTTSEngine));
	LSTATUS worst = ERROR_SUCCESS;

	auto record = [&worst](LSTATUS status) {
		if (status != ERROR_SUCCESS && worst == ERROR_SUCCESS) {
			worst = status;
		}
	};

	for (const wchar_t* root : kVoiceRoots) {
		for (const VoiceDesc& voice : AllVoices()) {
			record(DeleteTree(HKEY_LOCAL_MACHINE, std::wstring(root) + L"\\" + voice.tokenName));
		}
	}
	record(DeleteTree(HKEY_LOCAL_MACHINE, kClassesPrefix + clsid));

	PICO_LOG_INFO("unregistered (worst status %ld)", worst);
	return worst == ERROR_SUCCESS ? S_OK : HRESULT_FROM_WIN32(worst);
}

}  // namespace pico
