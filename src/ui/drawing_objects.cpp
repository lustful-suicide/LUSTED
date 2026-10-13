#include "ui/drawing.h"
#include "platform/windows/process.h"
#include <algorithm>
#include <cmath>

// Shared with drawing_window.cpp (same colorkey value; internal linkage per TU).
// Guarded so src/ui/drawing.cpp can include both TUs without a redefinition.
#ifndef LUSTED_DRAWING_KKEY_DEFINED
#define LUSTED_DRAWING_KKEY_DEFINED
static constexpr COLORREF kKey = RGB(255, 0, 255);
#endif

RECT DrawingManager::BBox(const Obj& o) {
    const int pad = o.thickness + 3;
    RECT r{};
    if (o.type == "Line") {
        r.left = (int)std::min(o.x, o.x2) - pad;
        r.top = (int)std::min(o.y, o.y2) - pad;
        r.right = (int)std::max(o.x, o.x2) + pad;
        r.bottom = (int)std::max(o.y, o.y2) + pad;
    } else if (o.type == "Circle") {
        r.left = (int)(o.x - o.radius) - pad;
        r.top = (int)(o.y - o.radius) - pad;
        r.right = (int)(o.x + o.radius) + pad;
        r.bottom = (int)(o.y + o.radius) + pad;
    } else if (o.type == "Text") {
        r.left = (int)o.x + (o.center ? -o.textWidth / 2 : 0) - pad;
        r.top = (int)o.y - pad;
        r.right = r.left + o.textWidth + pad * 2;
        r.bottom = (int)o.y + o.size + pad + 8;
    } else { // Square / Box
        r.left = (int)o.x - pad;
        r.top = (int)o.y - pad;
        r.right = (int)(o.x + o.w) + pad;
        r.bottom = (int)(o.y + o.h) + pad;
    }
    return r;
}

int DrawingManager::MeasureTextWidth(const Obj& o) {
    // Estimate instead of measuring with GDI every text update. GDI font
    // creation/measurement per call showed up as a hot spot in overlays with
    // many labels, and the estimate is close enough for box/title centering.
    if (o.text.empty()) return 0;
    return std::clamp((int)(o.text.size() * (size_t)o.size * 6 / 10) + 8, 0, 800);
}

void DrawingManager::TouchLocked(const Obj& o) {
    if (!o.visible) return;
    RECT b = BBox(o);
    if (dirty_.left >= dirty_.right || dirty_.top >= dirty_.bottom) dirty_ = b;
    else UnionRect(&dirty_, &dirty_, &b);
}

void DrawingManager::TouchIdLocked(int id) {
    auto it = objs_.find(id);
    if (it != objs_.end()) TouchLocked(it->second);
}

int DrawingManager::New(const std::string& t) {
    if (t != "Line" && t != "Square" && t != "Box" && t != "Circle" && t != "Text")
        return 0;
    std::lock_guard<std::mutex> lk(mtx_);
    int id = next_++;
    Obj o; o.type = t;
    objs_[id] = o;
    TouchIdLocked(id);
    return id;
}
void DrawingManager::Destroy(int id) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = objs_.find(id);
    if (it != objs_.end()) {
        TouchLocked(it->second);
        if (it->second.font) DeleteObject(it->second.font);
        objs_.erase(it);
    }
}
void DrawingManager::Clear() {
    std::lock_guard<std::mutex> lk(mtx_);
    for (auto& kv : objs_)
        if (kv.second.font) DeleteObject(kv.second.font);
    objs_.clear();
    dirtyFull_ = true; // everything must repaint away
}
bool DrawingManager::Set(int id, const std::string& p, const std::string& v) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = objs_.find(id);
    if (it == objs_.end()) return false;
    TouchLocked(it->second); // repaint the old spot too (moves/resizes)
    Obj& o = it->second;
    if (p == "Visible") o.visible = (v == "1" || v == "true");
    else if (p == "Filled") o.filled = (v == "1" || v == "true");
    else if (p == "Center") o.center = (v == "1" || v == "true");
    else if (p == "Thickness") o.thickness = (std::clamp)(atoi(v.c_str()), 1, 20);
    else if (p == "Width") o.w = (std::max)(0.0f, (float)atof(v.c_str()));
    else if (p == "Height") o.h = (std::max)(0.0f, (float)atof(v.c_str()));
    else if (p == "Size" || p == "FontSize") {
        o.size = (std::clamp)(atoi(v.c_str()), 1, 96);
        if (o.type == "Text") o.textWidth = MeasureTextWidth(o);
    }
    else if (p == "Radius") o.radius = (std::max)(0.0f, (float)atof(v.c_str()));
    else if (p == "Text") {
        if (o.text != v) {
            o.text = v;
            o.textWidth = MeasureTextWidth(o);
        }
    }
    else return false;
    TouchLocked(o);
    return true;
}
bool DrawingManager::SetVec(int id, const std::string& p, float x, float y) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = objs_.find(id);
    if (it == objs_.end()) return false;
    TouchLocked(it->second);
    Obj& o = it->second;
    if (p == "Position" || p == "From") { o.x = x; o.y = y; }
    else if (p == "To") { o.x2 = x; o.y2 = y; }
    else if (p == "Size") { o.w = x; o.h = y; }
    else return false;
    TouchLocked(o);
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
        o.stops.clear(); // back to solid: one color means no gradient
    }
    else return false;
    TouchLocked(o);
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
    if (p == "Width") { snprintf(b, sizeof(b), "%f", o.w); return b; }
    if (p == "Height") { snprintf(b, sizeof(b), "%f", o.h); return b; }
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

