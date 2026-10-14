cmake_minimum_required(VERSION 3.19)
project(VRMPet CXX)

# ---------------------------------------------------------------------------
#  VRMPet - VRM avatar rendered by WickedEngine
#
#  WickedEngine is consumed as a subproject built from source. The checkout at
#  WICKED_ENGINE_DIR is treated as strictly read-only: no file inside it is
#  modified by this project. Every build option below mirrors that checkout's
#  existing CMakeCache so the engine is configured identically.
# ---------------------------------------------------------------------------

set(WICKED_ENGINE_DIR "C:/Users/PC/Documents/content/wicked/WickedEngine"
    CACHE PATH "WickedEngine source checkout (read-only)")

if(NOT EXISTS "${WICKED_ENGINE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "WickedEngine not found at ${WICKED_ENGINE_DIR}")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(WICKED_DYNAMIC_LIBRARY OFF CACHE BOOL "" FORCE)
set(WICKED_PIC              OFF CACHE BOOL "" FORCE)
set(USE_LIBCXX              OFF CACHE BOOL "" FORCE)
set(WICKED_EDITOR           OFF CACHE BOOL "" FORCE)
set(WICKED_TESTS            OFF CACHE BOOL "" FORCE)
set(WICKED_IMGUI_EXAMPLE    OFF CACHE BOOL "" FORCE)
set(WICKED_WINDOWS_TEMPLATE OFF CACHE BOOL "" FORCE)
set(WICKED_LINUX_TEMPLATE   OFF CACHE BOOL "" FORCE)
set(WICKED_ENABLE_SYMLINKS  OFF CACHE BOOL "" FORCE)
set(WICKED_ENABLE_IPO       OFF CACHE BOOL "" FORCE)
set(WICKED_EMBED_SHADERS    OFF CACHE BOOL "" FORCE)
set(WICKED_ENABLE_RTTI      OFF CACHE BOOL "" FORCE)

add_subdirectory("${WICKED_ENGINE_DIR}" "${CMAKE_BINARY_DIR}/wickedengine")

# WickedEngine's library exports no model importer at all: wi::scene::LoadModel
# only reads the engine's own .wiscene Archive format. The VRM / glTF / FBX / OBJ
# importers live in Editor/ and are compiled into the editor executable, never
# into the library. To import a .vrm from a standalone app we compile the
# editor's glTF importer ourselves.
#
# third_party/wicked_importer/ModelImporter_GLTF.cpp is a byte-for-byte copy of
# WickedEngine/Editor/ModelImporter_GLTF.cpp (never edited), placed beside a
# stand-in stdafx.h. MSVC resolves a quoted include against the including file's
# own directory first, so the copy picks up our minimal stdafx.h instead of
# Editor/stdafx.h, which would drag in Editor.h and the entire editor.
add_executable(VRMPet WIN32
    src/main.cpp
    # Voice and chat (option A: everything in C++, no WebRTC).
    #   config  every tuning constant and the credentials, from config.txt
    #   util    string/path helpers and the one logger
    #   http    WinHTTP, because WickedEngine has no HTTP client
    #   voice   WASAPI capture, the WAV header, and the validator that keeps a
    #           network WAV away from wiAudio's unbounded chunk walker
    #   fish    /v1/asr, the agent's own LLM, /v1/tts
    #   bubble  the transcript band, Direct2D into the overlay's bitmap
    #   petcall the turn: mic -> text -> reply -> speech -> mouth
    src/config.cpp
    src/util.cpp
    src/http.cpp
    src/voice.cpp
    src/fish.cpp
    src/bubble.cpp
    src/petcall.cpp
    third_party/wicked_importer/ModelImporter_GLTF.cpp
)

target_include_directories(VRMPet PRIVATE
    "${WICKED_ENGINE_DIR}/Editor"                              # ModelImporter.h, tiny_gltf.h, json.hpp
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/wicked_importer"  # our stdafx.h
)

# winhttp  - the transport (no HTTP client exists anywhere in the engine)
# d2d1 +
# dwrite   - the speech bubble; GDI text writes no usable alpha on a layered window
# ole32 +
# oleaut32 - WASAPI (the microphone) and the DirectWrite factory's COM plumbing
# uuid     - IID_PPV_ARGS for the audio interfaces
# shell32  - "Open config.txt" from the right-click menu (ShellExecuteW);
#            CommandLineToArgvW for the command line comes from here too
target_link_libraries(VRMPet PRIVATE
    WickedEngine
    winhttp
    d2d1
    dwrite
    ole32
    oleaut32
    uuid
    shell32
)

# tiny_gltf + nlohmann::json in a single TU overflow the default section limit.
if(MSVC)
    target_compile_options(VRMPet PRIVATE /bigobj)
endif()

set(VRMPET_RUNDIR "${CMAKE_BINARY_DIR}/run")
set_target_properties(VRMPet PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${VRMPET_RUNDIR}"
)

# dxcompiler.dll must sit beside the executable. The engine compiles any shader
# it cannot find precompiled and caches the result into <cwd>/shaders/, so the
# first launch is slow and every launch after that is not.
add_custom_command(TARGET VRMPet POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${WICKED_ENGINE_DIR}/WickedEngine/dxcompiler.dll"
            "${VRMPET_RUNDIR}/dxcompiler.dll"
    COMMENT "Staging dxcompiler.dll next to VRMPet.exe"
)

# The avatar and its idle loop are staged next to the executable too, so the
# tool runs from build/run with no arguments. assets/ in the project root stays
# the source of truth - this is a copy, and copy_if_different keeps it to a
# single stat per build once the files are in place. Both are inputs, not build
# output, so .gitignore tracks the originals and ignores build/ wholesale.
add_custom_command(TARGET VRMPet POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/assets"
            "${VRMPET_RUNDIR}/assets"
    COMMENT "Staging assets/ next to VRMPet.exe"
)

# config.txt ships with the exe and lands beside it, which is where the tool
# reads it from. The copy is made only when there is not one there already:
# the shipped file is a template with an empty API key, and overwriting the live
# copy would throw away the key (and the tuning) every build. A build is then
# idempotent, and a fresh build dir gets a file the tool would have written
# itself anyway - this way it is written by the build, with the comments.
add_custom_command(TARGET VRMPet POST_BUILD
    COMMAND ${CMAKE_COMMAND}
            -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/config.txt
            -DDEST=${VRMPET_RUNDIR}/config.txt
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/stage_config.cmake
    COMMENT "Staging config.txt next to VRMPet.exe (never overwriting an existing one)"
)

# Content/ is 489 MB and only holds sample assets the prototype does not need.
# The avatar is loaded from assets/ instead.
