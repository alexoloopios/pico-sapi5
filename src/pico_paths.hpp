// Where this engine's files live.
//
// Nothing here consults the registry for a location: the lingware sits beside
// the DLL that is running, so an installation can be moved, or a second copy
// tested from a build tree, without anything to keep in sync.

#pragma once

#include <string>

namespace pico {

//: Directory containing the DLL (or executable) this code is linked into, with
//: a trailing backslash.
const std::wstring& ModuleDirectory();

//: Directory holding the lingware .bin files. ModuleDirectory()\lang, falling
//: back to the module directory itself so that a flat staging folder also works.
const std::wstring& LangDirectory();

//: Full path of the per-user configuration file under %LOCALAPPDATA%. The
//: directory is created if it does not exist.
std::wstring ConfigFilePath();

//: Full path of the log file, beside the configuration file.
std::wstring LogFilePath();

//: Encodes a path for pico. The vendored picopal_fopen takes UTF-8 on Windows,
//: so any path the filesystem accepts survives the round trip.
std::string EncodePathForPico(const std::wstring& path);

std::string ToUtf8(const std::wstring& text);
std::wstring FromUtf8(const std::string& text);

}  // namespace pico
