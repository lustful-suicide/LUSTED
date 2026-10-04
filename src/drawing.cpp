#include "drawing.h"
#include <algorithm>

// Magenta colorkey: background disappears, drawings stay (avoids black screen;
// pure black drawings would vanish with a black key).
static constexpr COLORREF kKey = RGB(255, 0, 255);
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
    InvalidateRect(h, nullptr, FALSE);
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
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "LustedDrawClass";
    RegisterClassA(&wc);
    HWND r = FindRobloxWindow();
    int x = 0, y = 0, sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    if (r) { RECT rc{}; GetWindowRect(r, &rc); x = rc.left; y = rc.top; sw = rc.right - rc.left; sh = rc.bottom - rc.top; }
    hwnd_ = CreateWindowExA(WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE,
        "LustedDrawClass", "LustedDraw", WS_POPUP,
        x, y, sw, sh, nullptr, nullptr, GetModuleHandleA(nullptr), nullptr);
    if (!hwnd_) return false;
    SetLayeredWindowAttributes(hwnd_, kKey, 0, LWA_COLORKEY);
    ShowWindow(hwnd_, SW_SHOW);
    SetTimer(hwnd_, 1, 33, Tick);
    return true;
}

void DrawingManager::Shutdown() {
    if (hwnd_) { KillTimer(hwnd_, 1); DestroyWindow(hwnd_); hwnd_ = nullptr; }
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

int DrawingManager::New(const std::string& t) {
    if (t != "Line" && t != "Square" && t != "Box" && t != "Circle" && t != "Text")
        return 0;
    std::lock_guard<std::mutex> lk(mtx_);
    int id = next_++;
    Obj o; o.type = t;
    objs_[id] = o;
    return id;
}
void DrawingManager::Destroy(int id) {
    std::lock_guard<std::mutex> lk(mtx_);
    objs_.erase(id);
}
void DrawingManager::Clear() {
    std::lock_guard<std::mutex> lk(mtx_);
    objs_.clear();
}
bool DrawingManager::Set(int id, const std::string& p, const std::string& v) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = objs_.find(id);
    if (it == objs_.end()) return false;
    Obj& o = it->second;
    if (p == "Visible") o.visible = (v == "1" || v == "true");
    else if (p == "Filled") o.filled = (v == "1" || v == "true");
    else if (p == "Center") o.center = (v == "1" || v == "true");
    else if (p == "Thickness") o.thickness = (std::clamp)(atoi(v.c_str()), 1, 20);
    else if (p == "Size" || p == "FontSize") o.size = (std::clamp)(atoi(v.c_str()), 1, 96);
    else if (p == "Radius") o.radius = (std::max)(0.0f, (float)atof(v.c_str()));
    else if (p == "Text") o.text = v;
    else return false;
    return true;
}
bool DrawingManager::SetVec(int id, const std::string& p, float x, float y) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = objs_.find(id);
    if (it == objs_.end()) return false;
    Obj& o = it->second;
    if (p == "Position" || p == "From") { o.x = x; o.y = y; }
    else if (p == "To") { o.x2 = x; o.y2 = y; }
    else if (p == "Size") { o.w = x; o.h = y; }
    else return false;
    return true;
}
bool DrawingManager::SetColor(int id, const std::string& p, int r, int g, int b, int a) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = objs_.find(id);
    if (it == objs_.end()) return false;
    Obj& o = it->second;
    if (p == "Color") {
        r = (std::clamp)(r, 0, 255);
        g = (std::clamp)(g, 0, 255);
        b = (std::clamp)(b, 0, 255);
        a = (std::clamp)(a, 0, 255);
        // magenta is the transparent key: nudge it so it stays visible
        if (r == 255 && g == 0 && b == 255) b = 254;
        o.r = r; o.g = g; o.b = b; o.a = a;
    }
    else return false;
    return true;
}
std::string DrawingManager::Get(int id, const std::string& p) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = objs_.find(id);
    if (it == objs_.end()) return "";
    Obj& o = it->second;
    char b[64];
    if (p == "Visible") return o.visible ? "1" : "0";
    if (p == "Filled") return o.filled ? "1" : "0";
    if (p == "Center") return o.center ? "1" : "0";
    if (p == "Type") return o.type;
    if (p == "Text") return o.text;
    if (p == "Radius") { snprintf(b, sizeof(b), "%f", o.radius); return b; }
    if (p == "Thickness") { snprintf(b, sizeof(b), "%d", o.thickness); return b; }
    if (p == "Size" && (o.type == "Square" || o.type == "Box")) {
        snprintf(b, sizeof(b), "%f,%f", o.w, o.h);
        return b;
    }
    if (p == "Size" || p == "FontSize") { snprintf(b, sizeof(b), "%d", o.size); return b; }
    if (p == "Position" || p == "From") {
        snprintf(b, sizeof(b), "%f,%f", o.x, o.y);
        return b;
    }
    if (p == "To") {
        snprintf(b, sizeof(b), "%f,%f", o.x2, o.y2);
        return b;
    }
    if (p == "Size2D") {
        snprintf(b, sizeof(b), "%f,%f", o.w, o.h);
        return b;
    }
    if (p == "Color") {
        snprintf(b, sizeof(b), "%d,%d,%d", o.r, o.g, o.b);
        return b;
    }
    return "";
}

void DrawingManager::Render(HDC dc) {
    std::lock_guard<std::mutex> lk(mtx_);
    RECT rc; GetClientRect(hwnd_, &rc);
    HBRUSH key = CreateSolidBrush(kKey);
    FillRect(dc, &rc, key);
    DeleteObject(key);
    for (auto& kv : objs_) {
        Obj& o = kv.second;
        if (!o.visible) continue;
        HPEN pen = CreatePen(PS_SOLID, o.thickness, RGB(o.r, o.g, o.b));
        HGDIOBJ oldPen = SelectObject(dc, pen);
        HBRUSH brush = o.filled ? CreateSolidBrush(RGB(o.r, o.g, o.b))
                                : (HBRUSH)GetStockObject(HOLLOW_BRUSH);
        HGDIOBJ oldBrush = SelectObject(dc, brush);
        SetTextColor(dc, RGB(o.r, o.g, o.b));
        SetBkMode(dc, TRANSPARENT);
        if (o.type == "Line") {
            MoveToEx(dc, (int)o.x, (int)o.y, nullptr);
            LineTo(dc, (int)o.x2, (int)o.y2);
        } else if (o.type == "Square" || o.type == "Box") {
            Rectangle(dc, (int)o.x, (int)o.y, (int)(o.x + o.w), (int)(o.y + o.h));
        } else if (o.type == "Circle") {
            Ellipse(dc, (int)(o.x - o.radius), (int)(o.y - o.radius),
                        (int)(o.x + o.radius), (int)(o.y + o.radius));
        } else if (o.type == "Text") {
            HFONT f = CreateFontA(o.size, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                DEFAULT_QUALITY, DEFAULT_PITCH, "Arial");
            HGDIOBJ oldF = SelectObject(dc, f);
            UINT fmt = DT_SINGLELINE | (o.center ? DT_CENTER : DT_LEFT);
            RECT tr{(int)o.x, (int)o.y, (int)o.x + 800, (int)o.y + 100};
            DrawTextA(dc, o.text.c_str(), -1, &tr, fmt | DT_NOCLIP);
            SelectObject(dc, oldF);
            DeleteObject(f);
        }
        SelectObject(dc, oldPen);
        SelectObject(dc, oldBrush);
        DeleteObject(pen);
        if (o.filled) DeleteObject(brush);
    }
}
