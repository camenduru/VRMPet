#include "config.h"
#include "util.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace vrmpet
{
    namespace
    {
        // -------------------------------------------------------------------
        // The table
        //
        // One row per setting: which section it is written under, the key, the
        // value a file that says nothing about it gets, and the sentence that
        // explains it. This is the only place these numbers exist - the overlay
        // reads the band's size from here, so the band cannot disagree with the
        // window that has to hold it.
        //
        // The credentials here take the place of the .env this project used to
        // read: same names, same meaning, one file instead of two.
        // -------------------------------------------------------------------
        struct Entry
        {
            const char* section;
            const char* key;
            const char* value;
            const char* note;
        };

        const Entry kEntries[] = {
            // --- who the pet is ---------------------------------------------
            { "fish", "FISH_API_KEY", "",    "your Fish Audio key - the only secret in this file" },
            { "fish", "FISH_API_BASE", "https://api.fish.audio", "where that key is sent; leave it alone unless you are testing a stub" },
            { "fish", "AGENT",        "",    "the agent's name on fish.audio, exactly as it appears there" },
            { "fish", "MODEL",        "drama-3", "TTS voice model; the valid names are read from the API at startup" },
            { "fish", "GREET",        "",    "what the pet says when it appears; empty means it waits to be spoken to. \\n is a line break" },

            // --- how big the pet is -----------------------------------------
            { "avatar", "avatar.zoom", "1.0", "how big the pet is on screen: 1.0 as framed, 2.0 twice as tall. 0.25..4" },

            // --- the pet on the desktop --------------------------------------
            { "overlay", "overlay.margin",        "0",    "gap in px between the pet and the edge of the work area" },
            { "overlay", "overlay.window_size",   "1024", "engine render target, px before avatar.zoom scales it; the avatar is captured from this and cropped" },
            { "overlay", "overlay.crop_pad",      "40",   "headroom in px around the avatar, so an idle sway is never clipped" },
            { "overlay", "overlay.check_frames",  "60",   "frames between re-measurements of the crop box" },
            { "overlay", "overlay.settle_frames", "30",   "frames rendered before the first capture, so spring bones have settled" },
            { "overlay", "overlay.seconds",       "0",    "quit after this many seconds; 0 runs until Ctrl+Shift+Q" },
            { "overlay", "overlay.grabbable",     "1",    "1: the pet can be dragged and right-clicked on its own pixels; 0: always click-through" },
            { "overlay", "overlay.grab_alpha",    "24",   "how solid a pixel must be (0-255) to count as the pet; raise it if the edge feels sticky" },

            // --- the voice ---------------------------------------------------
            { "voice", "mouth.gain", "1.0", "scales the reply's audio level into the mouth; some voices need more" },

            // --- the transcript band, the box the window reserves ------------
            { "band", "band.visible",        "1",   "1: the transcript band is shown; 0: the pet runs with no bubbles (right-click toggles it)" },
            { "band", "band.width",          "420", "px; the band sits directly above the avatar's head" },
            { "band", "band.height",         "214", "px; four one-line bubbles plus the status line" },
            { "band", "band.gap",            "10",  "px between the bottom of the band and the top of the avatar" },
            { "band", "band.history_lines",  "3",   "bubbles shown at once, 1..band.turns_shown (Ctrl+Shift+H toggles)" },
            { "band", "band.turns_shown",    "4",   "most bubbles the band can lay out before it overflows" },
            { "band", "band.max_segments",   "12",  "turns kept in memory, so a long conversation does not grow forever" },

            // --- the bubbles inside it ---------------------------------------
            { "band style", "band.font_family",      "Segoe UI", "" },
            { "band style", "band.font_size",        "14",       "bubble text, px" },
            { "band style", "band.status_font_size", "11",       "the status line and mic meter, px" },
            { "band style", "band.line_height",      "19",       "one line of text at band.font_size, px" },
            { "band style", "band.pad_x",            "12",       "horizontal padding inside a bubble, px" },
            { "band style", "band.pad_y",            "8",        "vertical padding inside a bubble, px" },
            { "band style", "band.radius",           "10",       "bubble corner radius, px" },
            { "band style", "band.line_gap",         "8",        "px between two bubbles" },
            { "band style", "band.tail_room",        "26",       "right margin a bubble leaves for the tail, px" },
            { "band style", "band.tail_rows",        "14",       "rows in the tail at full width, px" },
            { "band style", "band.tail_base",        "26",       "width of the tail where it meets the bubble, px" },
            { "band style", "band.status_height",    "26",       "reserved at the bottom of the band for status + meter, px" },

            // --- and their colours, #RRGGBB or #RRGGBBAA --------------------
            { "band colors", "band.agent_color",     "#FCFCFFF2", "a reply" },
            { "band colors", "band.user_color",      "#FFF2D4F0", "what you said" },
            { "band colors", "band.tool_color",      "#E0E0EBE6", "reserved for tool output" },
            { "band colors", "band.text_color",      "#0F121AFF", "" },
            { "band colors", "band.border_color",    "#3338478C", "the hairline around a bubble" },
            { "band colors", "band.status_color",    "#1A1F29BF", "" },
            { "band colors", "band.mic_track_color", "#4D526159", "the mic meter's background" },
            { "band colors", "band.mic_live_color",  "#3DB85CF2", "the meter while the mic is open" },
            { "band colors", "band.mic_muted_color", "#BF423DF2", "the meter while the mic is muted" },
        };

        const Entry* Find(const std::string& key)
        {
            for (const Entry& entry : kEntries)
            {
                if (key == entry.key)
                    return &entry;
            }
            return nullptr;
        }
    }

    Config& Settings()
    {
        static Config config;
        return config;
    }

    void Config::Load(const std::vector<std::string>& dirs)
    {
        if (loaded_)
            return;
        loaded_ = true;   // even a failed search: one answer per process

        for (const std::string& dir : dirs)
        {
            if (dir.empty())
                continue;
            const std::string candidate = JoinPath(dir, "config.txt");
            std::vector<std::pair<std::string, std::string>> items;
            if (!ParseKeyValues(candidate, items))
                continue;
            path_ = candidate;
            items_ = std::move(items);
            break;
        }

        if (path_.empty())
        {
            // The file is the interface, so it is written rather than described:
            // a run with no config.txt leaves one beside the exe, holding every
            // key this build understands.
            const std::string created = JoinPath(ExeDirectory(), "config.txt");
            WriteDefaults(created);
            Log("VRMPet: no config.txt found - wrote the defaults to " + created
                + "; edit it and it is read on the next run");
            return;
        }

        // A key this build has never heard of is a typo or a file left over from
        // another version. Saying so costs one line and saves the hour spent
        // wondering why `band.width = 900` did nothing.
        std::string unknown;
        for (const auto& item : items_)
        {
            if (Find(item.first) == nullptr)
            {
                if (!unknown.empty())
                    unknown += ", ";
                unknown += item.first;
            }
        }
        if (!unknown.empty())
            Log("VRMPet: config - ignoring unrecognised key(s): " + unknown);
        Log("VRMPet: config " + path_ + " (" + std::to_string(items_.size()) + " keys set)");

        // A file that has fallen behind the build is not an error - every key
        // not in it keeps its default - but it is worth one line, because the
        // whole point of shipping config.txt is that the knobs are visible, and
        // a knob that is absent from the file is invisible.
        const std::vector<std::string> missing = Missing();
        if (!missing.empty())
        {
            std::string names;
            for (const std::string& key : missing)
            {
                if (!names.empty())
                    names += ", ";
                names += key;
            }
            Log("VRMPet: config - " + std::to_string(missing.size()) + " key(s) this build knows "
                "are not in the file (using defaults): " + names);
            Log("VRMPet: config - `VRMPet.exe config` prints the complete file to stdout");
        }
    }

    std::string Config::Default(const std::string& key)
    {
        const Entry* entry = Find(key);
        return entry == nullptr ? std::string() : std::string(entry->value);
    }

    std::string Config::Text(const std::string& key) const
    {
        for (const auto& item : items_)
        {
            if (item.first == key)
                return item.second;
        }
        return Default(key);
    }

    int Config::Int(const std::string& key) const
    {
        const Entry* entry = Find(key);
        if (entry == nullptr)
            return 0;
        const std::string raw = Text(key);
        char* end = nullptr;
        const long value = std::strtol(raw.c_str(), &end, 10);
        if (end == raw.c_str() || *end != '\0')
        {
            Log("VRMPet: config - " + key + " = '" + raw + "' is not a whole number; using " + entry->value);
            return int(std::strtol(entry->value, nullptr, 10));
        }
        return int(value);
    }

    float Config::Float(const std::string& key) const
    {
        const Entry* entry = Find(key);
        if (entry == nullptr)
            return 0.0f;
        const std::string raw = Text(key);
        char* end = nullptr;
        const float value = std::strtof(raw.c_str(), &end);
        if (end == raw.c_str() || *end != '\0')
        {
            Log("VRMPet: config - " + key + " = '" + raw + "' is not a number; using " + entry->value);
            return std::strtof(entry->value, nullptr);
        }
        return value;
    }

    bool Config::Flag(const std::string& key) const
    {
        const std::string raw = Lower(Trim(Text(key)));
        return raw == "1" || raw == "true" || raw == "yes" || raw == "on";
    }

    std::vector<std::string> Config::Unknown() const
    {
        std::vector<std::string> out;
        for (const auto& item : items_)
        {
            if (Find(item.first) == nullptr)
                out.push_back(item.first);
        }
        return out;
    }

    std::vector<std::string> Config::Missing() const
    {
        std::vector<std::string> out;
        for (const Entry& entry : kEntries)
        {
            const bool present = std::any_of(items_.begin(), items_.end(),
                [&](const std::pair<std::string, std::string>& item) { return item.first == entry.key; });
            if (!present)
                out.push_back(entry.key);
        }
        return out;
    }

    std::string Config::Render() const
    {
        std::string out;
        out += "# VRMPet - everything this tool can be told, in one file.\n";
        out += "#\n";
        out += "# Read at startup from config.txt beside VRMPet.exe (or, failing that, the\n";
        out += "# working directory). A key that is not in this file keeps the value shown\n";
        out += "# below; a key that is here overrides it, on the next run.\n";
        out += "#\n";
        out += "# Syntax: one `key = value` per line. A value runs to the end of its line and is\n";
        out += "# taken as written - '#' is a comment only at the start of a line, never inside\n";
        out += "# a value, because a colour is written #RRGGBB. Comments go on their own line.\n";
        out += "#\n";
        out += "# FISH_API_KEY is a secret. The copy beside VRMPet.exe lives in build/, which\n";
        out += "# is gitignored; this copy is shipped with the code, so leave the key out of it.\n";
        out += "#\n";
        out += "# This file is regenerated from the build with:  VRMPet.exe config\n";

        const char* section = nullptr;
        for (const Entry& entry : kEntries)
        {
            out += "\n";
            if (section == nullptr || std::string(section) != entry.section)
            {
                out += "# --- " + std::string(entry.section) + " ---\n";
                section = entry.section;
            }
            if (entry.note != nullptr && entry.note[0] != '\0')
                out += "# " + std::string(entry.note) + "\n";
            out += std::string(entry.key) + " = " + entry.value + "\n";
        }
        return out;
    }

    bool Config::WriteDefaults(const std::string& path) const
    {
        std::FILE* file = std::fopen(path.c_str(), "wb");
        if (file == nullptr)
        {
            Log("VRMPet: config - could not write " + path);
            return false;
        }
        const std::string text = Render();
        const bool ok = std::fwrite(text.data(), 1, text.size(), file) == text.size();
        std::fclose(file);
        return ok;
    }
}
