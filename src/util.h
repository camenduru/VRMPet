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

    // -----------------------------------------------------------------------
    // .env
    //
    // The key lives in the project's .env and nothing else reads it: it is
    // never logged, never printed, and never sent anywhere except the host in
    // FISH_API_BASE. Keys() returns the names alone, which is what the startup
    // log reports - a key name that is missing is the whole diagnostic, and
    // printing the value would put the credential in a file that gets pasted
    // into bug reports.
    // -----------------------------------------------------------------------
    class Env
    {
    public:
        // First directory that contains a .env wins.
        bool Load(const std::vector<std::string>& dirs);
        bool        Has(const std::string& key) const;
        std::string Get(const std::string& key, const std::string& fallback = {}) const;
        const std::string& Path() const { return path_; }
        std::string Keys() const;   // "FISH_API_KEY, AGENT" - names only
    private:
        std::vector<std::pair<std::string, std::string>> items_;
        std::string path_;
    };

    Env& Environment();

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

    double NowSeconds();
}
