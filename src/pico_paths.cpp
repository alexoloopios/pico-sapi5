#include "pico_paths.hpp"

#include <windows.h>
#include <shlobj.h>

#include <mutex>

namespace pico {

namespace {

//: Anchor for GetModuleHandleEx. Taking the address of something in this
//: translation unit finds the module actually running, which is the SAPI DLL
//: when loaded by an application and the executable when a tool links the code
//: directly.
void ModuleAnchor() {}

std::wstring ComputeModuleDirectory() {
	HMODULE module = nullptr;
	if (!GetModuleHandleExW(
			GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCWSTR>(&ModuleAnchor), &module)) {
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
			break;
		}
		path.resize(path.size() * 2);
	}
	const size_t slash = path.find_last_of(L'\\');
	if (slash == std::wstring::npos) {
		return std::wstring();
	}
	return path.substr(0, slash + 1);
}

bool DirectoryExists(const std::wstring& path) {
	const DWORD attributes = GetFileAttributesW(path.c_str());
	return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

std::wstring ComputeLangDirectory() {
	const std::wstring& base = ModuleDirectory();
	if (base.empty()) {
		return base;
	}
	const std::wstring candidate = base + L"lang\\";
	if (DirectoryExists(candidate)) {
		return candidate;
	}
	// A tool run from a subdirectory of the installation still finds the shared
	// lingware one level up.
	const std::wstring parent = base + L"..\\lang\\";
	if (DirectoryExists(parent)) {
		return parent;
	}
	// Last resort: a flat folder with the .bin files loose beside the binary.
	return base;
}

//: %LOCALAPPDATA%\Pico SAPI5\, created on demand. Empty if it cannot be made.
std::wstring UserDataDirectory() {
	PWSTR localAppData = nullptr;
	if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localAppData))) {
		return std::wstring();
	}
	std::wstring directory(localAppData);
	CoTaskMemFree(localAppData);
	directory += L"\\Pico SAPI5\\";
	if (!DirectoryExists(directory) && !CreateDirectoryW(directory.c_str(), nullptr) &&
		GetLastError() != ERROR_ALREADY_EXISTS) {
		return std::wstring();
	}
	return directory;
}

}  // namespace

const std::wstring& ModuleDirectory() {
	static const std::wstring directory = ComputeModuleDirectory();
	return directory;
}

const std::wstring& LangDirectory() {
	static const std::wstring directory = ComputeLangDirectory();
	return directory;
}

std::wstring ConfigFilePath() {
	const std::wstring directory = UserDataDirectory();
	return directory.empty() ? std::wstring() : directory + L"config.ini";
}

std::wstring LogFilePath() {
	const std::wstring directory = UserDataDirectory();
	return directory.empty() ? std::wstring() : directory + L"pico_sapi5.log";
}

std::string ToUtf8(const std::wstring& text) {
	if (text.empty()) {
		return std::string();
	}
	const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
										 nullptr, 0, nullptr, nullptr);
	if (size <= 0) {
		return std::string();
	}
	std::string result(static_cast<size_t>(size), '\0');
	WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), result.data(), size,
						nullptr, nullptr);
	return result;
}

std::wstring FromUtf8(const std::string& text) {
	if (text.empty()) {
		return std::wstring();
	}
	const int size =
		MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
	if (size <= 0) {
		return std::wstring();
	}
	std::wstring result(static_cast<size_t>(size), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), result.data(), size);
	return result;
}

std::string EncodePathForPico(const std::wstring& path) {
	return ToUtf8(path);
}

}  // namespace pico
