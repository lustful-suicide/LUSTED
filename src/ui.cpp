#include "ui.h"
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include <windowsx.h>
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <unordered_set>
#include <nlohmann/json.hpp>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

static UiManager* g_ui = nullptr;
struct FindCtx { DWORD pid; HWND hwnd; };

// Class/title are process-lifetime strings; nothing outside this file looks
// them up by name (FindCb matches on pid only), so they can differ per run.
static const char* RandomClassName() {
    static std::string s = [] {
        char b[48];
        snprintf(b, sizeof(b), "Wnd%08X%04X",
                 (unsigned)(GetTickCount64() & 0xFFFFFFFF),
                 (unsigned)(GetCurrentProcessId() & 0xFFFF));
        return std::string(b);
    }();
    return s.c_str();
}
static std::string RandomWindowTitle() {
    char b[48];
    snprintf(b, sizeof(b), "Roblox%08X", (unsigned)(GetTickCount64() & 0xFFFFFFFF));
    return std::string(b);
}

static std::filesystem::path ConfigDirectory() {
    return std::filesystem::path("C:/LUSTED/configs");
}

static bool IsValidConfigName(const std::string& name) {
    return !name.empty() && name.size() <= 48 &&
        std::all_of(name.begin(), name.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9') || c == '_' || c == '-';
        });
}

static std::string ConfigControlIdentity(int kind, const std::string& tab,
    const std::string& section, const std::string& label, int occurrence) {
    return std::to_string(kind) + "\x1f" + tab + "\x1f" + section + "\x1f" + label +
        "\x1f" + std::to_string(occurrence);
}

static bool GetClientScreenBounds(HWND hwnd, RECT& bounds) {
    RECT client{};
    if (!GetClientRect(hwnd, &client)) return false;
    POINT topLeft{ client.left, client.top };
    POINT bottomRight{ client.right, client.bottom };
    if (!ClientToScreen(hwnd, &topLeft) || !ClientToScreen(hwnd, &bottomRight)) return false;
    bounds = { topLeft.x, topLeft.y, bottomRight.x, bottomRight.y };
    return bounds.right > bounds.left && bounds.bottom > bounds.top;
}

static void ClampWindowRect(RECT& window, const RECT& bounds) {
    int width = (std::min)(window.right - window.left, bounds.right - bounds.left);
    int height = (std::min)(window.bottom - window.top, bounds.bottom - bounds.top);
    window.left = (std::clamp)(window.left, bounds.left, bounds.right - width);
    window.top = (std::clamp)(window.top, bounds.top, bounds.bottom - height);
    window.right = window.left + width;
    window.bottom = window.top + height;
}

static std::string KeyName(int key) {
    if (key <= 0 || key > 255) return "None";
    if (key == VK_HOME) return "Home";
    if (key == VK_END) return "End";
    if (key == VK_PRIOR) return "Page Up";
    if (key == VK_NEXT) return "Page Down";
    if (key == VK_LEFT) return "Left";
    if (key == VK_RIGHT) return "Right";
    if (key == VK_UP) return "Up";
    if (key == VK_DOWN) return "Down";
    UINT scan = MapVirtualKeyA((UINT)key, MAPVK_VK_TO_VSC);
    char name[80]{};
    if (GetKeyNameTextA((LONG)(scan << 16), name, sizeof(name))) return name;
    return "Key " + std::to_string(key);
}

