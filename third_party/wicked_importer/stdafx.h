#pragma once

// ---------------------------------------------------------------------------
//  Stand-in for the Editor's precompiled header.
//
//  WickedEngine keeps its VRM / glTF / FBX / OBJ importers in Editor/, not in
//  the engine library - wi::scene::LoadModel() only understands WickedEngine's
//  own .wiscene Archive format. To let a standalone app import a .vrm we
//  compile Editor/ModelImporter_GLTF.cpp, which begins with:
//
//      #include "stdafx.h"
//
//  MSVC resolves a quoted include against the directory of the including file
//  first, so the copy of that .cpp in this directory picks up THIS header
//  instead of Editor/stdafx.h (which drags in Editor.h and the whole editor).
//
//  The original .cpp is copied here byte-for-byte and is never edited.
//  Source: WickedEngine/Editor/ModelImporter_GLTF.cpp
//  sha256: 12c6bb7c5e98f2967f303750dc4f8f04068d11dacda1275f5a26f57042e88293
//
//  This mirrors the standard-header list in Editor/stdafx.h, minus Editor.h.
// ---------------------------------------------------------------------------

#include <string>
#include <cassert>
#include <cmath>
#include <deque>
#include <limits>
#include <mutex>
#include <fstream>
#include <istream>
#include <streambuf>
#include <unordered_map>
#include <algorithm>
#include <cctype>
#include <sstream>
#include <vector>

#include "WickedEngine.h"
