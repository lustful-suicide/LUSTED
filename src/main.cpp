#include "roblox_finder.h"
#include "offsets_fetcher.h"
#include "memory.h"
#include "instance.h"
#include "ui.h"
#include "drawing.h"
#include "luau_manager.h"
#include "log.h"
#include <iostream>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <thread>
#include <atomic>
#include <vector>
#include <nlohmann/json.hpp>

static std::vector<std::filesystem::path> CollectLuas(const std::filesystem::path& dir) {
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

// First run: fetch C:/LUSTED/Luas/official/ from GitHub (raw). Override
// with LUSTED_OFFICIAL_LUAS_URL ("<base>/", trailing slash optional).
// Absent until pushed upstream -> logs and retries next launch.
static void EnsureOfficialLuas() {
    namespace fs = std::filesystem;
    static const char* kFiles[] = {
        "speed.luau", "jump.luau", "noclip.luau", "teleport.luau", "fullbright.luau",
    };
    constexpr size_t kCount = sizeof(kFiles) / sizeof(kFiles[0]);
    std::error_code ec;
    const fs::path destDir("C:/LUSTED/Luas/official");
    if (fs::exists(destDir, ec)) {
        for (auto& e : fs::directory_iterator(destDir, ec)) {
            if (ec) break;
            if (!e.is_regular_file(ec)) continue;
            auto ext = e.path().extension().string();
            if (ext == ".luau" || ext == ".lua") return; // already fetched
        }
    }
    const char* env = std::getenv("LUSTED_OFFICIAL_LUAS_URL");
    std::string base = (env && *env)
        ? env
        : "https://raw.githubusercontent.com/lustful-suicide/LUSTED/main/official-luas/";
    if (!base.empty() && base.back() != '/') base.push_back('/');
    fs::create_directories(destDir, ec);
    size_t got = 0;
    for (auto* f : kFiles)
        if (OffsetsFetcher::DownloadToFile(base + f, destDir / f)) ++got;
    std::cout << "[official] fetched " << got << "/" << kCount
              << " from " << base << std::endl;
    if (!got) LustedLog("official fetch failed; retrying next launch");
}

static bool LooksLikeHash(const std::string& s) {
    std::string h = s;
    if (h.rfind("version-", 0) == 0) h = h.substr(8);
    if (h.size() < 8 || h.size() > 64) return false;
    for (char c : h) if (!isxdigit((unsigned char)c)) return false;
    return true;
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0); // keep prints visible if we crash
    std::cout << "Lusted external -- finder + offsets + custom-write + luau (auto)" << std::endl;

    bool once = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--help" || a == "-h") {
            std::cout << "usage: lusted.exe [versionHash] [--once]" << std::endl;
            std::cout << "  auto-executes C:/LUSTED/Luas/*.luau (sorted)" << std::endl;
            return 0;
        }
        if (a == "--once") once = true;
    }

    // fast finder on main thread (no heavy scan yet)
    auto found = FindRobloxPlayer();
    std::string versionHash = "02c37bc51a384b8f";
    DWORD pid = 0;
    if (found) {
        std::cout << "[finder] exe: " << found->exePath << std::endl;
        if (!found->versionHash.empty()) versionHash = found->versionHash;
        std::cout << "[finder] version-" << versionHash << std::endl;
        if (!found->fileVersion.empty()) std::cout << "[finder] file version: " << found->fileVersion << std::endl;
        pid = found->pid;
        std::cout << "[finder] pid: " << pid << (pid ? " (running)" : " (not running)") << std::endl;
    } else {
        std::cout << "[finder] RobloxPlayerBeta.exe not found, using fallback version-" << versionHash << std::endl;
    }
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a.rfind("--", 0) == 0) continue;
        if (LooksLikeHash(a)) {
            versionHash = a.rfind("version-", 0) == 0 ? a.substr(8) : a;
            break;
        }
    }

    // UI + overlay first so windows paint while worker scans (no Not Responding)
    UiManager ui;
    DrawingManager drawing;
    if (!ui.Init("LUSTED", pid)) {
        std::cerr << "[ui] Dear ImGui initialization failed" << std::endl;
        return 1;
    }
    drawing.Init(pid);
    ui.Poll(); drawing.Poll();

    std::atomic<bool> done{ false };
    std::atomic<bool> finished{ false };
    std::atomic<bool> stop{ false };
    std::atomic<int> code{ 0 };
    ui.SetStopFlag(&stop);

    std::thread worker([&]() {
        OffsetsFetcher fetcher(versionHash);
        OffsetBundle bundle = fetcher.FetchAll();
        std::cout << "[urls]\n  " << OffsetsFetcher::UrlOffsetsJson(versionHash) << "\n  "
                  << OffsetsFetcher::UrlStructHpp(versionHash) << "\n  "
                  << OffsetsFetcher::UrlFflagsJson(versionHash) << std::endl;
        if (!bundle.ok)
            std::cerr << "[warn] some downloads failed. Check " << bundle.dir.string() << std::endl;

        Memory mem;
        if (pid && mem.Attach(pid))
            std::cout << "[memory] attached (CustomWrite NT path)" << std::endl;
        else std::cout << "[memory] no live attach; helpers return nil until Roblox runs." << std::endl;

        InstanceStore inst(&mem);
        LuauManager luau(&mem, &inst, &pid, &ui, &drawing, &stop);
        // Explorer window reads through this same store (stable address even
        // across rescan moves); pid pointer is main's, also stable.
        ui.SetExplorerStore(&inst);
        ui.SetExplorerPid(&pid);
        // Offsets load before anything else so the index pass (started next)
        // reads an immutable table; Init skips the reload once it sees this.
        if (!inst.IsLoaded()) {
            inst.Load(bundle.offsetsJson.string());
            LustedLog("offsets loaded");
        }
        ui.SetStatus("Indexing…");
        // The heap pass runs ahead of Init now: refresh_game inside Init misses
        // into the build-wait instead of launching competing sweeps, and the
        // auto-exec scripts below land on a warm snapshot.
        std::thread indexThread;
        if (pid && mem.IsOpen()) {
            indexThread = std::thread([&]() {
                if (inst.BuildIndex(pid, true))
                    LustedLogf("index build done ready=%d", inst.IsIndexReady() ? 1 : 0);
                else
                    LustedLog("index build failed/cancelled");
                ui.SetStatus(inst.IsIndexReady() ? "Ready" : "Index failed");
            });
        } else {
            ui.SetStatus("No attach");
        }
        // Script-driven features need the COMPLETE tree (hidden vectors only
        // land in the sweep union), so auto-exec waits for the full pass.
        // The fast snapshot marks ready in ~1s and the sweep unions the rest;
        // the UI stays live on the main thread while this worker waits.
        if (!luau.Init(versionHash, bundle.offsetsJson.string())) {
            std::cerr << "luau init failed" << std::endl;
            inst.RequestIndexCancel();
            if (indexThread.joinable()) indexThread.join();
            code = 1; done = true; finished = true; return;
        }
        LustedLog("luau init ok");
        if (indexThread.joinable()) {
            ui.SetStatus("Indexing (full)…");
            indexThread.join();
            ui.SetStatus(inst.IsIndexReady() ? "Ready" : "Index failed");
        }
        std::filesystem::path autoDir("C:/LUSTED/Luas");
        std::error_code ec;
        std::filesystem::create_directories(autoDir, ec);
        EnsureOfficialLuas();
        auto luas = CollectLuas(autoDir);
        std::cout << "[auto] " << luas.size() << " file(s) in " << autoDir.string() << std::endl;
        LustedLogf("auto-exec start files=%zu", luas.size());
        for (auto& f : luas) {
            std::cout << "[auto] running " << f.string() << std::endl;
            LustedLogf("script start %s", f.filename().string().c_str());
            bool ok = luau.RunFile(f);
            LustedLogf("script end %s ok=%d", f.filename().string().c_str(), ok ? 1 : 0);
            LustedLogFlush();
            if (!ok) std::cerr << "[auto] failed: " << f.string() << std::endl;
        }
        std::cout << "[run] auto-execution done." << std::endl;
        LustedLog("auto-exec done");
        done = true;
        // Tight poll cadence: per-frame features (noclip re-enforce vs server
        // replication races, teleport spam) need as many ticks per second as
        // cheap polling allows. TakeEvent/Poll work is microseconds.
        while (!once && !stop) {
            luau.Poll();
            Sleep(8);
        }
        // Interactive unload lands here while a pass may still run; in --once
        // mode the pass is awaited so the exit state covers the whole snapshot.
        if (!once) inst.RequestIndexCancel();
        if (indexThread.joinable()) indexThread.join();
        finished = true;
    });

    // main thread: pump UI + overlay so Windows never ghosts us
    if (once) {
        while (!done) {
            ui.Poll();
            drawing.Poll();
            fflush(stdout);
            Sleep(16);
        }
        stop = true;
        while (!finished) {
            ui.Poll();
            drawing.Poll();
            Sleep(16);
        }
        worker.join();
        std::cout << "[run] --once: exiting after auto-execution" << std::endl;
        return (int)code;
    }
    std::cout << "[run] UI + overlay live. Use the Settings unload button or keybind to exit." << std::endl;
    while (!stop) {
        ui.Poll();
        drawing.Poll();
        Sleep(16);
    }
    std::cout << "[run] unload requested; shutting down." << std::endl;
    while (!done) { ui.Poll(); drawing.Poll(); Sleep(16); }
    stop = true;
    while (!finished) { ui.Poll(); drawing.Poll(); Sleep(16); }
    worker.join();
    std::cout << "[run] exiting" << std::endl;
    return (int)code;
}
