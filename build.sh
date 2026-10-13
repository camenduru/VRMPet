#!/usr/bin/env bash
# ================================================================
# VRMPet build script  (Bash)
# ----------------------------------------------------------------
# cmd.exe is blocked in the agent sandbox, so the .bat cannot be used.
# This mirrors the toolchain setup from RoguelikeGame/build_output.sh:
#
#   * PATH entries MUST be Unix-style (/c/...). Git Bash silently
#     ignores Windows-style (C:/...) entries for its own lookup, so
#     cl.exe / rc.exe / link.exe are never found and the configure
#     fails at the link probe with "rc ... no such file or directory".
#   * INCLUDE and LIB are read directly by cl.exe, so those stay
#     Windows-style with backslashes and semicolons.
#   * Toolset and SDK versions are auto-detected (highest wins).
#
# Differs from the RoguelikeGame script in one way: this uses the Ninja
# generator instead of NMake Makefiles. Ninja parallelises (24 cores here)
# and is always incremental, which matters because WickedEngine is a large
# library and a serial NMake build of it takes a very long time.
#
# Usage:
#   ./build.sh           configure + incremental build (default)
#   ./build.sh --fresh   wipe the build dir and rebuild everything
# ================================================================
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LOG="$ROOT/build_vrmpet.txt"
cd "$ROOT"

FRESH=0
[ "${1:-}" = "--fresh" ] && FRESH=1

# ---- locate the MSVC toolset + Windows SDK (highest version wins) ----
VS_VC_WIN='C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC'
VS_VC_UNIX='/c/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/MSVC'
KITS_WIN='C:\Program Files (x86)\Windows Kits\10'
KITS_UNIX='/c/Program Files (x86)/Windows Kits/10'
NINJA_UNIX='/c/Users/PC/.workbuddy-ai/binaries/python/versions/3.13.12/Scripts'

if [ ! -d "$VS_VC_UNIX" ]; then
    echo "ERROR: Visual Studio 18 Community not found at $VS_VC_WIN" >&2
    exit 1
fi
TOOLSET="$(ls -1 "$VS_VC_UNIX" | sort -V | tail -1)"
SDK="$(ls -1 "$KITS_UNIX/Include" | sort -V | tail -1)"

export INCLUDE="$VS_VC_WIN\\$TOOLSET\\include;$KITS_WIN\\Include\\$SDK\\ucrt;$KITS_WIN\\Include\\$SDK\\shared;$KITS_WIN\\Include\\$SDK\\um;$KITS_WIN\\Include\\$SDK\\winrt;$KITS_WIN\\Include\\$SDK\\cppwinrt"
export LIB="$VS_VC_WIN\\$TOOLSET\\lib\\x64;$KITS_WIN\\Lib\\$SDK\\ucrt\\x64;$KITS_WIN\\Lib\\$SDK\\um\\x64"
export PATH="$VS_VC_UNIX/$TOOLSET/bin/Hostx64/x64:$KITS_UNIX/bin/$SDK/x64:/c/Program Files/CMake/bin:$NINJA_UNIX:$PATH"

{
    echo "=== START BUILD ==="
    echo "toolset: $TOOLSET"
    echo "sdk:     $SDK"
    echo "cl:      $(command -v cl)"
    echo "rc:      $(command -v rc)"
    echo "link:    $(command -v link)"
} > "$LOG" 2>&1

if [ -z "$(command -v cl)" ] || [ -z "$(command -v rc)" ]; then
    echo "ERROR: toolchain not on PATH - see $LOG" >&2
    exit 1
fi

# The linker cannot overwrite VRMPet.exe while it is running. Same reason the
# RoguelikeGame script does this; the doubled slashes stop MSYS rewriting
# //F //IM into F:/ paths.
echo "=== CLOSE RUNNING APP ===" >> "$LOG"
taskkill //F //IM VRMPet.exe >> "$LOG" 2>&1

if [ "$FRESH" -eq 1 ]; then
    echo "=== FRESH BUILD ===" >> "$LOG"
    # `cmake --fresh` deletes CMakeCache.txt and CMakeFiles/ itself, which also
    # recovers from a build dir configured with a different generator.
    FRESH_ARG="--fresh"
    CLEAN_ARG="--clean-first"
else
    echo "=== INCREMENTAL BUILD ===" >> "$LOG"
    FRESH_ARG=""
    CLEAN_ARG=""
fi

echo "=== CONFIGURE ===" >> "$LOG"
# shellcheck disable=SC2086
cmake $FRESH_ARG -S . -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_MAKE_PROGRAM="$NINJA_UNIX/ninja.exe" >> "$LOG" 2>&1
CFG=$?
echo "=== CONFIGURE EXIT $CFG ===" >> "$LOG"
if [ "$CFG" -ne 0 ]; then
    echo "CONFIGURE FAILED - see $LOG" >&2
    tail -30 "$LOG" >&2
    exit 1
fi

echo "=== BUILD ===" >> "$LOG"
# shellcheck disable=SC2086
cmake --build build --config Release $CLEAN_ARG >> "$LOG" 2>&1
BLD=$?
echo "=== BUILD EXIT $BLD ===" >> "$LOG"

echo "=== ARTIFACT ===" >> "$LOG"
ls -la build/run/ >> "$LOG" 2>&1
echo "=== DONE ===" >> "$LOG"

echo "TUs compiled: $(grep -c 'Building CXX object' "$LOG")"
grep -n "error C[0-9]\|error LNK\|fatal error" "$LOG" | head -20
echo "BUILD_EXIT=$BLD  (log: build_vrmpet.txt)"
exit "$BLD"
