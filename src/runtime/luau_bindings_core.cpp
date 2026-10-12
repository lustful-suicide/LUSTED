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

static uintptr_t ToAddr(lua_State* L, int idx) {
    if (lua_isstring(L, idx)) return (uintptr_t)strtoull(lua_tostring(L, idx), nullptr, 0);
    return (uintptr_t)(uintptr_t)lua_tonumber(L, idx);
}
static void PushAddr(lua_State* L, uintptr_t a) {
    if (!a) { lua_pushnil(L); return; }
    char b[32]; snprintf(b, sizeof(b), "0x%llX", (unsigned long long)a);
    lua_pushstring(L, b);
}

// ---- raw memory (CustomWrite path) ----
template <typename T> static int l_read(lua_State* L) {
    if (!g_mem || !g_mem->IsOpen()) { lua_pushnil(L); return 1; }
    T v{}; lua_pushnumber(L, g_mem->Read<T>(ToAddr(L, 1), v) ? (double)v : 0);
    return 1;
}
template <typename T> static int l_write(lua_State* L) {
    if (!g_mem || !g_mem->IsOpen()) { lua_pushboolean(L, 0); return 1; }
    T v = (T)lua_tonumber(L, 2);
    lua_pushboolean(L, g_mem->Write<T>(ToAddr(L, 1), v));
    return 1;
}
static int l_write_bytes(lua_State* L) {
    if (!g_mem || !g_mem->IsOpen()) { lua_pushboolean(L, 0); return 1; }
    uintptr_t a = ToAddr(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    size_t n = lua_objlen(L, 2);
    std::string b; b.reserve(n);
    for (size_t i = 1; i <= n; ++i) { lua_rawgeti(L, 2, (int)i); b.push_back((char)lua_tonumber(L, -1)); lua_pop(L, 1); }
    lua_pushboolean(L, g_mem->CustomWrite(a, b.data(), b.size()));
    return 1;
}
static int l_read_string(lua_State* L) {
    if (!g_mem || !g_mem->IsOpen()) { lua_pushstring(L, ""); return 1; }
    uintptr_t a = ToAddr(L, 1); size_t n = (size_t)lua_tonumber(L, 2);
    if (n == 0) n = 128; if (n > 4096) n = 4096;
    std::string s(n, '\0');
    if (!g_mem->CustomRead(a, s.data(), n)) { lua_pushstring(L, ""); return 1; }
    s = std::string(s.c_str());
    lua_pushstring(L, s.c_str());
    return 1;
}
static int l_read_float(lua_State* L) {
    if (!g_mem || !g_mem->IsOpen()) { lua_pushnil(L); return 1; }
    float v = 0; lua_pushnumber(L, g_mem->Read<float>(ToAddr(L, 1), v) ? v : 0);
    return 1;
}
static int l_write_float(lua_State* L) {
    if (!g_mem || !g_mem->IsOpen()) { lua_pushboolean(L, 0); return 1; }
    float v = (float)lua_tonumber(L, 2);
    lua_pushboolean(L, g_mem->Write<float>(ToAddr(L, 1), v));
    return 1;
}

// ---- offsets.json ----
static int l_offset(lua_State* L) {
    const char* cls = lua_tostring(L, 1); const char* m = lua_tostring(L, 2);
    if (!g_inst || !cls || !m) { lua_pushnil(L); return 1; }
    auto o = g_inst->OffsetOf(cls, m);
    if (!o) { lua_pushnil(L); return 1; }
    lua_pushnumber(L, (double)*o);
    return 1;
}

// ---- instance tree ----
static int l_get_parent(lua_State* L)      { PushAddr(L, g_inst ? g_inst->GetParent(ToAddr(L,1)) : 0); return 1; }
static int l_get_children(lua_State* L) {
    lua_newtable(L);
    if (!g_inst) return 1;
    auto v = g_inst->GetChildren(ToAddr(L, 1));
    int i = 1;
    for (auto a : v) { PushAddr(L, a); lua_rawseti(L, -2, i++); }
    return 1;
}
static int l_get_name(lua_State* L)       { lua_pushstring(L, g_inst ? g_inst->GetName(ToAddr(L,1)).c_str() : ""); return 1; }
static int l_get_class(lua_State* L)      { lua_pushstring(L, g_inst ? g_inst->GetClass(ToAddr(L,1)).c_str() : ""); return 1; }
static int l_direct_child(lua_State* L) {
    const char* name = lua_tostring(L, 2);
    PushAddr(L, g_inst ? g_inst->DirectChildByName(ToAddr(L, 1), name ? name : "") : 0);
    return 1;
}
static int l_find_first_child(lua_State* L) {
    PushAddr(L, g_inst ? g_inst->FindFirstChild(ToAddr(L,1), lua_tostring(L,2) ? lua_tostring(L,2) : "") : 0);
    return 1;
}
static int l_find_child_cached(lua_State* L) {
    PushAddr(L, g_inst ? g_inst->FindChildCached(ToAddr(L,1), lua_tostring(L,2) ? lua_tostring(L,2) : "") : 0);
    return 1;
}
static int l_find_first_child_of_class(lua_State* L) {
    PushAddr(L, g_inst ? g_inst->FindFirstChildOfClass(ToAddr(L,1), lua_tostring(L,2) ? lua_tostring(L,2) : "") : 0);
    return 1;
}
static int l_get_descendants(lua_State* L) {
    lua_newtable(L);
    if (!g_inst) return 1;
    std::vector<uintptr_t> out; g_inst->GetDescendants(ToAddr(L,1), out);
    int i = 1; for (auto a : out) { PushAddr(L, a); lua_rawseti(L, -2, i++); }
    return 1;
}
static int l_wait_for_child(lua_State* L) {
    const char* n = lua_tostring(L, 2); int t = (int)lua_tonumber(L, 3); if (!t) t = 5000;
    PushAddr(L, g_inst ? g_inst->WaitForChild(ToAddr(L,1), n ? n : "", t) : 0);
    return 1;
}
static int l_resolve_path(lua_State* L) {
    PushAddr(L, g_inst ? g_inst->ResolvePath(ToAddr(L,1), lua_tostring(L,2) ? lua_tostring(L,2) : "") : 0);
    return 1;
}
// ---- shortcuts ----
static DWORD CurPid() { return g_pid ? *g_pid : 0; }
static int l_get_datamodel(lua_State* L)  { PushAddr(L, g_inst ? g_inst->GetDataModel(CurPid()) : 0); return 1; }
static int l_set_datamodel(lua_State* L)  { if (g_inst) g_inst->SetDataModel(ToAddr(L,1)); return 0; }
static int l_is_datamodel(lua_State* L)   { lua_pushboolean(L, g_inst && g_inst->IsValidDataModel(ToAddr(L,1))); return 1; }
static int l_get_workspace(lua_State* L)  { PushAddr(L, g_inst ? g_inst->GetWorkspace(CurPid()) : 0); return 1; }
static int l_get_service(lua_State* L)    { PushAddr(L, g_inst ? g_inst->GetService(CurPid(), lua_tostring(L,1) ? lua_tostring(L,1) : "") : 0); return 1; }
static int l_get_localplayer(lua_State* L){ PushAddr(L, g_inst ? g_inst->GetLocalPlayer(CurPid()) : 0); return 1; }
static int l_get_character(lua_State* L)  { PushAddr(L, g_inst ? g_inst->GetCharacter(CurPid()) : 0); return 1; }
static int l_get_humanoid(lua_State* L)   { PushAddr(L, g_inst ? g_inst->GetHumanoid(CurPid()) : 0); return 1; }
static int l_get_rootpart(lua_State* L)   { PushAddr(L, g_inst ? g_inst->GetRootPart(CurPid()) : 0); return 1; }
// prop by class.member: get_prop(addr,"Humanoid","Health") / set_prop(addr,...)
static int l_get_prop_u32(lua_State* L) {
    if (!g_inst || !g_mem) { lua_pushnil(L); return 1; }
    auto o = g_inst->OffsetOf(lua_tostring(L,2) ? lua_tostring(L,2) : "", lua_tostring(L,3) ? lua_tostring(L,3) : "");
    if (!o) { lua_pushnil(L); return 1; }
    uint32_t v = 0; lua_pushnumber(L, g_mem->Read<uint32_t>(ToAddr(L,1)+(uintptr_t)*o, v) ? v : 0);
    return 1;
}
static int l_set_prop_u32(lua_State* L) {
    if (!g_inst || !g_mem) { lua_pushboolean(L,0); return 1; }
    auto o = g_inst->OffsetOf(lua_tostring(L,2) ? lua_tostring(L,2) : "", lua_tostring(L,3) ? lua_tostring(L,3) : "");
    if (!o) { lua_pushboolean(L,0); return 1; }
    uint32_t v = (uint32_t)lua_tonumber(L,4);
    lua_pushboolean(L, g_mem->Write<uint32_t>(ToAddr(L,1)+(uintptr_t)*o, v));
    return 1;
}
static int l_get_prop_float(lua_State* L) {
    if (!g_inst || !g_mem) { lua_pushnil(L); return 1; }
    auto o = g_inst->OffsetOf(lua_tostring(L,2) ? lua_tostring(L,2) : "", lua_tostring(L,3) ? lua_tostring(L,3) : "");
    if (!o) { lua_pushnil(L); return 1; }
    float v = 0; lua_pushnumber(L, g_mem->Read<float>(ToAddr(L,1)+(uintptr_t)*o, v) ? v : 0);
    return 1;
}
static int l_set_prop_float(lua_State* L) {
    if (!g_inst || !g_mem) { lua_pushboolean(L,0); return 1; }
    uintptr_t addr = ToAddr(L, 1);
    std::string cls = lua_tostring(L, 2) ? lua_tostring(L, 2) : "";
    std::string member = lua_tostring(L, 3) ? lua_tostring(L, 3) : "";
    float v = (float)lua_tonumber(L, 4);
    lua_pushboolean(L, g_inst->WritePropFloatMirrored(addr, cls, member, v));
    return 1;
}

// ---- typed property access (inheritance + sub-object chains) ----
static void PushVectorTable(lua_State* L, float x, float y, float z,
                            const char* const* names, int count) {
    lua_createtable(L, count, count);
    const int t = lua_gettop(L);
    const float values[3] = { x, y, z };
    for (int i = 0; i < count; ++i) {
        lua_pushnumber(L, values[i]);
        lua_rawseti(L, t, i + 1);
        lua_pushstring(L, names[i]);
        lua_pushnumber(L, values[i]);
        lua_setfield(L, t, names[i]); // pops the value; the key stays on top
        lua_pop(L, 1);
    }
}
static const char* const kXYZ[3] = { "X", "Y", "Z" };
static const char* const kRGB[3] = { "R", "G", "B" };
static void PushVec3(lua_State* L, float x, float y, float z) {
    PushVectorTable(L, x, y, z, kXYZ, 3);
}
static void PushVec2(lua_State* L, float x, float y) {
    lua_createtable(L, 2, 2);
    const int t = lua_gettop(L);
    lua_pushnumber(L, x); lua_rawseti(L, t, 1);
    lua_pushnumber(L, y); lua_rawseti(L, t, 2);
    lua_pushstring(L, "X"); lua_pushnumber(L, x); lua_setfield(L, t, "X"); lua_pop(L, 1);
    lua_pushstring(L, "Y"); lua_pushnumber(L, y); lua_setfield(L, t, "Y"); lua_pop(L, 1);
}
static void PushColor3(lua_State* L, float r, float g, float b) {
    PushVectorTable(L, r, g, b, kRGB, 3);
}
static int l_get_prop(lua_State* L) {
    // get_prop(addr, class, member) -> number | bool | address-string | {X,Y,Z}
    if (!g_inst || !g_mem) { lua_pushnil(L); return 1; }
    uintptr_t addr = ToAddr(L, 1);
    std::string cls = lua_tostring(L, 2) ? lua_tostring(L, 2) : "";
    std::string member = lua_tostring(L, 3) ? lua_tostring(L, 3) : "";
    PropInfo p = g_inst->ResolveProp(addr, cls, member);
    if (!p.ok()) { lua_pushnil(L); return 1; }
    uintptr_t fieldBase = g_inst->PropBase(p, addr);
    if (!fieldBase) { lua_pushnil(L); return 1; }
    uintptr_t field = fieldBase + (uintptr_t)p.offset;
    switch (p.type) {
        case PropType::Vec3: {
            float v[3] = {0, 0, 0};
            if (!g_inst->ReadPropVec3(p, addr, v)) { lua_pushnil(L); return 1; }
            PushVec3(L, v[0], v[1], v[2]);
            return 1;
        }
        case PropType::Vec2: {
            float v[2] = {0, 0};
            if (!g_mem->CustomRead(field, v, sizeof(float) * 2)) { lua_pushnil(L); return 1; }
            PushVec2(L, v[0], v[1]);
            return 1;
        }
        case PropType::C3: {
            float v[3] = {0, 0, 0};
            if (!g_mem->CustomRead(field, v, sizeof(float) * 3)) { lua_pushnil(L); return 1; }
            PushColor3(L, v[0], v[1], v[2]);
            return 1;
        }
        case PropType::Bool: {
            bool v = false;
            if (!g_inst->ReadPropBool(p, addr, v)) { lua_pushnil(L); return 1; }
            lua_pushboolean(L, v);
            return 1;
        }
        case PropType::I32:
        case PropType::U32: {
            uint32_t v = 0;
            if (!g_mem->CustomRead(field, &v, 4)) { lua_pushnil(L); return 1; }
            lua_pushnumber(L, (lua_Number)(int32_t)v);
            return 1;
        }
        case PropType::U64: {
            uint64_t v = 0;
            if (!g_mem->CustomRead(field, &v, 8)) { lua_pushnil(L); return 1; }
            lua_pushnumber(L, (lua_Number)v);
            return 1;
        }
        case PropType::Ptr: {
            uintptr_t v = 0;
            if (!g_mem->CustomRead(field, &v, 8)) { lua_pushnil(L); return 1; }
            PushAddr(L, v);
            return 1;
        }
        case PropType::RbxString:
        case PropType::String: {
            std::string v = g_inst->ReadPropString(p, addr);
            if (v.empty()) lua_pushnil(L); else lua_pushstring(L, v.c_str());
            return 1;
        }
        default: {
            float v = 0;
            if (!g_inst->ReadPropFloat(p, addr, v)) { lua_pushnil(L); return 1; }
            lua_pushnumber(L, v);
            return 1;
        }
    }
}
static int l_set_prop(lua_State* L) {
    if (!g_inst || !g_mem) { lua_pushboolean(L, 0); return 1; }
    uintptr_t addr = ToAddr(L, 1);
    std::string cls = lua_tostring(L, 2) ? lua_tostring(L, 2) : "";
    std::string member = lua_tostring(L, 3) ? lua_tostring(L, 3) : "";
    { // Replication-safe mirror: keep WalkSpeed/WalkSpeedCheck and JumpPower/JumpHeight in sync.
        std::string lc = cls, lm = member;
        for (auto& c : lc) c = (char)tolower((unsigned char)c);
        for (auto& c : lm) c = (char)tolower((unsigned char)c);
        if (lc == "humanoid" && (lm == "walkspeed" || lm == "walkspeedcheck" || lm == "jumppower" || lm == "jumpheight") && !lua_istable(L, 4)) {
            float fv = (float)lua_tonumber(L, 4);
            lua_pushboolean(L, g_inst->WritePropFloatMirrored(addr, cls, member, fv));
            return 1;
        }
    }
    PropInfo p = g_inst->ResolveProp(addr, cls, member);
    if (!p.ok()) { lua_pushboolean(L, 0); return 1; }
    uintptr_t fieldBase = g_inst->PropBase(p, addr);
    if (!fieldBase) { lua_pushboolean(L, 0); return 1; }
    uintptr_t field = fieldBase + (uintptr_t)p.offset;
    if ((p.type == PropType::Vec3 || p.type == PropType::C3) && lua_istable(L, 4)) {
        float v[3] = {0, 0, 0};
        const char* names[3] = { "X", "Y", "Z" };
        for (int i = 0; i < 3; ++i) {
            lua_rawgeti(L, 4, i + 1);
            if (lua_isnil(L, -1)) { lua_pop(L, 1); lua_getfield(L, 4, names[i]); }
            v[i] = (float)lua_tonumber(L, -1);
            lua_pop(L, 1);
        }
        lua_pushboolean(L, g_inst->WritePropVec3(p, addr, v));
        return 1;
    }
    if (p.type == PropType::Bool) {
        bool on = lua_isboolean(L, 4) ? lua_toboolean(L, 4) != 0 : lua_tonumber(L, 4) != 0;
        lua_pushboolean(L, g_inst->WritePropBool(p, addr, on));
        return 1;
    }
    float v = (float)lua_tonumber(L, 4);
    if (p.type == PropType::I32 || p.type == PropType::U32) {
        uint32_t iv = (uint32_t)(int32_t)v;
        lua_pushboolean(L, g_mem->CustomWrite(field, &iv, 4));
        return 1;
    }
    if (p.type == PropType::Ptr || p.type == PropType::U64) {
        uint64_t iv = (uint64_t)lua_tonumber(L, 4);
        lua_pushboolean(L, g_mem->CustomWrite(field, &iv, 8));
        return 1;
    }
    lua_pushboolean(L, g_inst->WritePropFloat(p, addr, v));
    return 1;
}
static int l_has_prop(lua_State* L) {
    std::string cls = lua_tostring(L, 1) ? lua_tostring(L, 1) : "";
    std::string member = lua_tostring(L, 2) ? lua_tostring(L, 2) : "";
    lua_pushboolean(L, g_inst && g_inst->HasProp(cls, member));
    return 1;
}
static int l_prop_offset(lua_State* L) {
    std::string cls = lua_tostring(L, 1) ? lua_tostring(L, 1) : "";
    std::string member = lua_tostring(L, 2) ? lua_tostring(L, 2) : "";
    if (!g_inst) { lua_pushnil(L); return 1; }
    PropInfo p = g_inst->ResolveProp(0, cls, member);
    if (!p.ok()) { lua_pushnil(L); return 1; }
    lua_pushnumber(L, (double)(p.hops.empty() ? p.offset : -p.offset));
    return 1;
}
static int l_get_prop_vec3(lua_State* L) {
    if (!g_inst || !g_mem) { lua_pushnil(L); return 1; }
    uintptr_t addr = ToAddr(L, 1);
    PropInfo p = g_inst->ResolveProp(addr, lua_tostring(L, 2) ? lua_tostring(L, 2) : "",
                                     lua_tostring(L, 3) ? lua_tostring(L, 3) : "");
    float v[3] = {0, 0, 0};
    if (!p.ok() || !g_inst->ReadPropVec3(p, addr, v)) { lua_pushnil(L); return 1; }
    PushVec3(L, v[0], v[1], v[2]);
    return 1;
}
// CFrame = 12 contiguous floats: rotation r00..r22 (9) then position px,py,pz (3).
static int l_get_cframe(lua_State* L) {
    if (!g_inst || !g_mem) { lua_pushnil(L); return 1; }
    uintptr_t addr = ToAddr(L, 1);
    PropInfo p = g_inst->ResolveProp(addr, lua_tostring(L, 2) ? lua_tostring(L, 2) : "",
                                     "CFrame");
    float v[12] = {0};
    if (!p.ok() || p.type != PropType::CF || !g_inst->ReadPropCFrame(p, addr, v)) {
        lua_pushnil(L); return 1;
    }
    lua_createtable(L, 12, 0);
    for (int i = 0; i < 12; ++i) { lua_pushnumber(L, v[i]); lua_rawseti(L, -2, i + 1); }
    return 1;
}
static int l_set_cframe(lua_State* L) {
    if (!g_inst || !g_mem) { lua_pushboolean(L, 0); return 1; }
    uintptr_t addr = ToAddr(L, 1);
    PropInfo p = g_inst->ResolveProp(addr, lua_tostring(L, 2) ? lua_tostring(L, 2) : "",
                                     "CFrame");
    if (!p.ok() || p.type != PropType::CF || !lua_istable(L, 3)) {
        lua_pushboolean(L, 0); return 1;
    }
    float v[12] = {0};
    for (int i = 0; i < 12; ++i) {
        lua_rawgeti(L, 3, i + 1);
        v[i] = (float)lua_tonumber(L, -1);
        lua_pop(L, 1);
    }
    lua_pushboolean(L, g_inst->WritePropCFrame(p, addr, v));
    return 1;
}
static int l_get_players(lua_State* L) {
    lua_newtable(L);
    if (!g_inst) return 1;
    auto v = g_inst->GetPlayers(CurPid());
    int i = 1;
    for (auto a : v) { PushAddr(L, a); lua_rawseti(L, -2, i++); }
    return 1;
}
static int l_get_player_count(lua_State* L) {
    lua_pushnumber(L, g_inst ? (double)g_inst->GetPlayers(CurPid()).size() : 0);
    return 1;
}
static int l_refresh_index(lua_State* L) {
    if (!g_inst) { lua_pushboolean(L, 0); return 1; }
    // refresh_index(force, deep): deep=true restores the vector traversal
    // pass for links the heap pattern can't see. Default is the fast sweep.
    lua_pushboolean(L, g_inst->BuildIndex(CurPid(), lua_toboolean(L, 1) != 0,
                                          lua_toboolean(L, 2) != 0));
    return 1;
}
static int l_index_stats(lua_State* L) {
    lua_pushstring(L, g_inst ? g_inst->IndexStats(CurPid()).c_str() : "no store");
    return 1;
}
static int l_invalidate_index(lua_State* L) {
    (void)L;
    if (g_inst) g_inst->InvalidateIndex();
    return 0;
}

void LuauManager::RegisterBindings() {
    lua_pushcfunction(L_, &l_read<uint8_t>,  "read_u8");  lua_setglobal(L_, "read_u8");
    lua_pushcfunction(L_, &l_read<uint16_t>, "read_u16"); lua_setglobal(L_, "read_u16");
    lua_pushcfunction(L_, &l_read<uint32_t>, "read_u32"); lua_setglobal(L_, "read_u32");
    lua_pushcfunction(L_, &l_read<uint64_t>, "read_u64"); lua_setglobal(L_, "read_u64");
    lua_pushcfunction(L_, &l_write<uint8_t>,  "write_u8");  lua_setglobal(L_, "write_u8");
    lua_pushcfunction(L_, &l_write<uint16_t>, "write_u16"); lua_setglobal(L_, "write_u16");
    lua_pushcfunction(L_, &l_write<uint32_t>, "write_u32"); lua_setglobal(L_, "write_u32");
    lua_pushcfunction(L_, &l_write<uint64_t>, "write_u64"); lua_setglobal(L_, "write_u64");
    lua_pushcfunction(L_, &l_write_bytes, "custom_write"); lua_setglobal(L_, "custom_write");
    lua_pushcfunction(L_, &l_read_string, "read_string"); lua_setglobal(L_, "read_string");
    lua_pushcfunction(L_, &l_read_float, "read_float"); lua_setglobal(L_, "read_float");
    lua_pushcfunction(L_, &l_write_float, "write_float"); lua_setglobal(L_, "write_float");

    lua_pushcfunction(L_, &l_offset, "offset"); lua_setglobal(L_, "offset");
    lua_pushcfunction(L_, &l_get_parent, "get_parent"); lua_setglobal(L_, "get_parent");
    lua_pushcfunction(L_, &l_get_children, "get_children"); lua_setglobal(L_, "get_children");
    lua_pushcfunction(L_, &l_get_name, "get_name"); lua_setglobal(L_, "get_name");
    lua_pushcfunction(L_, &l_get_class, "get_classname"); lua_setglobal(L_, "get_classname");
    lua_pushcfunction(L_, &l_find_first_child, "find_first_child"); lua_setglobal(L_, "find_first_child");
    lua_pushcfunction(L_, &l_find_child_cached, "find_child_cached"); lua_setglobal(L_, "find_child_cached");
    lua_pushcfunction(L_, &l_direct_child, "direct_child_by_name"); lua_setglobal(L_, "direct_child_by_name");
    lua_pushcfunction(L_, &l_find_first_child_of_class, "find_first_child_of_class"); lua_setglobal(L_, "find_first_child_of_class");
    lua_pushcfunction(L_, &l_get_descendants, "get_descendants"); lua_setglobal(L_, "get_descendants");
    lua_pushcfunction(L_, &l_wait_for_child, "wait_for_child"); lua_setglobal(L_, "wait_for_child");
    lua_pushcfunction(L_, &l_resolve_path, "resolve_path"); lua_setglobal(L_, "resolve_path");
    lua_pushcfunction(L_, &l_get_datamodel, "get_datamodel"); lua_setglobal(L_, "get_datamodel");
    lua_pushcfunction(L_, &l_set_datamodel, "set_datamodel"); lua_setglobal(L_, "set_datamodel");
    lua_pushcfunction(L_, &l_is_datamodel, "is_datamodel"); lua_setglobal(L_, "is_datamodel");
    lua_pushcfunction(L_, &l_get_workspace, "get_workspace"); lua_setglobal(L_, "get_workspace");
    lua_pushcfunction(L_, &l_get_service, "get_service"); lua_setglobal(L_, "get_service");
    lua_pushcfunction(L_, &l_get_localplayer, "get_localplayer"); lua_setglobal(L_, "get_localplayer");
    lua_pushcfunction(L_, &l_get_character, "get_character"); lua_setglobal(L_, "get_character");
    lua_pushcfunction(L_, &l_get_humanoid, "get_humanoid"); lua_setglobal(L_, "get_humanoid");
    lua_pushcfunction(L_, &l_get_rootpart, "raw_get_rootpart"); lua_setglobal(L_, "raw_get_rootpart");
    lua_pushcfunction(L_, &l_get_rootpart, "get_rootpart"); lua_setglobal(L_, "get_rootpart");
    lua_pushcfunction(L_, &l_get_prop_u32, "get_prop_u32"); lua_setglobal(L_, "get_prop_u32");
    lua_pushcfunction(L_, &l_set_prop_u32, "set_prop_u32"); lua_setglobal(L_, "set_prop_u32");
    lua_pushcfunction(L_, &l_get_prop_float, "get_prop_float"); lua_setglobal(L_, "get_prop_float");
    lua_pushcfunction(L_, &l_set_prop_float, "set_prop_float"); lua_setglobal(L_, "set_prop_float");
    lua_pushcfunction(L_, &l_get_prop, "get_prop"); lua_setglobal(L_, "get_prop");
    lua_pushcfunction(L_, &l_set_prop, "set_prop"); lua_setglobal(L_, "set_prop");
    lua_pushcfunction(L_, &l_has_prop, "has_prop"); lua_setglobal(L_, "has_prop");
    lua_pushcfunction(L_, &l_prop_offset, "prop_offset"); lua_setglobal(L_, "prop_offset");
    lua_pushcfunction(L_, &l_get_prop_vec3, "get_prop_vec3"); lua_setglobal(L_, "get_prop_vec3");
    lua_pushcfunction(L_, &l_get_cframe, "raw_get_cframe"); lua_setglobal(L_, "raw_get_cframe");
    lua_pushcfunction(L_, &l_set_cframe, "raw_set_cframe"); lua_setglobal(L_, "raw_set_cframe");
    lua_pushcfunction(L_, &l_get_players, "raw_get_players"); lua_setglobal(L_, "raw_get_players");
    lua_pushcfunction(L_, &l_get_player_count, "raw_get_player_count"); lua_setglobal(L_, "raw_get_player_count");
    lua_pushcfunction(L_, &l_refresh_index, "raw_refresh_index"); lua_setglobal(L_, "raw_refresh_index");
    lua_pushcfunction(L_, &l_index_stats, "raw_index_stats"); lua_setglobal(L_, "raw_index_stats");
    lua_pushcfunction(L_, &l_invalidate_index, "invalidate_index"); lua_setglobal(L_, "invalidate_index");
    // aliases matching Roblox naming
    lua_getglobal(L_, "find_first_child"); lua_setglobal(L_, "FindFirstChild");
    lua_getglobal(L_, "get_children"); lua_setglobal(L_, "GetChildren");
    lua_getglobal(L_, "get_descendants"); lua_setglobal(L_, "GetDescendants");
    lua_getglobal(L_, "wait_for_child"); lua_setglobal(L_, "WaitForChild");
    RegisterUi();
    RegisterDrawing();
}
