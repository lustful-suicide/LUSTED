#include "offsets_fetcher.h"
#include <windows.h>
#include <winhttp.h>
#include <fstream>
#include <iostream>

OffsetsFetcher::OffsetsFetcher(std::string v, std::filesystem::path c)
    : version_(std::move(v)), root_(std::move(c)) {}

std::string OffsetsFetcher::UrlOffsetsJson(const std::string& v) {
    return "https://www.rbxoffsets.com/api/v1/windows/offsets/version-" + v + "/final/offsets.json";
}
std::string OffsetsFetcher::UrlOffsetsHpp(const std::string& v) {
    return "https://www.rbxoffsets.com/api/v1/windows/offsets/version-" + v + "/final/offsets.hpp";
}
std::string OffsetsFetcher::UrlStructHpp(const std::string& v) {
    return "https://www.rbxoffsets.com/api/v1/windows/offsets/version-" + v + "/default/struct.hpp";
}
std::string OffsetsFetcher::UrlFflagsJson(const std::string& v) {
    return "https://www.rbxoffsets.com/api/v1/windows/offsets/version-" + v + "/default/fflags.json";
}
std::string OffsetsFetcher::UrlFflagsHpp(const std::string& v) {
    return "https://www.rbxoffsets.com/api/v1/windows/offsets/version-" + v + "/default/fflags.hpp";
}

bool OffsetsFetcher::SplitUrl(const std::string& url, std::string& host, std::string& path, bool& https) {
    std::string u = url;
    https = false;
    if (u.rfind("https://", 0) == 0) { https = true; u = u.substr(8); }
    else if (u.rfind("http://", 0) == 0) { u = u.substr(7); }
    auto slash = u.find('/');
    if (slash == std::string::npos) { host = u; path = "/"; }
    else { host = u.substr(0, slash); path = u.substr(slash); }
    return !host.empty();
}

bool OffsetsFetcher::DownloadToFile(const std::string& url, const std::filesystem::path& out) {
    std::string host, path; bool https = true;
    if (!SplitUrl(url, host, path, https)) return false;
    std::wstring wh(host.begin(), host.end()), wp(path.begin(), path.end());

    HINTERNET ses = WinHttpOpen(L"Lusted/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0);
    if (!ses) return false;
    HINTERNET con = WinHttpConnect(ses, wh.c_str(), https ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT, 0);
    if (!con) { WinHttpCloseHandle(ses); return false; }
    DWORD flags = https ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET req = WinHttpOpenRequest(con, L"GET", wp.c_str(), nullptr, nullptr, nullptr, flags);
    if (!req) { WinHttpCloseHandle(con); WinHttpCloseHandle(ses); return false; }
    bool ok = WinHttpSendRequest(req, nullptr, 0, nullptr, 0, 0, 0) &&
              WinHttpReceiveResponse(req, nullptr);
    std::string data;
    if (ok) {
        // Drain until server reports 0 available (fixes 1369-byte truncation:
        // old code exited after the first QueryDataAvailable chunk).
        for (;;) {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(req, &avail)) { ok = false; break; }
            if (avail == 0) break;
            char buf[8192];
            DWORD remaining = avail;
            while (remaining > 0) {
                DWORD chunk = remaining > sizeof(buf) ? sizeof(buf) : remaining;
                DWORD rd = 0;
                if (!WinHttpReadData(req, buf, chunk, &rd) || rd == 0) break;
                data.append(buf, rd);
                remaining -= rd;
            }
        }
        DWORD status = 0; DWORD slen = sizeof(status);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            nullptr, &status, &slen, nullptr);
        if (status < 200 || status >= 300) ok = false;
    }
    WinHttpCloseHandle(req); WinHttpCloseHandle(con); WinHttpCloseHandle(ses);
    if (!ok || data.empty()) return false;
    std::error_code ec;
    std::filesystem::create_directories(out.parent_path(), ec);
    std::ofstream f(out, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(data.data(), (std::streamsize)data.size());
    f.close();
    return (bool)f;
}

OffsetBundle OffsetsFetcher::FetchAll() {
    OffsetBundle b;
    b.versionHash = version_;
    b.dir = root_ / ("version-" + version_);
    b.offsetsJson = b.dir / "offsets.json";
    b.offsetsHpp  = b.dir / "offsets.hpp";
    b.structHpp   = b.dir / "struct.hpp";
    b.fflagsJson  = b.dir / "fflags.json";
    b.fflagsHpp   = b.dir / "fflags.hpp";

    auto need = [](const std::filesystem::path& p, size_t minBytes = 1) {
        std::error_code ec;
        if (!std::filesystem::exists(p, ec)) return true;
        auto sz = std::filesystem::file_size(p, ec);
        if (ec || sz < minBytes) return true;
        // offsets.json truncated by old downloader was exactly ~1369 bytes
        // and is not valid JSON; force redownload for anything suspicious.
        if (p.extension() == ".json" && sz < 20000) {
            // quick JSON sanity: must parse and contain Offsets
            std::ifstream f(p);
            std::string head((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            if (head.find("\"Offsets\"") == std::string::npos ||
                head.find("\"DataModel\"") == std::string::npos)
                return true;
        }
        if (p.extension() == ".hpp" && sz < 2000) return true;
        return false;
    };

    bool j1 = true, h1 = true, s1 = true, j2 = true;
    if (need(b.offsetsJson)) j1 = DownloadToFile(UrlOffsetsJson(version_), b.offsetsJson);
    if (need(b.offsetsHpp))  h1 = DownloadToFile(UrlOffsetsHpp(version_), b.offsetsHpp);
    if (need(b.structHpp))   s1 = DownloadToFile(UrlStructHpp(version_), b.structHpp);
    if (need(b.fflagsJson))  j2 = DownloadToFile(UrlFflagsJson(version_), b.fflagsJson);
    // fflags.hpp is optional mirror; don't fail bundle if missing
    if (need(b.fflagsHpp)) DownloadToFile(UrlFflagsHpp(version_), b.fflagsHpp);

    if (!j1) std::cerr << "[offsets] FAIL " << UrlOffsetsJson(version_) << "\n";
    if (!h1) std::cerr << "[offsets] note: offsets.hpp mirror missing (json is authoritative)\n";
    if (!s1) std::cerr << "[offsets] FAIL " << UrlStructHpp(version_) << "\n";
    if (!j2) std::cerr << "[offsets] FAIL " << UrlFflagsJson(version_) << "\n";

    b.ok = j1 && s1 && j2;
    if (b.ok) {
        std::cout << "[offsets] cached in " << b.dir.string() << "\n"
                  << "  json:   " << b.offsetsJson.string() << "\n"
                  << "  struct: " << b.structHpp.string() << "\n"
                  << "  fflags: " << b.fflagsJson.string() << "\n";
    }
    return b;
}
