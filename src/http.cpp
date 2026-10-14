#include "http.h"
#include "util.h"

#include <windows.h>
#include <winhttp.h>

#include <atomic>

#pragma comment(lib, "winhttp.lib")

namespace vrmpet
{
    namespace
    {
        std::atomic<uint32_t> g_boundaryCounter{ 0 };

        struct WinHttpHandle
        {
            HINTERNET h = nullptr;
            WinHttpHandle() = default;
            explicit WinHttpHandle(HINTERNET handle) : h(handle) {}
            ~WinHttpHandle() { if (h != nullptr) WinHttpCloseHandle(h); }
            WinHttpHandle(const WinHttpHandle&) = delete;
            WinHttpHandle& operator=(const WinHttpHandle&) = delete;
            operator HINTERNET() const { return h; }
            explicit operator bool() const { return h != nullptr; }
        };
    }

    std::string NewBoundary()
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "----VRMPetBoundary%08x%08x",
                 unsigned(GetTickCount()), unsigned(g_boundaryCounter.fetch_add(1)));
        return buf;
    }

    std::string BoundaryBody(const std::string& boundary, const std::string& name,
                             const std::string& filename, const std::string& contentType,
                             const std::string& data)
    {
        std::string body;
        body.reserve(data.size() + 512);
        body += "--" + boundary + "\r\n";
        body += "Content-Disposition: form-data; name=\"" + name + "\"";
        if (!filename.empty())
            body += "; filename=\"" + filename + "\"";
        body += "\r\n";
        if (!contentType.empty())
            body += "Content-Type: " + contentType + "\r\n";
        body += "\r\n";
        body += data;
        body += "\r\n--" + boundary + "--\r\n";
        return body;
    }

    bool HttpRequest(const std::string& method, const std::string& url,
                     const std::vector<HttpHeader>& headers, const std::string& body,
                     HttpResponse& response)
    {
        const double t0 = NowSeconds();
        response = {};
        response.status = 0;

        // Every exchange is logged, so a voice that goes quiet can be traced to
        // the network or exonerated from it. A failure logs the reason; a success
        // logs the status and byte count. The Authorization header is never
        // logged, only the method and URL.
        auto fail = [&](const std::string& why) -> bool
        {
            response.error = why;
            response.ms = (long long)((NowSeconds() - t0) * 1000.0);
            Log("VRMPet: http " + method + " " + url + " FAILED - " + why
                + " (" + std::to_string(response.ms) + " ms)");
            return false;
        };

        const std::wstring wurl = Widen(url);
        URL_COMPONENTS parts = {};
        parts.dwStructSize = sizeof(parts);
        parts.dwSchemeLength = DWORD(-1);
        parts.dwHostNameLength = DWORD(-1);
        parts.dwUrlPathLength = DWORD(-1);
        parts.dwExtraInfoLength = DWORD(-1);

        if (!WinHttpCrackUrl(wurl.c_str(), UINT(wurl.size()), 0, &parts))
            return fail("could not parse URL (error " + std::to_string(GetLastError()) + ")");

        const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
        std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
        if (parts.dwExtraInfoLength > 0)
            path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
        if (path.empty())
            path = L"/";

        const bool secure = (parts.nScheme == INTERNET_SCHEME_HTTPS);

        WinHttpHandle session(WinHttpOpen(L"VRMPet/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
                                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
        if (!session)
            return fail("WinHttpOpen failed (error " + std::to_string(GetLastError()) + ")");

        // Bounded so a dead network fails a turn instead of parking a thread.
        WinHttpSetTimeouts(session, 5000, 5000, 30000, 60000);

        WinHttpHandle connect(WinHttpConnect(session, host.c_str(), parts.nPort, 0));
        if (!connect)
            return fail("could not connect to " + Narrow(host)
                + " (error " + std::to_string(GetLastError()) + ")");

        const std::wstring wmethod = Widen(method);
        WinHttpHandle request(WinHttpOpenRequest(connect, wmethod.c_str(), path.c_str(),
                                                 nullptr, WINHTTP_NO_REFERER,
                                                 WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                 secure ? WINHTTP_FLAG_SECURE : 0));
        if (!request)
            return fail("WinHttpOpenRequest failed (error " + std::to_string(GetLastError()) + ")");

        // TLS 1.2 and up. The Windows default is usually fine, but pinning it
        // makes a handshake failure a handshake failure rather than a mystery.
        DWORD securityFlags = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
        WinHttpSetOption(request, WINHTTP_OPTION_SECURITY_FLAGS, &securityFlags, sizeof(securityFlags));

        std::wstring headerBlock;
        for (const HttpHeader& header : headers)
        {
            headerBlock += header.name + L": " + header.value + L"\r\n";
        }

        const void* bodyData = body.empty() ? WINHTTP_NO_REQUEST_DATA : body.data();
        const DWORD bodySize = DWORD(body.size());

        if (!WinHttpSendRequest(request,
                                headerBlock.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headerBlock.c_str(),
                                headerBlock.empty() ? 0 : DWORD(headerBlock.size()),
                                const_cast<void*>(bodyData), bodySize, bodySize, 0))
            return fail("send failed (error " + std::to_string(GetLastError()) + ")");

        if (!WinHttpReceiveResponse(request, nullptr))
            return fail("no response (error " + std::to_string(GetLastError()) + ")");

        DWORD status = 0;
        DWORD statusSize = sizeof(status);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                            WINHTTP_NO_HEADER_INDEX);
        response.status = int(status);

        // A transfer that stops early is the failure behind a clip that plays
        // only halfway: WinHTTP still reports the header's 200, and hands back
        // whatever bytes arrived before the connection ended. When the response
        // is not content-encoded (so Content-Length is the wire length and not a
        // compressed one), compare it against what was actually read and fail the
        // exchange on a short body, so the caller can retry instead of playing a
        // truncated clip.
        DWORD expected = 0;
        DWORD expectedSize = sizeof(expected);
        const bool haveLength = WinHttpQueryHeaders(
            request, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &expected, &expectedSize,
            WINHTTP_NO_HEADER_INDEX) == TRUE;

        wchar_t encoding[64] = {};
        DWORD encodingSize = sizeof(encoding);
        const bool haveEncoding = WinHttpQueryHeaders(
            request, WINHTTP_QUERY_CONTENT_ENCODING,
            WINHTTP_HEADER_NAME_BY_INDEX, encoding, &encodingSize,
            WINHTTP_NO_HEADER_INDEX) == TRUE && encodingSize > 2;

        // Read the whole response. Status text and headers are not needed: every
        // error body this API returns carries its own message in JSON, and that
        // message is what gets logged.
        for (;;)
        {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(request, &available))
            {
                // A real read error, not end-of-data: report it rather than
                // returning the partial body as if the exchange had succeeded.
                return fail("read failed after " + std::to_string(response.body.size())
                    + " bytes (error " + std::to_string(GetLastError()) + ")");
            }
            if (available == 0)
                break;   // end of data
            const size_t offset = response.body.size();
            response.body.resize(offset + available);
            DWORD read = 0;
            if (!WinHttpReadData(request, response.body.data() + offset, available, &read))
            {
                return fail("read failed after " + std::to_string(response.body.size())
                    + " bytes (error " + std::to_string(GetLastError()) + ")");
            }
            response.body.resize(offset + read);
            if (read == 0)
                break;
        }

        if (haveLength && !haveEncoding && response.body.size() != expected)
        {
            return fail("truncated response (" + std::to_string(response.body.size())
                + " of " + std::to_string(expected) + " bytes)");
        }

        response.ms = (long long)((NowSeconds() - t0) * 1000.0);
        Log("VRMPet: http " + method + " " + url + " -> " + std::to_string(response.status)
            + " (" + std::to_string(response.body.size()) + " bytes"
            + (haveLength ? ", expected " + std::to_string(expected) : "") + ", "
            + std::to_string(response.ms) + " ms)");
        return true;
    }
}
