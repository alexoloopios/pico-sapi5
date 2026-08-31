// Self registration.
//
// Two things get written: the COM class that SAPI instantiates, and one voice
// token per entry in the voice catalogue. The tokens are written rather than
// enumerated at run time, which is what makes the voices visible to every SAPI 5
// application without any of them having to know this engine exists.
//
// Registration is machine wide, and so needs administrative rights. There is no
// per-user alternative: SPCAT_VOICES names a path under HKEY_LOCAL_MACHINE, and
// SAPI enumerates a category only from the hive its identifier names, so voice
// tokens written to HKEY_CURRENT_USER are never seen. This was measured, not
// assumed -- see docs/notes.md.
//
// Nothing here is architecture aware. A 32-bit regsvr32 registering the 32-bit
// DLL lands under WOW6432Node through the registry's own redirection, and a
// 64-bit one lands in the native view, so each build registers itself for the
// applications that can load it.

#pragma once

#include <windows.h>

namespace pico {

HRESULT RegisterServer();

//: Removes everything RegisterServer wrote. Missing keys are not an error, so
//: unregistering twice is harmless.
HRESULT UnregisterServer();

}  // namespace pico
