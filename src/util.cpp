#include "util.h"

// The one WickedEngine include in the voice modules. wi::backlog is the log the
// renderer already writes, and SetLogFile() is how a standalone GUI app gets any
// output at all.
#include "WickedEngine.h"

#include <cstdio>
#include <cstdarg>
#include <chrono>
#include <fstream>
#include <mutex>

namespace vrmpet
{
    std::wstring Widen(const std::string& utf8)
    {
        if (utf8.empty())
            return {};
        const int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), int(utf8.size()), nullptr, 0);
        if (len <= 0)
            return {};
        std::wstring out(size_t(len), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), int(utf8.size()), out.data(), len);
        return out;
    }

    std::string Narrow(const std::wstring& wide)
    {
        if (wide.empty())
            return {};
        const int len = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), int(wide.size()),
                                            nullptr, 0, nullptr, nullptr);
        if (len <= 0)
            return {};
        std::string out(size_t(len), '\0');
        WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), int(wide.size()), out.data(), len,
                            nullptr, nullptr);
        return out;
    }

    std::string JoinPath(std::string dir, const std::string& rel)
    {
        if (dir.empty())
            return rel;
        if (dir.back() != '/' && dir.back() != '\\')
            dir += '/';
        return dir + rel;
    }

    std::string ExeDirectory()
    {
        return wi::helper::GetDirectoryFromPath(wi::helper::GetExecutablePath());
    }

    std::string WorkingDirectory()
    {
        return wi::helper::GetCurrentPath();
    }

    bool FileExists(const std::string& path)
    {
        return !path.empty() && wi::helper::FileExists(path);
    }

    std::string Lower(std::string s)
    {
        for (char& c : s)
        {
            if (c >= 'A' && c <= 'Z')
                c = char(c - 'A' + 'a');
        }
        return s;
    }

    std::string Trim(const std::string& s)
    {
        size_t b = 0, e = s.size();
        while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n'))
            ++b;
        while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n'))
            --e;
        return s.substr(b, e - b);
    }

    std::string Fmt(float v, int decimals)
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "%.*f", decimals, double(v));
        return buf;
    }

    std::string FmtMs(long long ms)
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "%lldms", ms);
        return buf;
    }

    // -----------------------------------------------------------------------
    // .env
    // -----------------------------------------------------------------------
    bool Env::Load(const std::vector<std::string>& dirs)
    {
        for (const std::string& dir : dirs)
        {
            if (dir.empty())
                continue;
            const std::string candidate = JoinPath(dir, ".env");
            std::ifstream file(candidate, std::ios::binary);
            if (!file.is_open())
                continue;

            path_ = candidate;
            items_.clear();

            std::string line;
            while (std::getline(file, line))
            {
                line = Trim(line);
                if (line.empty() || line[0] == '#')
                    continue;
                const size_t eq = line.find('=');
                if (eq == std::string::npos)
                    continue;
                std::string key = Trim(line.substr(0, eq));
                std::string value = Trim(line.substr(eq + 1));
                if (value.size() >= 2 &&
                    ((value.front() == '"' && value.back() == '"') ||
                     (value.front() == '\'' && value.back() == '\'')))
                {
                    value = value.substr(1, value.size() - 2);
                }
                if (!key.empty())
                    items_.emplace_back(key, value);
            }
            return true;
        }
        return false;
    }

    bool Env::Has(const std::string& key) const
    {
        for (const auto& item : items_)
        {
            if (item.first == key)
                return true;
        }
        return false;
    }

    std::string Env::Get(const std::string& key, const std::string& fallback) const
    {
        for (const auto& item : items_)
        {
            if (item.first == key)
                return item.second;
        }
        return fallback;
    }

    std::string Env::Keys() const
    {
        std::string out;
        for (const auto& item : items_)
        {
            if (!out.empty())
                out += ", ";
            out += item.first;
        }
        return out;
    }

    Env& Environment()
    {
        static Env env;
        return env;
    }

    // -----------------------------------------------------------------------
    // Logging
    // -----------------------------------------------------------------------
    namespace
    {
        std::mutex g_logMutex;
    }

    // Every line is stamped with seconds since the process started. A turn is
    // measured in seconds end to end, and the interesting question is almost
    // always "how long was the gap between these two lines" - which an
    // unstamped log cannot answer without a stopwatch next to it.
    void Log(const std::string& msg)
    {
        const double t = NowSeconds();
        char stamp[32];
        snprintf(stamp, sizeof(stamp), "[%8.2f] ", t);
        const std::string line = std::string(stamp) + msg;

        wi::backlog::post(line);
        std::scoped_lock lock(g_logMutex);
        fprintf(stderr, "%s\n", line.c_str());
    }

    void LogFlush()
    {
        wi::backlog::Flush();
        std::scoped_lock lock(g_logMutex);
        fflush(stderr);
    }

    double NowSeconds()
    {
        using clock = std::chrono::steady_clock;
        static const clock::time_point start = clock::now();
        return std::chrono::duration<double>(clock::now() - start).count();
    }
}
