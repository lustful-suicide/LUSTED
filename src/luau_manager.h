#pragma once
#include <string>
#include <filesystem>
#include <unordered_map>
#include <atomic>
#include <windows.h>
struct lua_State;
class Memory;
class InstanceStore;
class UiManager;
class DrawingManager;

// Luau runtime: instance helpers + UI library + Drawing library.
// Auto-execution loads C:/LUSTED/Luas/*.luau (no manual REPL).
class LuauManager {
public:
    LuauManager(Memory* mem, InstanceStore* inst, DWORD* pidOut,
                UiManager* ui = nullptr, DrawingManager* dr = nullptr,
                std::atomic<bool>* stopRequest = nullptr);
    ~LuauManager() { Close(); }
    bool Init(const std::string& versionHash, const std::string& offsetsJsonPath);
    bool RunFile(const std::filesystem::path& file);
    bool RunString(const std::string& chunkName, const std::string& source);
    bool RegisterButtonCallback(int id, int functionIndex);
    bool RegisterControlCallback(int id, int functionIndex);
    // Per-frame tick callbacks (~60Hz from the worker Poll loop): the missing
    // primitive for server-fought features (noclip re-enforce on jump,
    // teleport spam against rubberband). Errors unregister + log, never loop.
    int RegisterTickCallback(int functionIndex);
    bool RemoveTickCallback(int id);
    bool RescanRoblox();
    bool RunAutoFile(const std::string& filename);
    void RequestUnload();
    void Poll();
    void Close();
    lua_State* State() const { return L_; }

private:
    void RegisterBindings();
    void RegisterUi();
    void RegisterDrawing();
    lua_State* L_ = nullptr;
    Memory* mem_ = nullptr;
    InstanceStore* inst_ = nullptr;
    DWORD* pidOut_ = nullptr;
    UiManager* ui_ = nullptr;
    DrawingManager* dr_ = nullptr;
    std::atomic<bool>* stopRequest_ = nullptr;
    std::string version_;
    std::string offsetsPath_;
    std::unordered_map<int, int> buttonCallbacks_;
    std::unordered_map<int, int> tickCallbacks_;
    int nextTickId_ = 1;
};