LRESULT CALLBACK UiManager::WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (g_ui && g_ui->imguiReady_ && ImGui_ImplWin32_WndProcHandler(h, m, w, l))
        return TRUE;

    if (g_ui && m == WM_NCHITTEST) {
        POINT cursor{ GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        ScreenToClient(h, &cursor);
        if (cursor.y >= 0 && cursor.y < 28) return HTCAPTION;
    }
    if (g_ui && m == WM_MOVING) {
        HWND target = g_ui->FindRobloxWindow();
        RECT clientBounds{};
        if (target && !IsIconic(target) && GetClientScreenBounds(target, clientBounds)) {
            ClampWindowRect(*(RECT*)l, clientBounds);
            return TRUE;
        }
    }
    if (g_ui && m == WM_ENTERSIZEMOVE) g_ui->moving_ = true;
    if (g_ui && m == WM_EXITSIZEMOVE) {
        g_ui->moving_ = false;
        RECT current{};
        GetWindowRect(h, &current);
        int baseX = 0, baseY = 0;
        HWND target = g_ui->FindRobloxWindow();
        if (target && !IsIconic(target)) {
            RECT targetRect{};
            if (GetClientScreenBounds(target, targetRect)) {
                baseX = targetRect.right - (current.right - current.left) - 16;
                baseY = targetRect.top + 16;
            }
        }
        g_ui->dockOffsetX_ = current.left - baseX;
        g_ui->dockOffsetY_ = current.top - baseY;
    }
    if (g_ui && m == WM_SIZE && w != SIZE_MINIMIZED) {
        g_ui->Resize((UINT)LOWORD(l), (UINT)HIWORD(l));
        return 0;
    }
    if (g_ui && m == WM_ERASEBKGND) return 1;
    if (g_ui && m == WM_CLOSE) {
        g_ui->visible_.store(false);
        ShowWindow(h, SW_HIDE);
        return 0;
    }
    if (g_ui && m == WM_KILLFOCUS) {
        g_ui->keyStatesPrimed_ = false;
    }
    if (g_ui && m == WM_APP + 1) {
        bool hadControls = g_ui->hasControls_;
        {
            std::lock_guard<std::mutex> lk(g_ui->mtx_);
            g_ui->hasControls_ = !g_ui->controlOrder_.empty();
        }
        if (!hadControls && g_ui->hasControls_) g_ui->visible_ = true;
        g_ui->DockToRoblox();
        return 0;
    }
    if (g_ui && m == WM_APP + 4) {
        g_ui->visible_.store(w != 0);
        if (g_ui->visible_.load()) g_ui->DockToRoblox();
        else ShowWindow(h, SW_HIDE);
        return 0;
    }
    if (m == WM_DESTROY) {
        if (g_ui) g_ui->hwnd_ = nullptr;
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

BOOL CALLBACK UiManager::FindCb(HWND h, LPARAM l) {
    FindCtx* c = (FindCtx*)l;
    DWORD wpid = 0;
    GetWindowThreadProcessId(h, &wpid);
    if (wpid != c->pid) return TRUE;
    if (!IsWindowVisible(h)) return TRUE;
    RECT rc{};
    GetWindowRect(h, &rc);
    if ((rc.right - rc.left) < 400 || (rc.bottom - rc.top) < 300) return TRUE;
    c->hwnd = h;
    return FALSE;
}

HWND UiManager::FindRobloxWindow() {
    DWORD targetPid = targetPid_.load();
    if (cachedPid_ != targetPid) {
        cachedPid_ = targetPid;
        roblox_ = nullptr;
        lastFind_ = 0;
    }
    if (!targetPid) return nullptr;
    uint64_t now = GetTickCount64();
    if (roblox_ && IsWindow(roblox_) && now - lastFind_ < 2000) return roblox_;
    lastFind_ = now;
    FindCtx c{ targetPid, nullptr };
    EnumWindows(FindCb, (LPARAM)&c);
    if (c.hwnd) roblox_ = c.hwnd;
    return roblox_;
}

void UiManager::DockToRoblox() {
    if (!hwnd_ || !hasControls_ || !visible_.load() || moving_) return;
    HWND r = FindRobloxWindow();
    int baseX = 0, baseY = 0;
    int width = 460, height = 610;
    RECT bounds{};
    bool constrained = false;
    if (r && !IsIconic(r)) {
        if (GetClientScreenBounds(r, bounds)) {
            constrained = true;
            width = (std::min)(width, (int)(bounds.right - bounds.left));
            height = (std::min)(height, (int)(bounds.bottom - bounds.top));
            baseX = bounds.right - width - 16;
            baseY = bounds.top + 16;
        }
    } else if (r && IsIconic(r)) {
        ShowWindow(hwnd_, SW_HIDE);
        return;
    }
    RECT desired{ baseX + dockOffsetX_, baseY + dockOffsetY_,
        baseX + dockOffsetX_ + width, baseY + dockOffsetY_ + height };
    if (constrained) ClampWindowRect(desired, bounds);
    RECT current{};
    GetWindowRect(hwnd_, &current);
    if (!IsWindowVisible(hwnd_) || current.left != desired.left || current.top != desired.top ||
        current.right - current.left != desired.right - desired.left ||
        current.bottom - current.top != desired.bottom - desired.top)
        SetWindowPos(hwnd_, HWND_TOPMOST, desired.left, desired.top,
            desired.right - desired.left, desired.bottom - desired.top, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void UiManager::CreateRenderTarget() {
    if (!swapChain_ || renderTarget_) return;
    ID3D11Texture2D* backBuffer = nullptr;
    if (SUCCEEDED(swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer)))) {
        device_->CreateRenderTargetView(backBuffer, nullptr, &renderTarget_);
        backBuffer->Release();
    }
}

void UiManager::CleanupRenderTarget() {
    if (!renderTarget_) return;
    renderTarget_->Release();
    renderTarget_ = nullptr;
}

void UiManager::Resize(UINT width, UINT height) {
    if (!swapChain_ || !width || !height) return;
    CleanupRenderTarget();
    if (SUCCEEDED(swapChain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0)))
        CreateRenderTarget();
}

bool UiManager::Init(const std::string& title, DWORD pid) {
    if (hwnd_) { targetPid_.store(pid); return true; }
    targetPid_.store(pid);
    g_ui = this;
    WNDCLASSA wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "LustedImGuiClass";
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.hbrBackground = nullptr;
    RegisterClassA(&wc);
    hwnd_ = CreateWindowExA(WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        "LustedImGuiClass", title.c_str(),
        WS_POPUP,
        CW_USEDEFAULT, CW_USEDEFAULT, 460, 610, nullptr, nullptr,
        GetModuleHandleA(nullptr), nullptr);
    if (!hwnd_) return false;

    DXGI_SWAP_CHAIN_DESC swapDesc{};
    swapDesc.BufferCount = 2;
    swapDesc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapDesc.BufferDesc.RefreshRate.Numerator = 60;
    swapDesc.BufferDesc.RefreshRate.Denominator = 1;
    swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapDesc.OutputWindow = hwnd_;
    swapDesc.SampleDesc.Count = 1;
    swapDesc.Windowed = TRUE;
    swapDesc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    D3D_FEATURE_LEVEL featureLevel{};
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    HRESULT result = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        0, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &swapDesc, &swapChain_, &device_,
        &featureLevel, &context_);
    if (FAILED(result)) {
        result = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            0, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &swapDesc, &swapChain_, &device_,
            &featureLevel, &context_);
    }
    if (FAILED(result)) {
        Shutdown();
        return false;
    }
    CreateRenderTarget();
    if (!renderTarget_) {
        Shutdown();
        return false;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad; // XInput throws with no pad
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 5.0f;
    style.FrameRounding = 3.0f;
    style.GrabRounding = 3.0f;
    style.WindowPadding = ImVec2(14.0f, 12.0f);
    style.ItemSpacing = ImVec2(8.0f, 8.0f);
    ImVec4* colors = style.Colors;
    colors[ImGuiCol_WindowBg] = ImVec4(0.055f, 0.065f, 0.073f, 1.0f);
    colors[ImGuiCol_ChildBg] = ImVec4(0.075f, 0.088f, 0.095f, 1.0f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.12f, 0.14f, 0.15f, 1.0f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.16f, 0.21f, 0.21f, 1.0f);
    colors[ImGuiCol_Button] = ImVec4(0.12f, 0.31f, 0.29f, 1.0f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.16f, 0.42f, 0.38f, 1.0f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.10f, 0.27f, 0.25f, 1.0f);
    colors[ImGuiCol_Header] = ImVec4(0.12f, 0.31f, 0.29f, 0.75f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.16f, 0.42f, 0.38f, 0.85f);
    colors[ImGuiCol_CheckMark] = ImVec4(0.30f, 0.82f, 0.69f, 1.0f);
    colors[ImGuiCol_SliderGrab] = ImVec4(0.25f, 0.67f, 0.58f, 1.0f);
    colors[ImGuiCol_SliderGrabActive] = ImVec4(0.35f, 0.86f, 0.73f, 1.0f);
    if (!ImGui_ImplWin32_Init(hwnd_)) {
        ImGui::DestroyContext();
        Shutdown();
        return false;
    }
    if (!ImGui_ImplDX11_Init(device_, context_)) {
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        Shutdown();
        return false;
    }
    imguiReady_ = true;
    ShowWindow(hwnd_, SW_HIDE);
    return true;
}

