#include "app/startup.h"
#include "roblox/offsets_fetcher.h"
#include "runtime/luau_manager.h"
#include "support/log.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iostream>

std::vector<std::filesystem::path> CollectLuas(const std::filesystem::path& dir) {
    std::vector<std::filesystem::path> out;
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) return out;
    for (auto& e : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) break;
        // First-run fetched pack lives one level down (Luas/official/).
        if (e.is_directory(ec)) {
            if (e.path().filename() != "official") continue;
            for (auto& f : std::filesystem::directory_iterator(e.path(), ec)) {
                if (ec) break;
                if (!f.is_regular_file(ec)) continue;
                auto ext = f.path().extension().string();
                if (ext == ".luau" || ext == ".lua") out.push_back(f.path());
            }
            continue;
        }
        if (!e.is_regular_file(ec)) continue;
        auto ext = e.path().extension().string();
        if (ext == ".luau" || ext == ".lua") out.push_back(e.path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

void EnsureOfficialLuas() {
    namespace fs = std::filesystem;
    static const char* kFiles[] = {
        "speed.luau", "jump.luau", "noclip.luau", "teleport.luau", "fullbright.luau",
        "esp.luau", "aimbot.luau",
    };
    constexpr size_t kCount = sizeof(kFiles) / sizeof(kFiles[0]);
    std::error_code ec;
    const fs::path destDir("C:/LUSTED/Luas/official");
    fs::create_directories(destDir, ec);
    const char* env = std::getenv("LUSTED_OFFICIAL_LUAS_URL");
    std::string base = (env && *env)
        ? env
        : "https://raw.githubusercontent.com/lustful-suicide/LUSTED/main/official-luas/";
    if (!base.empty() && base.back() != '/') base.push_back('/');
    // Per-file healing (not all-or-nothing): existing installs that fetched
    // before a new official script existed download just the missing ones.
    // Never overwrites: user edits to already-present files are left alone.
    size_t have = 0, got = 0;
    for (auto* f : kFiles) {
        if (fs::exists(destDir / f, ec)) { ++have; continue; }
        if (OffsetsFetcher::DownloadToFile(base + f, destDir / f)) ++got;
    }
    std::cout << "[official] have " << have << ", fetched " << got << "/" << kCount
              << " from " << base << std::endl;
    if (!have && !got) LustedLog("official fetch failed; retrying next launch");
}

bool LooksLikeHash(const std::string& s) {
    std::string h = s;
    if (h.rfind("version-", 0) == 0) h = h.substr(8);
    if (h.size() < 8 || h.size() > 64) return false;
    for (char c : h) if (!isxdigit((unsigned char)c)) return false;
    return true;
}

size_t RunAutoFiles(LuauManager& luau, const std::filesystem::path& autoDir) {
    std::error_code ec;
    std::filesystem::create_directories(autoDir, ec);
    EnsureOfficialLuas();
    auto luas = CollectLuas(autoDir);
    std::cout << "[auto] " << luas.size() << " file(s) in " << autoDir.string() << std::endl;
    LustedLogf("auto-exec start files=%zu", luas.size());
    size_t ran = 0;
    for (auto& f : luas) {
        std::cout << "[auto] running " << f.string() << std::endl;
        LustedLogf("script start %s", f.filename().string().c_str());
        bool ok = luau.RunFile(f);
        LustedLogf("script end %s ok=%d", f.filename().string().c_str(), ok ? 1 : 0);
        LustedLogFlush();
        if (!ok) std::cerr << "[auto] failed: " << f.string() << std::endl;
        ++ran;
    }
    std::cout << "[run] auto-execution done." << std::endl;
    LustedLog("auto-exec done");
    return ran;
}
