# SVOX Pico TTS engine, built as a static library and linked into the SAPI 5 DLL.
#
# The sources under lib/ are the upstream Apache-2.0 engine, vendored verbatim
# except for the little-endian define in picopltf.h (see ../../docs/pico-changes.md).
# Pico is portable C89, so unlike engines that only ship as a 32-bit binary it can
# simply be compiled for whichever architecture the host application needs.

file(GLOB PICO_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/lib/*.c)

add_library(pico_core STATIC ${PICO_SOURCES})
target_include_directories(pico_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/lib)

# Suppresses the dllexport upstream applies to every pico_* entry point, which
# would otherwise be re-exported from whatever DLL links this library.
target_compile_definitions(pico_core PUBLIC PICO_STATIC)

# Upstream targets C89 and predates most of these diagnostics; it is not our code
# to clean up, so warnings are quietened rather than fixed.
if(MSVC)
    target_compile_options(pico_core PRIVATE /W0 /wd4996)
    target_compile_definitions(pico_core PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(pico_core PRIVATE -w)
endif()

set_target_properties(pico_core PROPERTIES C_STANDARD 99 POSITION_INDEPENDENT_CODE ON)