void UiManager::Shutdown() {
    if (imguiReady_) {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        imguiReady_ = false;
    }
    CleanupRenderTarget();
    if (swapChain_) { swapChain_->Release(); swapChain_ = nullptr; }
    if (context_) { context_->Release(); context_ = nullptr; }
    if (device_) { device_->Release(); device_ = nullptr; }
    if (hwnd_) { DestroyWindow(hwnd_); hwnd_ = nullptr; }
    g_ui = nullptr;
}

void UiManager::Poll() {
    MSG msg;
    while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    DockToRoblox();
    {
        std::lock_guard<std::mutex> lk(mtx_);
        std::array<bool, 256> keysToPoll{};
        bool captureKey = false;
        for (const auto& item : ctls_) {
            if (item.second.kind != 6) continue;
            if (item.second.binding) captureKey = true;
            else if (item.second.key > 0 && item.second.key < 256) keysToPoll[item.second.key] = true;
        }
        if (captureKey) keysToPoll.fill(true);
        keyPressedThisFrame_.fill(false);
        for (int key = 1; key < 256; ++key) {
            if (!keysToPoll[key]) continue;
            bool down = (GetAsyncKeyState(key) & 0x8000) != 0;
            keyPressedThisFrame_[key] = keyStatesPrimed_ && down && !keyDowns_[key];
            keyDowns_[key] = down;
        }
        if (keyStatesPrimed_) {
            for (auto& item : ctls_) {
                Ctl& c = item.second;
                if (c.kind != 6) continue;
                if (c.binding) {
                    for (int key = 8; key < 256; ++key) {
                        if (!keyPressedThisFrame_[key] || key == VK_HOME || key == VK_END ||
                            key == VK_LWIN || key == VK_RWIN) continue;
                        c.key = key;
                        c.binding = false;
                        break;
                    }
                } else if (c.key > 0 && c.key < 256 && keyPressedThisFrame_[c.key]) {
                    c.pressed = true;
                }
            }
        }
        keyStatesPrimed_ = true;
    }
    if (visible_.load() && imguiReady_ && !IsIconic(hwnd_)) {
        // ImGui's Win32 backend calls XInput, which throws when no controller
        // is present (0xc06d007e). Never let that kill the process.
        try { Render(); } catch (...) { imguiReady_ = imguiReady_; }
    }
}

