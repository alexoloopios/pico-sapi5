// A small file logger.
//
// A SAPI engine runs inside whatever application loaded it, so there is no
// console to print to and a failure usually surfaces only as silence. Logging is
// off by default and turned on either in the configuration file or with the
// PICO_SAPI5_LOG environment variable, so that a user chasing a problem can
// produce a trace without a debug build.

#pragma once

#include <string>

namespace pico {

enum class LogLevel { Error = 0, Warn = 1, Info = 2, Debug = 3 };

//: Reads the configured level and opens the log file. Safe to call repeatedly;
//: only the first call in a process does anything.
void LogInit();

bool LogEnabled(LogLevel level);

//: printf style. Prefer the macros below, which skip formatting entirely when
//: the level is off.
void LogWrite(LogLevel level, const char* format, ...);

//: Renders a Windows error code as "0x80004005 (Unspecified error)".
std::string FormatHResult(long hr);

}  // namespace pico

#define PICO_LOG(level, ...)                            \
	do {                                                \
		if (::pico::LogEnabled(::pico::LogLevel::level)) { \
			::pico::LogWrite(::pico::LogLevel::level, __VA_ARGS__); \
		}                                               \
	} while (0)

#define PICO_LOG_ERROR(...) PICO_LOG(Error, __VA_ARGS__)
#define PICO_LOG_WARN(...) PICO_LOG(Warn, __VA_ARGS__)
#define PICO_LOG_INFO(...) PICO_LOG(Info, __VA_ARGS__)
#define PICO_LOG_DEBUG(...) PICO_LOG(Debug, __VA_ARGS__)
