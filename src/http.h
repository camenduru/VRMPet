// ---------------------------------------------------------------------------
//  vrmpet::http - a WinHTTP client, and the only network code in the project
//
//  WickedEngine has no HTTP client at all, so this is new. It is deliberately
//  small: three verbs and one multipart body builder, because that is the whole
//  of what option A needs (an ASR upload, three JSON calls and a binary TTS
//  download).
//
//  Two things are set explicitly rather than left to the defaults, and both are
//  "works on my machine" traps:
//
//    * timeouts. The WinHTTP defaults are 60 s to resolve, 60 s to connect, 30 s
//      to send and 30 s to receive. A hung request parks the worker thread for a
//      minute with no visible cause, which for a turn-based conversation is
//      indistinguishable from a hang.
//    * WINHTTP_ACCESS_TYPE_NO_PROXY. The target is api.fish.audio over TLS, and
//      WinHTTP does not consult the WinINet proxy settings unless asked - but
//      when it is asked, a corporate proxy in the way turns every request into
//      a 407. Left as NO_PROXY on purpose: this client talks to one host.
// ---------------------------------------------------------------------------
#pragma once

#include <string>
#include <vector>

namespace vrmpet
{
    struct HttpResponse
    {
        int         status = 0;      // 0 when the request never completed
        std::string body;            // bytes, not necessarily text
        std::string error;           // empty on a completed exchange
        long long   ms = 0;          // wall time for the whole exchange
        // A completed exchange is a 2xx with no transport error. The error test
        // matters: a response truncated mid-body still carries its 200, and
        // without this an empty or partial TTS clip would read as success.
        bool Ok() const { return status >= 200 && status < 300 && error.empty(); }
    };

    struct HttpHeader
    {
        std::wstring name;
        std::wstring value;
    };

    // Synchronous. Safe to call from any thread.
    bool HttpRequest(const std::string& method, const std::string& url,
                     const std::vector<HttpHeader>& headers, const std::string& body,
                     HttpResponse& response);

    // One binary part. Returns the body only; the caller sets the Content-Type
    // header with the boundary, because the boundary is in the body.
    std::string BoundaryBody(const std::string& boundary, const std::string& name,
                             const std::string& filename, const std::string& contentType,
                             const std::string& data);

    std::string NewBoundary();
}