bool DrawingManager::SetColors(int id, const std::vector<Stop>& stops) {
    if (stops.size() < 2 || stops.size() > 8) return false;
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = objs_.find(id);
    if (it == objs_.end()) return false;
    Obj& o = it->second;
    if (o.type != "Line" && o.type != "Box" && o.type != "Square") return false;
    for (auto& s : stops)
        if (s.r < 0 || s.r > 255 || s.g < 0 || s.g > 255 || s.b < 0 || s.b > 255)
            return false;
    o.stops = stops;
    TouchLocked(o);
    return true;
}
bool DrawingManager::GetColors(int id, std::vector<Stop>& out) const {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = objs_.find(id);
    if (it == objs_.end()) return false;
    out = it->second.stops;
    return true;
}

namespace {

// Piecewise-linear stop interpolation at t in [0,1].
void StopColorAt(const std::vector<DrawingManager::Stop>& s, float t,
                 int& r, int& g, int& b) {
    if (s.empty()) { r = g = b = 255; return; }
    if (s.size() == 1 || t <= 0) { r = s[0].r; g = s[0].g; b = s[0].b; return; }
    if (t >= 1) { r = s.back().r; g = s.back().g; b = s.back().b; return; }
    const float seg = t * (float)(s.size() - 1);
    size_t i = (size_t)seg;
    if (i >= s.size() - 1) i = s.size() - 2;
    const float f = seg - (float)i;
    r = (int)(s[i].r + (s[i + 1].r - s[i].r) * f + 0.5f);
    g = (int)(s[i].g + (s[i + 1].g - s[i].g) * f + 0.5f);
    b = (int)(s[i].b + (s[i + 1].b - s[i].b) * f + 0.5f);
}

void GradientLine(HDC dc, float x1, float y1, float x2, float y2, int thick,
                  const std::vector<DrawingManager::Stop>& s) {
    const float dx = x2 - x1, dy = y2 - y1;
    const float len = sqrtf(dx * dx + dy * dy);
    const int segs = (std::max)(2, (std::min)(48, (int)(len / 6.0f) + 1));
    // Each segment overshoots into the next by half a step: integer rounding
    // used to leave 1px notches on diagonals (the "not a full line" look).
    for (int i = 0; i < segs; ++i) {
        const float t0 = (float)i / (float)segs;
        const float t1 = (std::min)(1.0f, ((float)i + 1.5f) / (float)segs);
        int r, g, b;
        StopColorAt(s, (t0 + t1) * 0.5f, r, g, b);
        HPEN pen = CreatePen(PS_SOLID, thick, RGB(r, g, b));
        HGDIOBJ old = SelectObject(dc, pen);
        MoveToEx(dc, (int)(x1 + dx * t0), (int)(y1 + dy * t0), nullptr);
        LineTo(dc, (int)(x1 + dx * t1), (int)(y1 + dy * t1));
        SelectObject(dc, old);
        DeleteObject(pen);
    }
}

// Vertical gradient fill + perimeter gradient border for boxes.
void GradientBox(HDC dc, float x, float y, float w, float h, int thick, bool filled,
                 const std::vector<DrawingManager::Stop>& s) {
    if (w <= 0 || h <= 0) return;
    if (filled) {
        const int rows = (std::max)(2, (std::min)(64, (int)h));
        for (int i = 0; i < rows; ++i) {
            int r, g, b;
            StopColorAt(s, (i + 0.5f) / (float)rows, r, g, b);
            HBRUSH br = CreateSolidBrush(RGB(r, g, b));
            RECT rc{ (int)x, (int)(y + h * i / rows),
                     (int)(x + w), (int)(y + h * (i + 1) / rows) + 1 };
            FillRect(dc, &rc, br);
            DeleteObject(br);
        }
    }
    // Border follows the stops around the perimeter.
    const float per = 2.0f * (w + h);
    const int segs = (std::max)(8, (std::min)(64, (int)(per / 8.0f) + 1));
    auto pointAt = [&](float d, float& ox, float& oy) {
        const float p0 = w, p1 = w + h, p2 = 2 * w + h;
        if (d < p0) { ox = x + d; oy = y; }
        else if (d < p1) { ox = x + w; oy = y + (d - p0); }
        else if (d < p2) { ox = x + w - (d - p1); oy = y + h; }
        else { ox = x; oy = y + h - (d - p2); }
    };
    for (int i = 0; i < segs; ++i) {
        int r, g, b;
        StopColorAt(s, (i + 0.5f) / (float)segs, r, g, b);
        HPEN pen = CreatePen(PS_SOLID, thick, RGB(r, g, b));
        HGDIOBJ old = SelectObject(dc, pen);
        float ax, ay, bx, by;
        pointAt(per * i / segs, ax, ay);
        // Overshoot like GradientLine so corners/joins have no notches.
        pointAt((std::min)(per, per * (i + 1.4f) / segs), bx, by);
        MoveToEx(dc, (int)ax, (int)ay, nullptr);
        LineTo(dc, (int)bx, (int)by);
        SelectObject(dc, old);
        DeleteObject(pen);
    }
}

} // namespace

