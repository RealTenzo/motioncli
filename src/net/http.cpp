#include "net/http.h"

#include <windows.h>
#include <winhttp.h>

#include <cstdio>
#include <cstdlib>
#include <vector>

#pragma comment(lib, "winhttp.lib")

namespace motion::http {

namespace {

std::string lastError(const char* stage) {
    DWORD code = GetLastError();
    char buf[256];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%s failed (error %lu)", stage, code);
    return buf;
}

struct UrlParts {
    std::wstring host;
    std::wstring path;
    INTERNET_PORT port = 0;
    bool https = false;
    bool ok = false;
};

UrlParts crackUrl(const std::wstring& url) {
    UrlParts parts;
    URL_COMPONENTS comp{};
    comp.dwStructSize = sizeof(comp);

    wchar_t host[256] = {0};
    wchar_t path[2048] = {0};
    comp.lpszHostName = host;
    comp.dwHostNameLength = ARRAYSIZE(host);
    comp.lpszUrlPath = path;
    comp.dwUrlPathLength = ARRAYSIZE(path);

    if (!WinHttpCrackUrl(url.c_str(), (DWORD)url.size(), 0, &comp))
        return parts;

    parts.host  = host;
    parts.path  = path[0] ? path : L"/";
    parts.port  = comp.nPort;
    parts.https = (comp.nScheme == INTERNET_SCHEME_HTTPS);
    parts.ok    = true;
    return parts;
}

struct OpenedRequest {
    HINTERNET session = nullptr;
    HINTERNET connect = nullptr;
    HINTERNET request = nullptr;
    bool ok = false;

    ~OpenedRequest() { close(); }
    OpenedRequest() = default;
    OpenedRequest(OpenedRequest&& o) noexcept : session(o.session), connect(o.connect), request(o.request), ok(o.ok) { o.session = o.connect = o.request = nullptr; o.ok = false; }
    OpenedRequest& operator=(OpenedRequest&& o) noexcept { if (this != &o) { close(); session = o.session; connect = o.connect; request = o.request; ok = o.ok; o.session = o.connect = o.request = nullptr; o.ok = false; } return *this; }
    OpenedRequest(const OpenedRequest&) = delete;
    OpenedRequest& operator=(const OpenedRequest&) = delete;

    void close() {
        if (request) WinHttpCloseHandle(request);
        if (connect) WinHttpCloseHandle(connect);
        if (session) WinHttpCloseHandle(session);
        request = connect = session = nullptr;
    }
};

OpenedRequest openGet(const std::wstring& url, std::string& err,
                      const std::wstring& extraHeaders = L"") {
    OpenedRequest r;

    UrlParts parts = crackUrl(url);
    if (!parts.ok) { err = "Invalid URL"; return r; }

    r.session = WinHttpOpen(L"MotionCLI/0.2",
                            WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!r.session) { err = lastError("WinHttpOpen"); return r; }

    r.connect = WinHttpConnect(r.session, parts.host.c_str(), parts.port, 0);
    if (!r.connect) { err = lastError("WinHttpConnect"); r.close(); return r; }

    DWORD flags = parts.https ? WINHTTP_FLAG_SECURE : 0;
    r.request = WinHttpOpenRequest(r.connect, L"GET", parts.path.c_str(),
                                   nullptr, WINHTTP_NO_REFERER,
                                   WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!r.request) { err = lastError("WinHttpOpenRequest"); r.close(); return r; }

    DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(r.request, WINHTTP_OPTION_REDIRECT_POLICY,
                     &redirect, sizeof(redirect));

    LPCWSTR headers = extraHeaders.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS
                                           : extraHeaders.c_str();
    DWORD headerLen = extraHeaders.empty() ? 0 : (DWORD)-1L;
    if (!WinHttpSendRequest(r.request, headers, headerLen,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        err = lastError("WinHttpSendRequest"); r.close(); return r;
    }

    if (!WinHttpReceiveResponse(r.request, nullptr)) {
        err = lastError("WinHttpReceiveResponse"); r.close(); return r;
    }

    DWORD status = 0, size = sizeof(status);
    WinHttpQueryHeaders(r.request,
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                        WINHTTP_NO_HEADER_INDEX);
    if (status < 200 || status >= 300) {
        char buf[64];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "HTTP status %lu", status);
        err = buf;
        r.close();
        return r;
    }

    r.ok = true;
    return r;
}

unsigned long long contentLength(HINTERNET request) {
    wchar_t lenStr[32] = {0};
    DWORD size = sizeof(lenStr);
    if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH,
                            WINHTTP_HEADER_NAME_BY_INDEX, lenStr, &size,
                            WINHTTP_NO_HEADER_INDEX)) {
        return _wcstoui64(lenStr, nullptr, 10);
    }
    return 0;
}

}

