#include "runtime/luau_manager.h"
#include "roblox/script_dump.h"
#include "platform/windows/memory.h"
#include "roblox/instance.h"
#include "ui/ui.h"
#include "ui/drawing.h"
#include "roblox/roblox_finder.h"
#include "roblox/offsets_fetcher.h"
#include "runtime/luau_stdlib.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <cstdio>
#include <cctype>
#include <vector>

#include "lua.h"
#include "lualib.h"
#include "Luau/Compiler.h"
#include <thread>
#include <chrono>

#include "runtime/luau_context.h"

Memory* g_mem = nullptr;
InstanceStore* g_inst = nullptr;
DWORD* g_pid = nullptr;
UiManager* g_ui = nullptr;
DrawingManager* g_dr = nullptr;
LuauManager* g_luau = nullptr;

LuauManager::LuauManager(Memory* m, InstanceStore* i, DWORD* p, UiManager* u, DrawingManager* d,
    std::atomic<bool>* stopRequest)
    : mem_(m), inst_(i), pidOut_(p), ui_(u), dr_(d), stopRequest_(stopRequest) {}

bool LuauManager::Init(const std::string& vh, const std::string& jp) {
    version_ = vh; offsetsPath_ = jp;
    g_mem = mem_; g_inst = inst_; g_pid = pidOut_; g_ui = ui_; g_dr = dr_;
    g_luau = this;
    // main() loads the table before the index thread starts; reloading here
    // would clear it (and the snapshot) under a running pass.
    if (g_inst && !g_inst->IsLoaded()) g_inst->Load(jp);
    L_ = luaL_newstate();
    if (!L_) return false;
    luaL_openlibs(L_);
    RegisterBindings();
    lua_pushstring(L_, version_.c_str()); lua_setglobal(L_, "VERSION_HASH");
    lua_pushstring(L_, offsetsPath_.c_str()); lua_setglobal(L_, "OFFSETS_PATH");
    if (!RunString("@builtin/helpers", kInstanceLibrary) ||
        !RunString("@builtin/ui", kUiLibrary) ||
        !RunString("@builtin/drawing", kDrawingLibrary)) return false;
    return true;
}

int l_luau_rescan(lua_State* L) {
    lua_pushboolean(L, g_luau && g_luau->RescanRoblox());
    return 1;
}
int l_luau_run_file(lua_State* L) {
    const char* filename = lua_tostring(L, 1);
    lua_pushboolean(L, g_luau && filename && g_luau->RunAutoFile(filename));
    return 1;
}
int l_request_unload(lua_State* L) {
    (void)L;
    if (g_luau) g_luau->RequestUnload();
    return 0;
}
int l_on_tick(lua_State* L) {
    // on_tick(fn) -> tick id (runs ~every frame), or nil.
    int id = (g_luau && lua_isfunction(L, 1)) ? g_luau->RegisterTickCallback(1) : 0;
    if (!id) lua_pushnil(L);
    else lua_pushnumber(L, id);
    return 1;
}
int l_remove_tick(lua_State* L) {
    lua_pushboolean(L, g_luau && g_luau->RemoveTickCallback((int)lua_tonumber(L, 1)));
    return 1;
}