void DrawingManager::Render(HDC dc) {
    // Snapshot under the lock, then paint without it. Holding mtx_ for the
    // whole GDI pass made every drawing_set on the worker block for the
    // full paint duration, so overlay latency grew with the object count.
    // Paint works on copies; fonts resolve per pass in a local cache so no
    // shared HFONT lifetime is touched off-lock.
    std::vector<Obj> snap;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        snap.reserve(objs_.size());
        for (auto& kv : objs_) snap.push_back(kv.second);
    }
    RECT rc{};
    if (GetClipBox(dc, &rc) <= 0) return;
    HBRUSH key = CreateSolidBrush(kKey);
    FillRect(dc, &rc, key);
    DeleteObject(key);
    std::unordered_map<int, HFONT> fonts;
    for (Obj& o : snap) {
        if (!o.visible) continue;
        // Skip objects fully outside the paint region.
        RECT bb = BBox(o), hit{};
        if (!IntersectRect(&hit, &bb, &rc)) continue;
        HPEN pen = CreatePen(PS_SOLID, o.thickness, RGB(o.r, o.g, o.b));
        HGDIOBJ oldPen = SelectObject(dc, pen);
        HBRUSH brush = o.filled ? CreateSolidBrush(RGB(o.r, o.g, o.b))
                                : (HBRUSH)GetStockObject(HOLLOW_BRUSH);
        HGDIOBJ oldBrush = SelectObject(dc, brush);
        SetTextColor(dc, RGB(o.r, o.g, o.b));
        SetBkMode(dc, TRANSPARENT);
        if (o.type == "Line") {
            if (o.stops.size() >= 2)
                GradientLine(dc, o.x, o.y, o.x2, o.y2, o.thickness, o.stops);
            else {
                MoveToEx(dc, (int)o.x, (int)o.y, nullptr);
                LineTo(dc, (int)o.x2, (int)o.y2);
            }
        } else if (o.type == "Square" || o.type == "Box") {
            if (o.stops.size() >= 2)
                GradientBox(dc, o.x, o.y, o.w, o.h, o.thickness, o.filled, o.stops);
            else
                Rectangle(dc, (int)o.x, (int)o.y, (int)(o.x + o.w), (int)(o.y + o.h));
        } else if (o.type == "Circle") {
            Ellipse(dc, (int)(o.x - o.radius), (int)(o.y - o.radius),
                        (int)(o.x + o.radius), (int)(o.y + o.radius));
        } else if (o.type == "Text") {
            HFONT f = nullptr;
            auto fi = fonts.find(o.size);
            if (fi != fonts.end()) f = fi->second;
            else {
                f = CreateFontA(o.size, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                    DEFAULT_QUALITY, DEFAULT_PITCH, "Arial");
                fonts[o.size] = f;
            }
            HGDIOBJ oldF = f ? SelectObject(dc, f) : nullptr;
            UINT fmt = DT_SINGLELINE | (o.center ? DT_CENTER : DT_LEFT);
            const int left = (int)o.x + (o.center ? -o.textWidth / 2 : 0);
            RECT tr{left, (int)o.y, left + o.textWidth, (int)o.y + 100};
            DrawTextA(dc, o.text.c_str(), -1, &tr, fmt);
            if (oldF) SelectObject(dc, oldF);
        }
        SelectObject(dc, oldPen);
        SelectObject(dc, oldBrush);
        DeleteObject(pen);
        if (o.filled) DeleteObject(brush);
    }
    for (auto& kf : fonts) if (kf.second) DeleteObject(kf.second);
}