void UiManager::SetVisible(bool visible) {
    if (hwnd_) PostMessageA(hwnd_, WM_APP + 4, visible ? 1 : 0, 0);
}
void UiManager::ToggleVisible() {
    SetVisible(!visible_.load());
}

void UiManager::Render() {
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin("LUSTED", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);
    ImGui::BeginChild("##LustedContent", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None,
        ImGuiWindowFlags_AlwaysVerticalScrollbar);

    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<std::string> tabs;
    std::unordered_set<std::string> seenTabs;
    for (int id : controlOrder_) {
        auto it = ctls_.find(id);
        if (it != ctls_.end() && it->second.tab != "Settings" && seenTabs.insert(it->second.tab).second)
            tabs.push_back(it->second.tab);
    }
    tabs.push_back("Settings");
    if (ImGui::BeginTabBar("##LustedTabs")) {
        for (const auto& tab : tabs) {
            if (!ImGui::BeginTabItem(tab.c_str())) continue;
            std::string section;
            for (int id : controlOrder_) {
                auto it = ctls_.find(id);
                if (it == ctls_.end() || it->second.tab != tab) continue;
                Ctl& c = it->second;
                if (c.dependencyId) {
                    auto dependency = ctls_.find(c.dependencyId);
                    if (dependency == ctls_.end() || dependency->second.kind != 2 ||
                        dependency->second.toggle != c.dependencyValue) continue;
                }
                if (section != c.section) {
                    if (!section.empty()) ImGui::Spacing();
                    section = c.section;
                    ImGui::SeparatorText(section.c_str());
                }
                ImGui::PushID(id);
                if (c.kind == 0) {
                    ImGui::TextWrapped("%s", c.text.c_str());
                } else if (c.kind == 1) {
                    if (ImGui::Button(c.text.c_str(), ImVec2(-FLT_MIN, 0.0f))) c.pressed = true;
                } else if (c.kind == 2) {
                    if (ImGui::Checkbox(c.text.c_str(), &c.toggle)) c.changed = true;
                } else if (c.kind == 3) {
                    if (ImGui::SliderInt(c.text.c_str(), &c.val, c.mn, c.mx)) c.changed = true;
                } else if (c.kind == 4) {
                    std::vector<const char*> items;
                    items.reserve(c.options.size());
                    for (const auto& option : c.options) items.push_back(option.c_str());
                    if (!items.empty() && ImGui::Combo(c.text.c_str(), &c.selected, items.data(), (int)items.size()))
                        c.changed = true;
                } else if (c.kind == 5) {
                    if (ImGui::InputTextWithHint(c.text.c_str(), c.placeholder.c_str(), c.buffer.data(), c.buffer.size()))
                        c.changed = true;
                } else if (c.kind == 6) {
                    ImGui::TextUnformatted(c.text.c_str());
                    ImGui::SameLine();
                    if (ImGui::Button(c.binding ? "Press a key" : KeyName(c.key).c_str(), ImVec2(110.0f, 0.0f)))
                        c.binding = true;
                }
                ImGui::PopID();
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();
    ImGui::End();
    ImGui::Render();
    const float clearColor[4] = { 0.055f, 0.065f, 0.073f, 1.0f };
    context_->OMSetRenderTargets(1, &renderTarget_, nullptr);
    context_->ClearRenderTargetView(renderTarget_, clearColor);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    swapChain_->Present(0, 0);
}

void UiManager::SetTab(const std::string& name) {
    std::lock_guard<std::mutex> lk(mtx_);
    currentTab_ = name.empty() ? "Main" : name;
    currentSection_ = "General";
}
void UiManager::SetSection(const std::string& name) {
    std::lock_guard<std::mutex> lk(mtx_);
    currentSection_ = name.empty() ? "General" : name;
}
int UiManager::AddLabel(const std::string& t) {
    int id;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        id = next_++;
        Ctl c; c.kind = 0; c.tab = currentTab_; c.section = currentSection_; c.text = t;
        ctls_[id] = c;
        controlOrder_.push_back(id);
    }
    if (hwnd_) SendMessageA(hwnd_, WM_APP + 1, id, 0);
    return id;
}
void UiManager::SetLabel(int id, const std::string& text) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = ctls_.find(id);
    if (it != ctls_.end() && it->second.kind == 0) it->second.text = text;
}
int UiManager::AddButton(const std::string& t) {
    int id;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        id = next_++;
        Ctl c; c.kind = 1; c.tab = currentTab_; c.section = currentSection_; c.text = t;
        ctls_[id] = c;
        controlOrder_.push_back(id);
    }
    if (hwnd_) SendMessageA(hwnd_, WM_APP + 1, id, 0);
    return id;
}
bool UiManager::IsButton(int id) const {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = ctls_.find(id);
    return it != ctls_.end() && it->second.kind == 1;
}
bool UiManager::IsPressable(int id) const {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = ctls_.find(id);
    return it != ctls_.end() && (it->second.kind == 1 || it->second.kind == 6);
}
bool UiManager::IsControl(int id) const {
    std::lock_guard<std::mutex> lk(mtx_);
    return ctls_.find(id) != ctls_.end();
}
bool UiManager::ButtonPressed(int id) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = ctls_.find(id);
    if (it == ctls_.end()) return false;
    bool p = it->second.pressed;
    it->second.pressed = false;
    return p;
}
bool UiManager::TakeEvent(int id) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = ctls_.find(id);
    if (it == ctls_.end()) return false;
    bool isPressControl = it->second.kind == 1 || it->second.kind == 6;
    bool occurred = isPressControl ? it->second.pressed : it->second.changed;
    it->second.pressed = false;
    it->second.changed = false;
    return occurred;
}
int UiManager::AddKeybind(const std::string& label, int defaultKey) {
    std::lock_guard<std::mutex> lk(mtx_);
    int id = next_++;
    Ctl c; c.kind = 6; c.tab = currentTab_; c.section = currentSection_; c.text = label;
    c.key = (std::clamp)(defaultKey, 0, 255);
    ctls_[id] = std::move(c);
    controlOrder_.push_back(id);
    if (hwnd_) PostMessageA(hwnd_, WM_APP + 1, id, 0);
    return id;
}
int UiManager::KeybindValue(int id) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = ctls_.find(id);
    return it == ctls_.end() || it->second.kind != 6 ? 0 : it->second.key;
}
void UiManager::SetKeybind(int id, int key) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = ctls_.find(id);
    if (it == ctls_.end() || it->second.kind != 6) return;
    it->second.key = (std::clamp)(key, 0, 255);
}
void UiManager::SetDependency(int id, int sourceId, bool expected) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = ctls_.find(id);
    auto source = ctls_.find(sourceId);
    if (it == ctls_.end() || source == ctls_.end() || source->second.kind != 2) return;
    it->second.dependencyId = sourceId;
    it->second.dependencyValue = expected;
}
int UiManager::AddToggle(const std::string& t, bool d) {
    int id;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        id = next_++;
        Ctl c; c.kind = 2; c.tab = currentTab_; c.section = currentSection_; c.text = t; c.toggle = d;
        ctls_[id] = c;
        controlOrder_.push_back(id);
    }
    if (hwnd_) SendMessageA(hwnd_, WM_APP + 1, id, 0);
    return id;
}
bool UiManager::ToggleState(int id) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = ctls_.find(id);
    return it == ctls_.end() ? false : it->second.toggle;
}
void UiManager::SetToggle(int id, bool v) {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = ctls_.find(id);
        if (it == ctls_.end() || it->second.kind != 2) return;
        it->second.toggle = v;
    }
    if (hwnd_) SendMessageA(hwnd_, WM_APP + 2, id, v ? 1 : 0);
}
int UiManager::AddSlider(const std::string& t, int mn, int mx, int d) {
    int id;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        id = next_++;
        Ctl c; c.kind = 3; c.tab = currentTab_; c.section = currentSection_; c.text = t;
        c.mn = (std::min)(mn, mx); c.mx = (std::max)(mn, mx);
        c.val = (std::clamp)(d, c.mn, c.mx);
        ctls_[id] = c;
        controlOrder_.push_back(id);
    }
    if (hwnd_) SendMessageA(hwnd_, WM_APP + 1, id, 0);
    return id;
}
int UiManager::SliderValue(int id) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = ctls_.find(id);
    return it == ctls_.end() ? 0 : it->second.val;
}
void UiManager::SetSlider(int id, int v) {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = ctls_.find(id);
        if (it == ctls_.end() || it->second.kind != 3) return;
        it->second.val = (std::clamp)(v, it->second.mn, it->second.mx);
        v = it->second.val;
    }
    if (hwnd_) SendMessageA(hwnd_, WM_APP + 3, id, v);
}

