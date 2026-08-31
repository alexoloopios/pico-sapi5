#include "pico_log.hpp"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>

#include "pico_paths.hpp"

namespace pico {

namespace {

std::mutex g_mutex;
LogLevel g_level = LogLevel::Error;
bool g_enabled = false;
std::wstring g_path;
std::once_flag g_initOnce;

//: The level is read straight from the ini file rather than through the config
//: module, so that logging is available while the configuration itself is being
//: loaded. PICO_SAPI5_LOG overrides it, which is the easier knob when
//: reproducing a problem inside an application that is already running.
int ReadConfiguredLevel() {
	wchar_t buffer[16] = {};
	if (GetEnvironmentVariableW(L"PICO_SAPI5_LOG", buffer, ARRAYSIZE(buffer)) > 0) {
		return _wtoi(buffer);
	}
	const std::wstring config = ConfigFilePath();
	if (config.empty()) {
		return 0;
	}
	return static_cast<int>(GetPrivateProfileIntW(L"diagnostics", L"logLevel", 0, config.c_str()));
}

void InitOnce() {
	const int level = ReadConfiguredLevel();
	if (level <= 0) {
		return;
	}
	g_level = static_cast<LogLevel>(level > 3 ? 3 : level);
	g_path = LogFilePath();
	g_enabled = !g_path.empty();
	if (g_enabled) {
		LogWrite(LogLevel::Info, "--- Pico SAPI5 log opened (level %d, pid %lu) ---", level,
				 GetCurrentProcessId());
	}
}

const char* LevelName(LogLevel level) {
	switch (level) {
		case LogLevel::Error: return "ERROR";
		case LogLevel::Warn: return "WARN ";
		case LogLevel::Info: return "INFO ";
		default: return "DEBUG";
	}
}

}  // namespace

void LogInit() {
	std::call_once(g_initOnce, InitOnce);
}

bool LogEnabled(LogLevel level) {
	LogInit();
	return g_enabled && static_cast<int>(level) <= static_cast<int>(g_level);
}

void LogWrite(LogLevel level, const char* format, ...) {
	if (g_path.empty()) {
		return;
	}
	char message[1024];
	va_list args;
	va_start(args, format);
	_vsnprintf_s(message, sizeof(message), _TRUNCATE, format, args);
	va_end(args);

	SYSTEMTIME now;
	GetLocalTime(&now);

	std::lock_guard<std::mutex> lock(g_mutex);
	FILE* file = nullptr;
	if (_wfopen_s(&file, g_path.c_str(), L"a") != 0 || file == nullptr) {
		return;
	}
	fprintf(file, "%02d:%02d:%02d.%03d [%lu] %s %s\n", now.wHour, now.wMinute, now.wSecond,
			now.wMilliseconds, GetCurrentThreadId(), LevelName(level), message);
	fclose(file);
}

std::string FormatHResult(long hr) {
	char buffer[160];
	LPSTR text = nullptr;
	const DWORD length = FormatMessageA(
		FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
		nullptr, static_cast<DWORD>(hr), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
		reinterpret_cast<LPSTR>(&text), 0, nullptr);
	if (length > 0 && text != nullptr) {
		// Drop the trailing newline FormatMessage appends.
		std::string description(text, length);
		while (!description.empty() && (description.back() == '\n' || description.back() == '\r')) {
			description.pop_back();
		}
		LocalFree(text);
		_snprintf_s(buffer, sizeof(buffer), _TRUNCATE, "0x%08lX (%s)", static_cast<unsigned long>(hr),
					description.c_str());
	} else {
		if (text != nullptr) {
			LocalFree(text);
		}
		_snprintf_s(buffer, sizeof(buffer), _TRUNCATE, "0x%08lX", static_cast<unsigned long>(hr));
	}
	return buffer;
}

}  // namespace pico
