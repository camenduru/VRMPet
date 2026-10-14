// ---------------------------------------------------------------------------
//  vrmpet::util - shared helpers for the voice/chat feature
//
//  Everything that is not the renderer, the microphone or the network lives
//  here: UTF-8/UTF-16 conversion, path joining, the .env reader, and one log
//  function. Log() is deliberately the only thing in this translation unit that
//  touches WickedEngine, so the other voice modules stay cheap to compile and
//  do not drag the engine headers in.
// ---------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace vrmpet
{
    std::wstring Widen(const std::string& utf8);
    std::string  Narrow(const std::wstring& wide);

    std::string JoinPath(std::string dir, const std::string& rel);
    std::string ExeDirectory();
    std::string WorkingDirectory();
    bool        FileExists(const std::string& path);
    std::string Lower(std::string s);
    std::string Trim(const std::string& s);
    std::string Fmt(float v, int decimals = 4);
    std::string FmtMs(long long ms);

    // Credentials and settings both come from config.txt, which is read by
    // vrmpet::Settings() (see config.h). Nothing in this file knows about a key:
    // the values are never logged, only the names of the keys that were set, so
    // a diagnostic can report the shape of the problem without putting the
    // credential into a file that gets pasted into bug reports.

    // -----------------------------------------------------------------------
    // Logging
    //
    // Engine backlog (so it lands in engine.log next to everything the renderer
    // says) and stderr (so a probe run in a terminal shows the same lines
    // without catting a file). The overlay is normally launched detached, where
    // the stderr copy goes nowhere and costs nothing.
    // -----------------------------------------------------------------------
    void Log(const std::string& msg);
    void LogFlush();

    // Gate for Log(). config.txt's log.enabled drives it, applied once the file
    // is read, so it can be turned off per run. The engine's own backlog lines
    // (shader init, texture allocations) are not affected - only this tool's.
    void SetLogEnabled(bool enabled);

    // -----------------------------------------------------------------------
    // key = value files
    //
    // One pair per line, blanks ignored, an optional pair of quotes around the
    // value, and a line whose first non-space character is '#' is a comment.
    // A '#' anywhere else is part of the value rather than the start of a
    // comment: colours are written #RRGGBB, and a reader that cannot tell those
    // two apart would quietly turn every colour into a truncation. A path that
    // will not open is not an error, it is an empty list.
    // -----------------------------------------------------------------------
    bool ParseKeyValues(const std::string& path,
                        std::vector<std::pair<std::string, std::string>>& out);

    double NowSeconds();
}
