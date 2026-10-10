#pragma once
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <atomic>
#include <array>
#include <string>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <cstdint>
#include "explorer.h"

// Polling UI docked next to the Roblox window (TOPMOST tool window).
class UiManager {
public:
    UiManager() = default;
    ~UiManager() { Shutdown(); }

    bool Init(const std::string& title = "LUSTED", DWORD targetPid = 0);
    void Shutdown();
    void Poll(); // pump messages + dock next to Roblox
    bool IsOpen() const { return hwnd_ != nullptr; }
    void SetTargetPid(DWORD pid) { targetPid_.store(pid); }
    bool Visible() const { return visible_.load(); }
    void SetVisible(bool visible);
    void ToggleVisible();
    // Native toggle/unload keys, handled on the UI thread inside Poll. The
    // Lua keybind controls ("Toggle UI"/"Unload") only remap these; the action
    // itself never waits on the worker thread (index build, scripts, rescan).
    void SetStopFlag(std::atomic<bool>* flag) { stopFlag_ = flag; }
    void SetStatus(const std::string& text);

    void SetTab(const std::string& name);
    void SetSection(const std::string& name);
    int AddLabel(const std::string& text);
    void SetLabel(int id, const std::string& text);
    int AddButton(const std::string& label);
    bool IsButton(int id) const;
    bool IsPressable(int id) const;
    bool IsControl(int id) const;
    bool ButtonPressed(int id);
    bool TakeEvent(int id);
    int AddKeybind(const std::string& label, int defaultKey);
    int KeybindValue(int id);
    void SetKeybind(int id, int key);
    void SetDependency(int id, int sourceId, bool expected);
    int AddToggle(const std::string& label, bool def);
    bool ToggleState(int id);
    void SetToggle(int id, bool v);
    int AddSlider(const std::string& label, int mn, int mx, int def);
    int SliderValue(int id);
    void SetSlider(int id, int v);
    int AddDropdown(const std::string& label, const std::vector<std::string>& options, int selected);
    int DropdownValue(int id);
    void SetDropdown(int id, int selected);
    int AddInput(const std::string& label, const std::string& placeholder);
    std::string InputValue(int id);
    void SetInput(int id, const std::string& value);
    bool SaveConfig(const std::string& name) const;
    bool LoadConfig(const std::string& name);
    bool DeleteConfig(const std::string& name) const;
    std::vector<std::string> ConfigNames() const;

    // Standalone Explorer window (see explorer.h/cpp): tree + click-to-props.
    void SetExplorerStore(class InstanceStore* store) { explorer_.SetStore(store); }
    void SetExplorerPid(DWORD* pid) { explorer_.SetPidPtr(pid); }
    void SetExplorerOpen(bool open) { explorer_.SetOpen(open); }
    bool IsExplorerOpen() const { return explorer_.IsOpen(); }

private:
    struct Ctl {
        int kind = 0;
        std::string tab;
        std::string section;
        std::string text;
        std::string placeholder;
        std::vector<std::string> options;
        std::array<char, 256> buffer{};
        bool pressed = false;
        bool changed = false;
        bool toggle = false;
        bool binding = false;
        int mn = 0, mx = 100, val = 50, selected = 0;
        int key = 0, dependencyId = 0;
        bool dependencyValue = true;
    };
    void CreateRenderTarget();
    void CleanupRenderTarget();
    void Resize(UINT width, UINT height);
    void SyncSwapchain();
    void Render();
    static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l);
    static BOOL CALLBACK FindCb(HWND h, LPARAM l);
    HWND FindRobloxWindow();
    void DockToRoblox();

    HWND hwnd_ = nullptr;
    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    IDXGISwapChain* swapChain_ = nullptr;
    ID3D11RenderTargetView* renderTarget_ = nullptr;
    std::atomic<DWORD> targetPid_{ 0 };
    DWORD cachedPid_ = 0;
    HWND roblox_ = nullptr;
    uint64_t lastFind_ = 0;
    std::unordered_map<int, Ctl> ctls_;
    std::vector<int> controlOrder_;
    mutable std::mutex mtx_;
    int next_ = 1;
    std::string currentTab_ = "Main";
    std::string currentSection_ = "General";
    bool hasControls_ = false;
    std::atomic<bool> visible_{ false };
    std::atomic<int> toggleKey_{ VK_HOME };
    std::atomic<int> unloadKey_{ VK_END };
    std::atomic<bool>* stopFlag_{ nullptr };
    int toggleCtlId_{ 0 };
    int unloadCtlId_{ 0 };
    int statusCtlId_{ 0 };
    bool moving_ = false;
    int dockOffsetX_ = 0;
    int dockOffsetY_ = 0;
    bool imguiReady_ = false;
    bool keyStatesPrimed_ = false;
    std::array<bool, 256> keyDowns_{};
    std::array<bool, 256> keyPressedThisFrame_{};
    int swapW_ = 0, swapH_ = 0;
    Explorer explorer_;
};
