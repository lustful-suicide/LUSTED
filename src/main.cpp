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
        if (!e.is_regular_file(ec)) continue;
        auto ext = e.path().extension().string();
        if (ext == ".luau" || ext == ".lua") out.push_back(e.path());
    }
    std::sort(out.begin(), out.end());
    return out;
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
        if (!luau.Init(versionHash, bundle.offsetsJson.string())) {
            std::cerr << "luau init failed" << std::endl; code = 1; done = true; finished = true; return;
        }
        LustedLog("luau init ok");
        // Index after Init: Load() resets the snapshot, so building earlier
        // would leave the store with an empty index.
        if (pid && mem.IsOpen()) {
            inst.BuildIndex(pid, true);
            LustedLogf("index build done ready=%d", inst.IsIndexReady() ? 1 : 0);
        }
        std::filesystem::path autoDir("C:/LUSTED/Luas");
        std::error_code ec;
        std::filesystem::create_directories(autoDir, ec);
        auto luas = CollectLuas(autoDir);
        std::cout << "[auto] " << luas.size() << " file(s) in " << autoDir.string() << std::endl;
        LustedLogf("auto-exec start files=%zu", luas.size());
        for (auto& f : luas) {
            std::cout << "[auto] running " << f.string() << std::endl;
            LustedLogf("script start %s", f.filename().string().c_str());
            bool ok = luau.RunFile(f);
            LustedLogf("script end %s ok=%d", f.filename().string().c_str(), ok ? 1 : 0);
            if (!ok) std::cerr << "[auto] failed: " << f.string() << std::endl;
        }
        std::cout << "[run] auto-execution done." << std::endl;
        LustedLog("auto-exec done");
        done = true;
        while (!once && !stop) {
            luau.Poll();
            Sleep(16);
        }
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
