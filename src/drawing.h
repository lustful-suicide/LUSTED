#pragma once
#include <windows.h>
#include <atomic>
#include <string>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <cstdint>

// Exploit-style Drawing API over a transparent click-through GDI overlay
// attached to the Roblox window rect (colorkey transparency, no black screen).
class DrawingManager {
public:
    DrawingManager() = default;
    ~DrawingManager() { Shutdown(); }

    bool Init(DWORD targetPid = 0);
    void Shutdown();
    void Poll(); // follow Roblox rect + pump messages
    bool IsOpen() const { return hwnd_ != nullptr; }
    void SetTargetPid(DWORD pid) { targetPid_.store(pid); }

    int New(const std::string& type);
    void Destroy(int id);
    void Clear();
    bool Set(int id, const std::string& prop, const std::string& val);
    bool SetVec(int id, const std::string& prop, float x, float y);
    bool SetColor(int id, const std::string& prop, int r, int g, int b, int a = 255);
    // Gradient stops (2-8). Empty = solid single Color. Lines interpolate
    // along their length; boxes interpolate vertically (fill) / around the
    // perimeter (border).
    struct Stop { int r = 255, g = 255, b = 255; };
    bool SetColors(int id, const std::vector<Stop>& stops);
    bool GetColors(int id, std::vector<Stop>& out) const;
    std::string Get(int id, const std::string& prop);

private:
    struct Obj {
        std::string type = "Line";
        float x = 100, y = 100;
        float x2 = 200, y2 = 200;
        float w = 100, h = 100;
        float radius = 50;
        int r = 255, g = 255, b = 255, a = 255;
        std::vector<Stop> stops; // empty = solid Color above
        int thickness = 1;
        bool filled = false;
        bool visible = true;
        bool center = false;
        int size = 16;
        std::string text;
        // Cached GDI font: creating it per frame cost more than all shapes.
        HFONT font = nullptr;
        int fontSize = 0;
    };
    static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l);
    static VOID CALLBACK Tick(HWND h, UINT m, UINT_PTR id, DWORD t);
    static BOOL CALLBACK FindCb(HWND h, LPARAM l);
    void Render(HDC dc);
    // Dirty rectangles: repaint covers only changed areas, not the whole
    // screen, so steady 60fps doesn't cost a fullscreen fill per frame.
    static RECT BBox(const Obj& o);
    void TouchLocked(const Obj& o);
    void TouchIdLocked(int id);
    HWND FindRobloxWindow();
    void FollowRoblox();

    HWND hwnd_ = nullptr;
    std::atomic<DWORD> targetPid_{ 0 };
    DWORD cachedPid_ = 0;
    HWND roblox_ = nullptr;
    uint64_t lastFind_ = 0;
    std::unordered_map<int, Obj> objs_;
    mutable std::mutex mtx_;
    int next_ = 1;
    RECT dirty_ = {};
    bool dirtyFull_ = false;
};