int UiManager::AddDropdown(const std::string& label, const std::vector<std::string>& options, int selected) {
    std::lock_guard<std::mutex> lk(mtx_);
    int id = next_++;
    Ctl c; c.kind = 4; c.tab = currentTab_; c.section = currentSection_; c.text = label;
    c.options = options;
    c.selected = c.options.empty() ? 0 : (std::clamp)(selected, 0, (int)c.options.size() - 1);
    ctls_[id] = std::move(c);
    controlOrder_.push_back(id);
    if (hwnd_) PostMessageA(hwnd_, WM_APP + 1, id, 0);
    return id;
}
int UiManager::DropdownValue(int id) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = ctls_.find(id);
    return it == ctls_.end() || it->second.kind != 4 ? -1 : it->second.selected;
}
void UiManager::SetDropdown(int id, int selected) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = ctls_.find(id);
    if (it == ctls_.end() || it->second.kind != 4 || it->second.options.empty()) return;
    it->second.selected = (std::clamp)(selected, 0, (int)it->second.options.size() - 1);
}
int UiManager::AddInput(const std::string& label, const std::string& placeholder) {
    std::lock_guard<std::mutex> lk(mtx_);
    int id = next_++;
    Ctl c; c.kind = 5; c.tab = currentTab_; c.section = currentSection_;
    c.text = label; c.placeholder = placeholder;
    ctls_[id] = std::move(c);
    controlOrder_.push_back(id);
    if (hwnd_) PostMessageA(hwnd_, WM_APP + 1, id, 0);
    return id;
}
std::string UiManager::InputValue(int id) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = ctls_.find(id);
    return it == ctls_.end() || it->second.kind != 5 ? std::string() : it->second.buffer.data();
}
void UiManager::SetInput(int id, const std::string& value) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = ctls_.find(id);
    if (it == ctls_.end() || it->second.kind != 5) return;
    strncpy_s(it->second.buffer.data(), it->second.buffer.size(), value.c_str(), _TRUNCATE);
}

