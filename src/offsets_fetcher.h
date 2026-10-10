#pragma once
#include <filesystem>
#include <string>
#include <windows.h>
#include <winhttp.h>

struct OffsetBundle {
    std::string versionHash;
    std::filesystem::path dir;          // offsets/version-<hash>/
    std::filesystem::path offsetsJson;  // final/offsets.json (+ .hpp mirror)
    std::filesystem::path offsetsHpp;
    std::filesystem::path structHpp;    // default/struct.hpp
    std::filesystem::path fflagsJson;   // default/fflags.json (+ .hpp mirror)
    std::filesystem::path fflagsHpp;
    bool ok = false;
};

class OffsetsFetcher {
public:
    OffsetsFetcher(std::string versionHash,
                   std::filesystem::path cacheRoot = "C:/LUSTED/offsets");
    OffsetBundle FetchAll(); // downloads 3 URLs, caches, returns paths

    static std::string UrlOffsetsJson(const std::string& v);
    static std::string UrlOffsetsHpp(const std::string& v);
    static std::string UrlStructHpp(const std::string& v);
    static std::string UrlFflagsJson(const std::string& v);
    static std::string UrlFflagsHpp(const std::string& v);
    // Shared downloader (also used for the first-run official-luas fetch).
    static bool DownloadToFile(const std::string& url, const std::filesystem::path& out);

private:
    std::string version_;
    std::filesystem::path root_;
    static bool SplitUrl(const std::string& url, std::string& host, INTERNET_PORT& port,
                         std::string& path, bool& https);
};