bool LuauDecompileSelfTest(std::string& source) {
    source.clear();
    // A real chunk from the bundled compiler exercises the exact bytes the
    // API expects (header, string table, proto graph).
    Luau::CompileOptions co;
    std::string bc;
    try {
        bc = Luau::compile("local function add(a, b) return a + b end return add(40, 2)", co);
    } catch (...) {
        return false;
    }
    if (bc.empty()) return false;
    source = DecompileBytecode(std::vector<uint8_t>(bc.begin(), bc.end()), "selftest.luau");
    return !source.empty();
}
bool LuauManager::RunString(const std::string& chunk, const std::string& src) {
    Luau::CompileOptions co;
    std::string bc = Luau::compile(src, co);
    if (luau_load(L_, chunk.c_str(), bc.data(), bc.size(), 0) != 0) {
        std::cerr << "[luau] compile: " << lua_tostring(L_, -1) << "\n";
        lua_pop(L_, 1); return false;
    }
    if (lua_pcall(L_, 0, 0, 0) != 0) {
        std::cerr << "[luau] runtime: " << lua_tostring(L_, -1) << "\n";
        lua_pop(L_, 1); return false;
    }
    return true;
}
bool LuauManager::RunFile(const std::filesystem::path& f) {
    std::ifstream in(f);
    if (!in) { std::cerr << "[luau] cannot open " << f.string() << "\n"; return false; }
    std::stringstream ss; ss << in.rdbuf();
    return RunString("@" + f.string(), ss.str());
}
bool LuauManager::RunAutoFile(const std::string& filename) {
    std::filesystem::path leaf(filename);
    if (filename.empty() || leaf.has_parent_path() || leaf.filename() != leaf ||
        (leaf.extension() != ".luau" && leaf.extension() != ".lua")) return false;
    return RunFile(std::filesystem::path("C:/LUSTED/Luas") / leaf);
}
void LuauManager::ResetSessionScripts() {
    if (L_) {
        for (const auto& callback : buttonCallbacks_) lua_unref(L_, callback.second);
        buttonCallbacks_.clear();
        for (const auto& tick : tickCallbacks_) lua_unref(L_, tick.second);
        tickCallbacks_.clear();
    }
    if (dr_) dr_->Clear();
}
void LuauManager::SetTargetPid(DWORD pid) {
    if (ui_) ui_->SetTargetPid(pid);
    if (dr_) dr_->SetTargetPid(pid);
}
bool LuauManager::RescanRoblox() {
    // A background index pass may be merging while this swaps the store;
    // cancelling first keeps the move from racing the merge.
    if (inst_) {
        inst_->RequestIndexCancel();
        while (inst_->IsIndexBuilding())
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    auto install = FindRobloxPlayer();
    if (!install || !install->pid) return false;
    const DWORD newPid = install->pid;
    const std::string newVersion = install->versionHash.empty() ? version_ : install->versionHash;
    std::filesystem::path newOffsetsPath = offsetsPath_;

    if (newVersion != version_) {
        OffsetsFetcher fetcher(newVersion);
        OffsetBundle bundle = fetcher.FetchAll();
        if (!bundle.ok) return false;
        newOffsetsPath = bundle.offsetsJson;
    }

    InstanceStore refreshed(inst_ ? mem_ : nullptr);
    if (!inst_ || !refreshed.Load(newOffsetsPath.string())) return false;

    const DWORD oldPid = pidOut_ ? *pidOut_ : 0;
    bool attached = true;
    auto replaceStore = [&]() {
        if (mem_ && (!mem_->IsOpen() || oldPid != newPid) && !mem_->Attach(newPid)) {
            if (oldPid) mem_->Attach(oldPid);
            attached = false;
            return;
        }
        *inst_ = std::move(refreshed);
        if (pidOut_) *pidOut_ = newPid;
    };
    if (ui_) ui_->WithExplorerStoreExclusive(replaceStore);
    else replaceStore();
    if (!attached) return false;
    if (ui_) ui_->SetTargetPid(newPid);
    if (dr_) dr_->SetTargetPid(newPid);
    version_ = newVersion;
    offsetsPath_ = newOffsetsPath.string();
    lua_pushstring(L_, version_.c_str()); lua_setglobal(L_, "VERSION_HASH");
    lua_pushstring(L_, offsetsPath_.c_str()); lua_setglobal(L_, "OFFSETS_PATH");
    std::cout << "[rescan] attached to pid " << newPid << " version-" << version_ << "\n";
    return true;
}
void LuauManager::RequestUnload() {
    if (stopRequest_) stopRequest_->store(true);
}
bool LuauManager::RegisterButtonCallback(int id, int functionIndex) {
    if (!ui_ || !ui_->IsPressable(id)) return false;
    return RegisterControlCallback(id, functionIndex);
}
bool LuauManager::RegisterControlCallback(int id, int functionIndex) {
    if (!L_ || !ui_ || !ui_->IsControl(id) || !lua_isfunction(L_, functionIndex)) return false;
    int ref = lua_ref(L_, functionIndex);
    if (ref == LUA_REFNIL) return false;
    auto it = buttonCallbacks_.find(id);
    if (it != buttonCallbacks_.end()) lua_unref(L_, it->second);
    buttonCallbacks_[id] = ref;
    return true;
}
int LuauManager::RegisterTickCallback(int functionIndex) {
    if (!L_ || !lua_isfunction(L_, functionIndex)) return 0;
    int ref = lua_ref(L_, functionIndex);
    if (ref == LUA_REFNIL) return 0;
    int id = nextTickId_++;
    tickCallbacks_[id] = ref;
    return id;
}
bool LuauManager::RemoveTickCallback(int id) {
    auto it = tickCallbacks_.find(id);
    if (it == tickCallbacks_.end()) return false;
    if (L_) lua_unref(L_, it->second);
    tickCallbacks_.erase(it);
    return true;
}
void LuauManager::Poll() {
    if (!L_) return;
    if (ui_ && !buttonCallbacks_.empty()) {
        std::vector<std::pair<int, int>> callbacks(buttonCallbacks_.begin(), buttonCallbacks_.end());
        for (const auto& [id, ref] : callbacks) {
            if (!ui_->TakeEvent(id)) continue;
            auto it = buttonCallbacks_.find(id);
            if (it == buttonCallbacks_.end() || it->second != ref) continue;
            lua_getref(L_, ref);
            lua_pushnumber(L_, id);
            if (lua_pcall(L_, 1, 0, 0) != 0) {
                std::cerr << "[luau] UI callback: " << lua_tostring(L_, -1) << "\n";
                lua_pop(L_, 1);
            }
        }
    }
    if (!tickCallbacks_.empty()) {
        std::vector<std::pair<int, int>> ticks(tickCallbacks_.begin(), tickCallbacks_.end());
        std::vector<int> dead;
        for (const auto& [id, ref] : ticks) {
            auto it = tickCallbacks_.find(id);
            if (it == tickCallbacks_.end() || it->second != ref) continue;
            lua_getref(L_, ref);
            int pr = lua_pcall(L_, 0, 0, 0);
            if (pr != 0) {
                std::cerr << "[luau] tick " << id << ": " << lua_tostring(L_, -1) << "\n";
                lua_pop(L_, 1);
                dead.push_back(id);
            }
        }
        for (int id : dead) RemoveTickCallback(id);
    }
}
void LuauManager::Close() {
    if (L_) {
        for (const auto& callback : buttonCallbacks_) lua_unref(L_, callback.second);
        buttonCallbacks_.clear();
        for (const auto& tick : tickCallbacks_) lua_unref(L_, tick.second);
        tickCallbacks_.clear();
    }
    if (L_) { lua_close(L_); L_ = nullptr; }
    if (g_mem == mem_) g_mem = nullptr;
    if (g_inst == inst_) g_inst = nullptr;
    if (g_ui == ui_) g_ui = nullptr;
    if (g_dr == dr_) g_dr = nullptr;
    if (g_luau == this) g_luau = nullptr;
}