bool UiManager::SaveConfig(const std::string& name) const {
    if (!IsValidConfigName(name)) return false;
    nlohmann::json config = nlohmann::json::object();
    std::unordered_map<std::string, int> occurrences;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        for (int id : controlOrder_) {
            auto it = ctls_.find(id);
            if (it == ctls_.end()) continue;
            const Ctl& c = it->second;
            if (c.kind < 2 || c.kind > 6) continue;
            std::string identity = std::to_string(c.kind) + "\x1f" + c.tab + "\x1f" + c.section + "\x1f" + c.text;
            std::string key = ConfigControlIdentity(c.kind, c.tab, c.section, c.text, occurrences[identity]++);
            if (c.kind == 2) config[key] = c.toggle;
            else if (c.kind == 3) config[key] = c.val;
            else if (c.kind == 4) config[key] = c.selected;
            else if (c.kind == 5) config[key] = c.buffer.data();
            else if (c.kind == 6) config[key] = c.key;
        }
    }

    std::error_code ec;
    auto directory = ConfigDirectory();
    std::filesystem::create_directories(directory, ec);
    if (ec) return false;
    auto destination = directory / (name + ".json");
    auto temporary = directory / (name + ".tmp");
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << config.dump(2);
        if (!out) { out.close(); std::filesystem::remove(temporary, ec); return false; }
    }
    if (!MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::filesystem::remove(temporary, ec);
        return false;
    }
    return true;
}