bool getString(const std::wstring& url, std::string& outBody, std::string& err,
               const std::wstring& extraHeaders) {
    OpenedRequest r = openGet(url, err, extraHeaders);
    if (!r.ok) return false;

    outBody.clear();
    DWORD avail = 0;
    std::vector<char> chunk;
    do {
        avail = 0;
        if (!WinHttpQueryDataAvailable(r.request, &avail)) {
            err = lastError("WinHttpQueryDataAvailable");
            return false;
        }
        if (avail == 0) break;

        chunk.resize(avail);
        DWORD read = 0;
        if (!WinHttpReadData(r.request, chunk.data(), avail, &read)) {
            err = lastError("WinHttpReadData");
            return false;
        }
        outBody.append(chunk.data(), read);
        if (outBody.size() > 1024 * 1024) {
            err = "Response too large";
            return false;
        }
    } while (avail > 0);

    return true;
}

bool downloadFile(const std::wstring& url,
                  const std::wstring& destPath,
                  ProgressFn onProgress,
                  void* progressCtx,
                  std::string& err) {
    OpenedRequest r = openGet(url, err);
    if (!r.ok) return false;

    const unsigned long long total = contentLength(r.request);

    HANDLE file = CreateFileW(destPath.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        err = lastError("CreateFile");
        r.close();
        return false;
    }

    unsigned long long received = 0;
    DWORD avail = 0;
    bool success = true;
    std::vector<char> chunk;

    do {
        avail = 0;
        if (!WinHttpQueryDataAvailable(r.request, &avail)) {
            err = lastError("WinHttpQueryDataAvailable");
            success = false;
            break;
        }
        if (avail == 0) break;

        chunk.resize(avail);
        DWORD read = 0;
        if (!WinHttpReadData(r.request, chunk.data(), avail, &read)) {
            err = lastError("WinHttpReadData");
            success = false;
            break;
        }

        received += read;

        DWORD written = 0;
        if (!WriteFile(file, chunk.data(), read, &written, nullptr) || written != read) {
            err = lastError("WriteFile");
            success = false;
            break;
        }

        if (onProgress) onProgress(received, total, progressCtx);
    } while (avail > 0);

    CloseHandle(file);

    if (!success) DeleteFileW(destPath.c_str());
    return success;
}

bool getBytes(const std::wstring& url, std::vector<unsigned char>& out,
              std::string& err, const std::wstring& extraHeaders) {
    OpenedRequest r = openGet(url, err, extraHeaders);
    if (!r.ok) return false;

    out.clear();
    DWORD avail = 0;
    std::vector<char> chunk;
    do {
        avail = 0;
        if (!WinHttpQueryDataAvailable(r.request, &avail)) {
            err = lastError("WinHttpQueryDataAvailable");
            return false;
        }
        if (avail == 0) break;

        chunk.resize(avail);
        DWORD read = 0;
        if (!WinHttpReadData(r.request, chunk.data(), avail, &read)) {
            err = lastError("WinHttpReadData");
            return false;
        }
        size_t pos = out.size();
        out.resize(pos + read);
        memcpy(out.data() + pos, chunk.data(), read);
    } while (avail > 0);

    return true;
}

}
