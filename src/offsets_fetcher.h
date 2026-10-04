#pragma once
#include <filesystem>
#include <string>

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

private:
    std::string version_;
    std::filesystem::path root_;
    bool DownloadToFile(const std::string& url, const std::filesystem::path& out);
    static bool SplitUrl(const std::string& url, std::string& host, std::string& path, bool& https);
};
