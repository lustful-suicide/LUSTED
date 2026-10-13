#include "runtime/luau_manager.h"
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

// ---- Drawing library (transparent overlay) ----
static void VecFromLua(lua_State* L, int idx, float& x, float& y) {
    if (lua_istable(L, idx)) {
        lua_rawgeti(L, idx, 1); x = (float)lua_tonumber(L, -1); lua_pop(L, 1);
        lua_rawgeti(L, idx, 2); y = (float)lua_tonumber(L, -1); lua_pop(L, 1);
    } else {
        x = (float)lua_tonumber(L, idx); y = 0;
    }
}
static int l_dr_new(lua_State* L) {
    if (!g_dr) { lua_pushnumber(L, 0); return 1; }
    int id = g_dr->New(lua_tostring(L, 1) ? lua_tostring(L, 1) : "Line");
    lua_pushnumber(L, id);
    return 1;
}
static int l_dr_destroy(lua_State* L) {
    if (g_dr) g_dr->Destroy((int)lua_tonumber(L, 1));
    return 0;
}
static int l_dr_clear(lua_State* L) {
    (void)L;
    if (g_dr) g_dr->Clear();
    return 0;
}
static int l_dr_set(lua_State* L) {
    // drawing_set(id, prop, value) value = string/number/bool/{x,y}/{r,g,b}
    if (!g_dr) { lua_pushboolean(L, 0); return 1; }
    int id = (int)lua_tonumber(L, 1);
    const char* prop = lua_tostring(L, 2);
    if (!prop) { lua_pushboolean(L, 0); return 1; }
    std::string p = prop;
    bool ok = false;
    if (lua_istable(L, 3)) {
        size_t n = lua_objlen(L, 3);
        if ((p == "Position" || p == "From" || p == "To" || p == "Size") && n >= 2) {
            lua_rawgeti(L, 3, 1); float x = (float)lua_tonumber(L, -1); lua_pop(L, 1);
            lua_rawgeti(L, 3, 2); float y = (float)lua_tonumber(L, -1); lua_pop(L, 1);
            ok = g_dr->SetVec(id, p, x, y);
        } else if (p == "Color" && n >= 3) {
            lua_rawgeti(L, 3, 1); int r = (int)lua_tonumber(L, -1); lua_pop(L, 1);
            lua_rawgeti(L, 3, 2); int g = (int)lua_tonumber(L, -1); lua_pop(L, 1);
            lua_rawgeti(L, 3, 3); int b = (int)lua_tonumber(L, -1); lua_pop(L, 1);
            ok = g_dr->SetColor(id, p, r, g, b);
        } else if (p == "Colors" && n >= 2) {
            // gradient stops: {{r,g,b}, ...} (2-8). One color = use Color.
            std::vector<DrawingManager::Stop> stops;
            stops.reserve(n > 8 ? 8 : n);
            bool bad = false;
            for (size_t i = 1; i <= n && i <= 8; ++i) {
                lua_rawgeti(L, 3, (int)i);
                if (!lua_istable(L, -1)) { bad = true; lua_pop(L, 1); break; }
                lua_rawgeti(L, -1, 1); int r = (int)lua_tonumber(L, -1); lua_pop(L, 1);
                lua_rawgeti(L, -1, 2); int g = (int)lua_tonumber(L, -1); lua_pop(L, 1);
                lua_rawgeti(L, -1, 3); int b = (int)lua_tonumber(L, -1); lua_pop(L, 1);
                lua_pop(L, 1); // stop table
                DrawingManager::Stop s;
                s.r = r; s.g = g; s.b = b;
                stops.push_back(s);
            }
            ok = !bad && g_dr->SetColors(id, stops);
        } else ok = false;
    } else if (lua_isboolean(L, 3)) {
        ok = g_dr->Set(id, p, lua_toboolean(L, 3) ? "1" : "0");
    } else if (lua_isnumber(L, 3)) {
        char b[32]; snprintf(b, sizeof(b), "%f", lua_tonumber(L, 3));
        // numbers for Position{X} handled via Vec path; here scalar props
        if (p == "Position" || p == "From" || p == "To" || p == "Size") {
            ok = g_dr->SetVec(id, p, (float)lua_tonumber(L, 3), (float)lua_tonumber(L, 4));
        } else ok = g_dr->Set(id, p, b);
    } else if (lua_isstring(L, 3)) {
        ok = g_dr->Set(id, p, lua_tostring(L, 3));
    }
    lua_pushboolean(L, ok);
    return 1;
}
static int l_dr_get(lua_State* L) {
    if (!g_dr) { lua_pushnil(L); return 1; }
    const char* property = lua_tostring(L, 2);
    std::string p = property ? property : "";
    int id = (int)lua_tonumber(L, 1);
    std::string type = g_dr->Get(id, "Type");
    if (p == "Colors") {
        std::vector<DrawingManager::Stop> stops;
        if (!g_dr->GetColors(id, stops) || stops.empty()) { lua_pushnil(L); return 1; }
        lua_createtable(L, (int)stops.size(), 0);
        int k = 1;
        for (auto& s : stops) {
            lua_createtable(L, 3, 0);
            lua_pushnumber(L, s.r); lua_rawseti(L, -2, 1);
            lua_pushnumber(L, s.g); lua_rawseti(L, -2, 2);
            lua_pushnumber(L, s.b); lua_rawseti(L, -2, 3);
            lua_rawseti(L, -2, k++);
        }
        return 1;
    }
    std::string v = g_dr->Get(id, p);
    if (v.empty()) lua_pushnil(L);
    else if (p == "Visible" || p == "Filled" || p == "Center") lua_pushboolean(L, v == "1");
    else if (p == "Position" || p == "From" || p == "To" || p == "Size2D" ||
        (p == "Size" && (type == "Square" || type == "Box"))) {
        float x = 0, y = 0;
        if (sscanf(v.c_str(), "%f,%f", &x, &y) != 2) { lua_pushnil(L); return 1; }
        lua_createtable(L, 2, 0);
        lua_pushnumber(L, x); lua_rawseti(L, -2, 1);
        lua_pushnumber(L, y); lua_rawseti(L, -2, 2);
    } else if (p == "Color") {
        int r = 0, g = 0, b = 0;
        if (sscanf(v.c_str(), "%d,%d,%d", &r, &g, &b) != 3) { lua_pushnil(L); return 1; }
        lua_createtable(L, 3, 0);
        lua_pushnumber(L, r); lua_rawseti(L, -2, 1);
        lua_pushnumber(L, g); lua_rawseti(L, -2, 2);
        lua_pushnumber(L, b); lua_rawseti(L, -2, 3);
    } else if (p == "Radius" || p == "Thickness" || p == "Size" || p == "FontSize" ||
                 p == "Width" || p == "Height")
        lua_pushnumber(L, atof(v.c_str()));
    else lua_pushstring(L, v.c_str());
    return 1;
}

void LuauManager::RegisterDrawing() {
    lua_pushcfunction(L_, &l_dr_new, "drawing_new"); lua_setglobal(L_, "drawing_new");
    lua_pushcfunction(L_, &l_dr_destroy, "drawing_destroy"); lua_setglobal(L_, "drawing_destroy");
    lua_pushcfunction(L_, &l_dr_clear, "drawing_clear"); lua_setglobal(L_, "drawing_clear");
    lua_pushcfunction(L_, &l_dr_set, "drawing_set"); lua_setglobal(L_, "drawing_set");
    lua_pushcfunction(L_, &l_dr_get, "drawing_get"); lua_setglobal(L_, "drawing_get");
}
