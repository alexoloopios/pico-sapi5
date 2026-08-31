cmake_minimum_required(VERSION 3.20)
project(PicoSAPI5 VERSION 1.0.0 LANGUAGES C CXX)

# Pico is portable C, so the same sources produce a complete, self-contained
# engine for whichever architecture the host application needs. Configure twice,
# -A Win32 and -A x64, into separate build directories; tools/build_all.ps1 does
# both and stages the result.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Set before anything is defined so that the vendored engine is built the same
# way as the rest: statically linked to the CRT, so an installation needs no
# redistributable and cannot be broken by one being removed.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

add_subdirectory(third_party/pico)

set(PICO_SRC ${CMAKE_CURRENT_SOURCE_DIR}/src)

# --------------------------------------------------------------------------------------
# Everything that does not depend on COM: the voice catalogue, path discovery,
# logging, the settings file, the parameter mapping and the engine wrapper. The
# command line tools link this and nothing else.
# --------------------------------------------------------------------------------------
add_library(pico_common STATIC
    ${PICO_SRC}/pico_voices.cpp
    ${PICO_SRC}/pico_paths.cpp
    ${PICO_SRC}/pico_log.cpp
    ${PICO_SRC}/pico_markup.cpp
    ${PICO_SRC}/pico_engine.cpp
)
target_include_directories(pico_common PUBLIC ${PICO_SRC})
target_link_libraries(pico_common PUBLIC pico_core ole32 oleaut32 uuid advapi32 shell32)

set(PICO_TARGETS pico_common)

# --------------------------------------------------------------------------------------
# Renders text to a WAV without touching COM or SAPI, so a fault can be placed
# either in the engine wrapper or in the layer above it.
# --------------------------------------------------------------------------------------
add_executable(pico_render ${CMAKE_CURRENT_SOURCE_DIR}/tools/pico_render.cpp)
target_link_libraries(pico_render PRIVATE pico_common)
list(APPEND PICO_TARGETS pico_render)

# --------------------------------------------------------------------------------------
# The SAPI 5 engine itself: one in-process COM class plus its self registration.
# --------------------------------------------------------------------------------------
add_library(PicoSAPI5 SHARED
    ${PICO_SRC}/pico_main.cpp
    ${PICO_SRC}/pico_tts_engine.cpp
    ${PICO_SRC}/pico_registry.cpp
    ${PICO_SRC}/pico_sapi.def
)
target_link_libraries(PicoSAPI5 PRIVATE pico_common sapi)
# The two builds are installed side by side in one directory so that they can
# share the lingware, which is by far the largest thing being shipped, so they
# cannot both be called PicoSAPI5.dll.
if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    set_target_properties(PicoSAPI5 PROPERTIES OUTPUT_NAME "PicoSAPI5_x86")
endif()
list(APPEND PICO_TARGETS PicoSAPI5)

# --------------------------------------------------------------------------------------
# Drives the DLL through its class object without registering it, so the whole
# ISpTTSEngine contract can be checked without administrative rights.
# --------------------------------------------------------------------------------------
add_executable(pico_sapitest ${CMAKE_CURRENT_SOURCE_DIR}/tools/pico_sapitest.cpp)
target_link_libraries(pico_sapitest PRIVATE sapi ole32 oleaut32 uuid)
add_dependencies(pico_sapitest PicoSAPI5)
list(APPEND PICO_TARGETS pico_sapitest)

# Speaks out loud through the registered stack, which is the only check that
# proves registration worked and that ordinary applications can find the voices.
add_executable(pico_speak ${CMAKE_CURRENT_SOURCE_DIR}/tools/pico_speak.cpp)
target_link_libraries(pico_speak PRIVATE sapi ole32 oleaut32 uuid)
list(APPEND PICO_TARGETS pico_speak)

foreach(target ${PICO_TARGETS})
    target_compile_definitions(${target} PRIVATE
        UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN
    )
    if(MSVC)
        target_compile_options(${target} PRIVATE /W3 /MP /utf-8)
    endif()
endforeach()

# The engine looks for its lingware in a "lang" directory beside the running
# binary, so the build tree gets its own copy and can be tested in place.
add_custom_target(pico_lingware ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:pico_render>/lang
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/lang $<TARGET_FILE_DIR:pico_render>/lang
    COMMENT "Staging the Pico lingware beside the built binaries"
)
add_dependencies(pico_render pico_lingware)
