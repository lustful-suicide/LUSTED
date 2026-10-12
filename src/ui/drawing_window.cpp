#include "ui/drawing.h"
#include "platform/windows/process.h"
#include <algorithm>
#include <cmath>

// Magenta colorkey: background disappears, drawings stay (avoids black screen;
// pure black drawings would vanish with a black key).
#ifndef LUSTED_DRAWING_KKEY_DEFINED
#define LUSTED_DRAWING_KKEY_DEFINED
static constexpr COLORREF kKey = RGB(255, 0, 255);
#endif
static DrawingManager* g_dr = nullptr;

struct FindCtx { DWORD pid; HWND hwnd; };

LRESULT CALLBACK DrawingManager::WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_PAINT && g_dr) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        g_dr->Render(dc);
        EndPaint(h, &ps);
        return 0;
    }
    if (m == WM_ERASEBKGND) return 1; // no flicker fill
    if (m == WM_TIMER && g_dr) {
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

VOID CALLBACK DrawingManager::Tick(HWND h, UINT m, UINT_PTR id, DWORD t) {
    (void)m; (void)id; (void)t;
    if (!g_dr) return;
    // Repaint only the union of changed areas. Empty dirty + no visible
    // objects means nothing to do (no fullscreen key-fill for nothing).
    std::lock_guard<std::mutex> lk(g_dr->mtx_);
    if (g_dr->dirtyFull_) {
        g_dr->dirtyFull_ = false;
        SetRectEmpty(&g_dr->dirty_);
        InvalidateRect(h, nullptr, FALSE);
        return;
    }
    if (g_dr->dirty_.left < g_dr->dirty_.right &&
        g_dr->dirty_.top < g_dr->dirty_.bottom) {
        RECT r = g_dr->dirty_;
        SetRectEmpty(&g_dr->dirty_);
        InvalidateRect(h, &r, FALSE);
    }
}

BOOL CALLBACK DrawingManager::FindCb(HWND h, LPARAM l) {
    FindCtx* c = (FindCtx*)l;
    DWORD wpid = 0;
    GetWindowThreadProcessId(h, &wpid);
    if (wpid != c->pid) return TRUE;
    if (!IsWindowVisible(h)) return TRUE;
    char cls[64] = {};
    GetClassNameA(h, cls, sizeof(cls));
    // Roblox main window; skip tiny/tool windows
    RECT rc{};
    GetWindowRect(h, &rc);
    if ((rc.right - rc.left) < 400 || (rc.bottom - rc.top) < 300) return TRUE;
    c->hwnd = h;
    return FALSE;
}

HWND DrawingManager::FindRobloxWindow() {
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

void DrawingManager::FollowRoblox() {
    if (!hwnd_) return;
    HWND r = FindRobloxWindow();
    if (!r) return;
    if (IsIconic(r)) { ShowWindow(hwnd_, SW_HIDE); return; }
    ShowWindow(hwnd_, SW_SHOW);
    RECT rc{};
    GetWindowRect(r, &rc);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w < 10 || h < 10) return;
    RECT cur{};
    GetWindowRect(hwnd_, &cur);
    if (cur.left != rc.left || cur.top != rc.top ||
        (cur.right - cur.left) != w || (cur.bottom - cur.top) != h) {
        SetWindowPos(hwnd_, HWND_TOPMOST, rc.left, rc.top, w, h,
            SWP_NOACTIVATE | SWP_SHOWWINDOW);
        // Moved/resized: object pixels shifted, repaint everything once.
        std::lock_guard<std::mutex> lk(mtx_);
        dirtyFull_ = true;
    } else {
        SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
}

bool DrawingManager::Init(DWORD pid) {
    if (hwnd_) { targetPid_.store(pid); return true; }
    targetPid_.store(pid);
    g_dr = this;
    WNDCLASSA wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = CurrentModule();
    wc.lpszClassName = "LustedDrawClass";
    RegisterClassA(&wc);
    HWND r = FindRobloxWindow();
    int x = 0, y = 0, sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    if (r) { RECT rc{}; GetWindowRect(r, &rc); x = rc.left; y = rc.top; sw = rc.right - rc.left; sh = rc.bottom - rc.top; }
    hwnd_ = CreateWindowExA(WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE,
        "LustedDrawClass", "LustedDraw", WS_POPUP,
        x, y, sw, sh, nullptr, nullptr, CurrentModule(), nullptr);
    if (!hwnd_) return false;
    SetLayeredWindowAttributes(hwnd_, kKey, 0, LWA_COLORKEY);
    ShowWindow(hwnd_, SW_SHOW);
    SetTimer(hwnd_, 1, 16, Tick); // ~60fps cadence; Tick skips empty frames
    return true;
}

void DrawingManager::Shutdown() {
    if (hwnd_) { KillTimer(hwnd_, 1); DestroyWindow(hwnd_); hwnd_ = nullptr; }
    Clear(); // frees cached fonts
    g_dr = nullptr;
}

void DrawingManager::Poll() {
    FollowRoblox();
    MSG msg;
    while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}
