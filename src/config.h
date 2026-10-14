// ---------------------------------------------------------------------------
//  vrmpet::config - every tuning constant, read from config.txt
//
//  Nothing in this tool is meant to be recompiled to be retuned. The band's
//  size, the avatar's headroom, the mouth gain, the bubble paddings and colours
//  and the engine's render size all come from a plain text file next to
//  VRMPet.exe:
//
//      key = value        one per line, `#` starts a comment
//
//  The built-in table in config.cpp holds a default for every key, so the file
//  is optional and a partial file is a valid file: a key that is absent keeps
//  its default, and a key that is present overrides it. On a run where no file
//  is found at all, the defaults are written out next to the executable - which
//  is how the list of things that *can* be tuned is discoverable without
//  reading this source.
//
//  Read through Settings(), which is a process-wide singleton: the band reads
//  its style once in Bubble::Create() and the overlay reads its geometry once
//  per window, and neither of them keeps a copy of the file.
// ---------------------------------------------------------------------------
#pragma once

#include <string>
#include <vector>

namespace vrmpet
{
    class Config
    {
    public:
        // First directory that holds a config.txt wins. Idempotent: the first
        // Load() of the process is the one that counts, because a second call
        // would silently disagree with the values already in use.
        void Load(const std::vector<std::string>& dirs);
        bool Loaded() const { return loaded_; }

        // A key that the file does not mention - or that holds something that
        // will not parse - falls back to the built-in default rather than to
        // zero, so one bad line cannot make the window 0x0.
        int         Int(const std::string& key) const;
        float       Float(const std::string& key) const;
        std::string Text(const std::string& key) const;
        bool        Flag(const std::string& key) const;   // "1", "true", "yes", "on"

        const std::string& Path() const { return path_; }
        std::vector<std::string> Unknown() const;   // in the file, not in this build

        // The full default file - every key, its value, and its comment.
        std::string Render() const;
        bool WriteDefaults(const std::string& path) const;

        static std::string Default(const std::string& key);

    private:
        std::vector<std::pair<std::string, std::string>> items_;
        std::string path_;
        bool        loaded_ = false;
    };

    Config& Settings();
}