bool UiManager::LoadConfig(const std::string& name) {
    if (!IsValidConfigName(name)) return false;
    std::ifstream in(ConfigDirectory() / (name + ".json"), std::ios::binary);
    if (!in) return false;
    nlohmann::json config;
    try { in >> config; } catch (...) { return false; }
    if (!config.is_object()) return false;

    std::lock_guard<std::mutex> lk(mtx_);
    std::unordered_map<std::string, int> occurrences;
    for (int id : controlOrder_) {
        auto it = ctls_.find(id);
        if (it == ctls_.end()) continue;
        Ctl& c = it->second;
        if (c.kind < 2 || c.kind > 6) continue;
        std::string identity = std::to_string(c.kind) + "\x1f" + c.tab + "\x1f" + c.section + "\x1f" + c.text;
        std::string key = ConfigControlIdentity(c.kind, c.tab, c.section, c.text, occurrences[identity]++);
        auto value = config.find(key);
        if (value == config.end()) continue;
        try {
            if (c.kind == 2 && value->is_boolean()) c.toggle = value->get<bool>();
            else if (c.kind == 3 && value->is_number_integer()) c.val = (std::clamp)(value->get<int>(), c.mn, c.mx);
            else if (c.kind == 4 && value->is_number_integer() && !c.options.empty())
                c.selected = (std::clamp)(value->get<int>(), 0, (int)c.options.size() - 1);
            else if (c.kind == 5 && value->is_string())
                strncpy_s(c.buffer.data(), c.buffer.size(), value->get<std::string>().c_str(), _TRUNCATE);
            else if (c.kind == 6 && value->is_number_integer()) c.key = (std::clamp)(value->get<int>(), 0, 255);
        } catch (...) { return false; }
    }
    return true;
}

bool UiManager::DeleteConfig(const std::string& name) const {
    if (!IsValidConfigName(name)) return false;
    std::error_code ec;
    bool removed = std::filesystem::remove(ConfigDirectory() / (name + ".json"), ec);
    return removed && !ec;
}

std::vector<std::string> UiManager::ConfigNames() const {
    std::vector<std::string> names;
    std::error_code ec;
    const auto directory = ConfigDirectory();
    if (!std::filesystem::exists(directory, ec)) return names;
    for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec) || entry.path().extension() != ".json") continue;
        const std::string name = entry.path().stem().string();
        if (IsValidConfigName(name)) names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    return names;
}
