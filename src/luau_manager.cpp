#include "luau_manager.h"
#include "memory.h"
#include "instance.h"
#include "ui.h"
#include "drawing.h"
#include "roblox_finder.h"
#include "offsets_fetcher.h"
#include "luau_stdlib.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <cstdio>
#include <vector>

#include "lua.h"
#include "lualib.h"
#include "Luau/Compiler.h"

static Memory* g_mem = nullptr;
static InstanceStore* g_inst = nullptr;
static DWORD* g_pid = nullptr;
static UiManager* g_ui = nullptr;
static DrawingManager* g_dr = nullptr;
static LuauManager* g_luau = nullptr;

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
static int l_find_first_child(lua_State* L) {
    PushAddr(L, g_inst ? g_inst->FindFirstChild(ToAddr(L,1), lua_tostring(L,2) ? lua_tostring(L,2) : "") : 0);
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
    auto o = g_inst->OffsetOf(lua_tostring(L,2) ? lua_tostring(L,2) : "", lua_tostring(L,3) ? lua_tostring(L,3) : "");
    if (!o) { lua_pushboolean(L,0); return 1; }
    float v = (float)lua_tonumber(L,4);
    lua_pushboolean(L, g_mem->Write<float>(ToAddr(L,1)+(uintptr_t)*o, v));
    return 1;
}

LuauManager::LuauManager(Memory* m, InstanceStore* i, DWORD* p, UiManager* u, DrawingManager* d,
    std::atomic<bool>* stopRequest)
    : mem_(m), inst_(i), pidOut_(p), ui_(u), dr_(d), stopRequest_(stopRequest) {}

bool LuauManager::Init(const std::string& vh, const std::string& jp) {
    version_ = vh; offsetsPath_ = jp;
    g_mem = mem_; g_inst = inst_; g_pid = pidOut_; g_ui = ui_; g_dr = dr_;
    g_luau = this;
    if (g_inst) g_inst->Load(jp);
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
    lua_pushcfunction(L_, &l_get_prop_u32, "get_prop_u32"); lua_setglobal(L_, "get_prop_u32");
    lua_pushcfunction(L_, &l_set_prop_u32, "set_prop_u32"); lua_setglobal(L_, "set_prop_u32");
    lua_pushcfunction(L_, &l_get_prop_float, "get_prop_float"); lua_setglobal(L_, "get_prop_float");
    lua_pushcfunction(L_, &l_set_prop_float, "set_prop_float"); lua_setglobal(L_, "set_prop_float");
    // aliases matching Roblox naming
    lua_getglobal(L_, "find_first_child"); lua_setglobal(L_, "FindFirstChild");
    lua_getglobal(L_, "get_children"); lua_setglobal(L_, "GetChildren");
    lua_getglobal(L_, "get_descendants"); lua_setglobal(L_, "GetDescendants");
    lua_getglobal(L_, "wait_for_child"); lua_setglobal(L_, "WaitForChild");
    RegisterUi();
    RegisterDrawing();
}

// ---- UI library (polling Win32 window) ----
static int l_ui_label(lua_State* L) {
    if (!g_ui) { lua_pushnumber(L, 0); return 1; }
    lua_pushnumber(L, g_ui->AddLabel(lua_tostring(L, 1) ? lua_tostring(L, 1) : ""));
    return 1;
}
static int l_ui_set_label(lua_State* L) {
    if (g_ui) g_ui->SetLabel((int)lua_tonumber(L, 1), lua_tostring(L, 2) ? lua_tostring(L, 2) : "");
    return 0;
}
static int l_ui_tab(lua_State* L) {
    if (g_ui) g_ui->SetTab(lua_tostring(L, 1) ? lua_tostring(L, 1) : "Main");
    return 0;
}
static int l_ui_section(lua_State* L) {
    if (g_ui) g_ui->SetSection(lua_tostring(L, 1) ? lua_tostring(L, 1) : "General");
    return 0;
}
static int l_ui_button(lua_State* L) {
    if (!g_ui) { lua_pushnumber(L, 0); return 1; }
    lua_pushnumber(L, g_ui->AddButton(lua_tostring(L, 1) ? lua_tostring(L, 1) : "Button"));
    return 1;
}
static int l_ui_pressed(lua_State* L) {
    lua_pushboolean(L, g_ui && g_ui->ButtonPressed((int)lua_tonumber(L, 1)));
    return 1;
}
static int l_ui_on_pressed(lua_State* L) {
    bool registered = g_luau && lua_isfunction(L, 2) &&
        g_luau->RegisterButtonCallback((int)lua_tonumber(L, 1), 2);
    lua_pushboolean(L, registered);
    return 1;
}
static int l_ui_on_changed(lua_State* L) {
    bool registered = g_luau && lua_isfunction(L, 2) &&
        g_luau->RegisterControlCallback((int)lua_tonumber(L, 1), 2);
    lua_pushboolean(L, registered);
    return 1;
}
static int l_ui_toggle(lua_State* L) {
    if (!g_ui) { lua_pushnumber(L, 0); return 1; }
    lua_pushnumber(L, g_ui->AddToggle(lua_tostring(L, 1) ? lua_tostring(L, 1) : "Toggle",
        lua_toboolean(L, 2) != 0));
    return 1;
}
static int l_ui_toggle_state(lua_State* L) {
    lua_pushboolean(L, g_ui && g_ui->ToggleState((int)lua_tonumber(L, 1)));
    return 1;
}
static int l_ui_set_toggle(lua_State* L) {
    if (g_ui) g_ui->SetToggle((int)lua_tonumber(L, 1), lua_toboolean(L, 2) != 0);
    return 0;
}
static int l_ui_slider(lua_State* L) {
    if (!g_ui) { lua_pushnumber(L, 0); return 1; }
    lua_pushnumber(L, g_ui->AddSlider(lua_tostring(L, 1) ? lua_tostring(L, 1) : "Slider",
        (int)lua_tonumber(L, 2), (int)lua_tonumber(L, 3), (int)lua_tonumber(L, 4)));
    return 1;
}
static int l_ui_slider_value(lua_State* L) {
    lua_pushnumber(L, g_ui ? g_ui->SliderValue((int)lua_tonumber(L, 1)) : 0);
    return 1;
}
static int l_ui_set_slider(lua_State* L) {
    if (g_ui) g_ui->SetSlider((int)lua_tonumber(L, 1), (int)lua_tonumber(L, 2));
    return 0;
}
static int l_ui_dropdown(lua_State* L) {
    if (!g_ui) { lua_pushnumber(L, 0); return 1; }
    std::vector<std::string> options;
    if (lua_istable(L, 2)) {
        size_t count = lua_objlen(L, 2);
        options.reserve(count);
        for (size_t index = 1; index <= count; ++index) {
            lua_rawgeti(L, 2, (int)index);
            options.emplace_back(lua_tostring(L, -1) ? lua_tostring(L, -1) : "");
            lua_pop(L, 1);
        }
    }
    lua_pushnumber(L, g_ui->AddDropdown(lua_tostring(L, 1) ? lua_tostring(L, 1) : "Select",
        options, (int)lua_tonumber(L, 3) - 1));
    return 1;
}
static int l_ui_dropdown_value(lua_State* L) {
    lua_pushnumber(L, g_ui ? g_ui->DropdownValue((int)lua_tonumber(L, 1)) + 1 : 0);
    return 1;
}
static int l_ui_set_dropdown(lua_State* L) {
    if (g_ui) g_ui->SetDropdown((int)lua_tonumber(L, 1), (int)lua_tonumber(L, 2) - 1);
    return 0;
}
static int l_ui_input(lua_State* L) {
    if (!g_ui) { lua_pushnumber(L, 0); return 1; }
    lua_pushnumber(L, g_ui->AddInput(lua_tostring(L, 1) ? lua_tostring(L, 1) : "Input",
        lua_tostring(L, 2) ? lua_tostring(L, 2) : ""));
    return 1;
}
static int l_ui_input_value(lua_State* L) {
    if (!g_ui) { lua_pushstring(L, ""); return 1; }
    std::string value = g_ui->InputValue((int)lua_tonumber(L, 1));
    lua_pushstring(L, value.c_str());
    return 1;
}
static int l_ui_set_input(lua_State* L) {
    if (g_ui) g_ui->SetInput((int)lua_tonumber(L, 1), lua_tostring(L, 2) ? lua_tostring(L, 2) : "");
    return 0;
}
static int l_ui_keybind(lua_State* L) {
    if (!g_ui) { lua_pushnumber(L, 0); return 1; }
    lua_pushnumber(L, g_ui->AddKeybind(lua_tostring(L, 1) ? lua_tostring(L, 1) : "Keybind",
        (int)lua_tonumber(L, 2)));
    return 1;
}
static int l_ui_keybind_value(lua_State* L) {
    lua_pushnumber(L, g_ui ? g_ui->KeybindValue((int)lua_tonumber(L, 1)) : 0);
    return 1;
}
static int l_ui_set_keybind(lua_State* L) {
    if (g_ui) g_ui->SetKeybind((int)lua_tonumber(L, 1), (int)lua_tonumber(L, 2));
    return 0;
}
static int l_ui_depends(lua_State* L) {
    if (g_ui) g_ui->SetDependency((int)lua_tonumber(L, 1), (int)lua_tonumber(L, 2),
        lua_isnoneornil(L, 3) || lua_toboolean(L, 3));
    return 0;
}
static int l_ui_config_names(lua_State* L) {
    lua_newtable(L);
    if (!g_ui) return 1;
    int index = 1;
    for (const auto& name : g_ui->ConfigNames()) {
        lua_pushstring(L, name.c_str());
        lua_rawseti(L, -2, index++);
    }
    return 1;
}
static int l_ui_save_config(lua_State* L) {
    const char* name = lua_tostring(L, 1);
    lua_pushboolean(L, g_ui && name && g_ui->SaveConfig(name));
    return 1;
}
static int l_ui_load_config(lua_State* L) {
    const char* name = lua_tostring(L, 1);
    lua_pushboolean(L, g_ui && name && g_ui->LoadConfig(name));
    return 1;
}
static int l_ui_delete_config(lua_State* L) {
    const char* name = lua_tostring(L, 1);
    lua_pushboolean(L, g_ui && name && g_ui->DeleteConfig(name));
    return 1;
}
static int l_ui_toggle_visible(lua_State* L) {
    (void)L;
    if (g_ui) g_ui->ToggleVisible();
    return 0;
}
static int l_luau_rescan(lua_State* L) {
    lua_pushboolean(L, g_luau && g_luau->RescanRoblox());
    return 1;
}
static int l_luau_run_file(lua_State* L) {
    const char* filename = lua_tostring(L, 1);
    lua_pushboolean(L, g_luau && filename && g_luau->RunAutoFile(filename));
    return 1;
}
static int l_request_unload(lua_State* L) {
    (void)L;
    if (g_luau) g_luau->RequestUnload();
    return 0;
}

void LuauManager::RegisterUi() {
    lua_pushcfunction(L_, &l_ui_label, "ui_label"); lua_setglobal(L_, "ui_label");
    lua_pushcfunction(L_, &l_ui_set_label, "ui_set_label"); lua_setglobal(L_, "ui_set_label");
    lua_pushcfunction(L_, &l_ui_tab, "ui_tab"); lua_setglobal(L_, "ui_tab");
    lua_pushcfunction(L_, &l_ui_section, "ui_section"); lua_setglobal(L_, "ui_section");
    lua_pushcfunction(L_, &l_ui_button, "ui_button"); lua_setglobal(L_, "ui_button");
    lua_pushcfunction(L_, &l_ui_pressed, "ui_pressed"); lua_setglobal(L_, "ui_pressed");
    lua_pushcfunction(L_, &l_ui_on_pressed, "ui_on_pressed"); lua_setglobal(L_, "ui_on_pressed");
    lua_pushcfunction(L_, &l_ui_on_changed, "ui_on_changed"); lua_setglobal(L_, "ui_on_changed");
    lua_pushcfunction(L_, &l_ui_toggle, "ui_toggle"); lua_setglobal(L_, "ui_toggle");
    lua_pushcfunction(L_, &l_ui_toggle_state, "ui_toggle_state"); lua_setglobal(L_, "ui_toggle_state");
    lua_pushcfunction(L_, &l_ui_set_toggle, "ui_set_toggle"); lua_setglobal(L_, "ui_set_toggle");
    lua_pushcfunction(L_, &l_ui_slider, "ui_slider"); lua_setglobal(L_, "ui_slider");
    lua_pushcfunction(L_, &l_ui_slider_value, "ui_slider_value"); lua_setglobal(L_, "ui_slider_value");
    lua_pushcfunction(L_, &l_ui_set_slider, "ui_set_slider"); lua_setglobal(L_, "ui_set_slider");
    lua_pushcfunction(L_, &l_ui_dropdown, "ui_dropdown"); lua_setglobal(L_, "ui_dropdown");
    lua_pushcfunction(L_, &l_ui_dropdown_value, "ui_dropdown_value"); lua_setglobal(L_, "ui_dropdown_value");
    lua_pushcfunction(L_, &l_ui_set_dropdown, "ui_set_dropdown"); lua_setglobal(L_, "ui_set_dropdown");
    lua_pushcfunction(L_, &l_ui_input, "ui_input"); lua_setglobal(L_, "ui_input");
    lua_pushcfunction(L_, &l_ui_input_value, "ui_input_value"); lua_setglobal(L_, "ui_input_value");
    lua_pushcfunction(L_, &l_ui_set_input, "ui_set_input"); lua_setglobal(L_, "ui_set_input");
    lua_pushcfunction(L_, &l_ui_keybind, "ui_keybind"); lua_setglobal(L_, "ui_keybind");
    lua_pushcfunction(L_, &l_ui_keybind_value, "ui_keybind_value"); lua_setglobal(L_, "ui_keybind_value");
    lua_pushcfunction(L_, &l_ui_set_keybind, "ui_set_keybind"); lua_setglobal(L_, "ui_set_keybind");
    lua_pushcfunction(L_, &l_ui_depends, "ui_depends"); lua_setglobal(L_, "ui_depends");
    lua_pushcfunction(L_, &l_ui_config_names, "ui_config_names"); lua_setglobal(L_, "ui_config_names");
    lua_pushcfunction(L_, &l_ui_save_config, "ui_save_config"); lua_setglobal(L_, "ui_save_config");
    lua_pushcfunction(L_, &l_ui_load_config, "ui_load_config"); lua_setglobal(L_, "ui_load_config");
    lua_pushcfunction(L_, &l_ui_delete_config, "ui_delete_config"); lua_setglobal(L_, "ui_delete_config");
    lua_pushcfunction(L_, &l_ui_toggle_visible, "ui_toggle_visible"); lua_setglobal(L_, "ui_toggle_visible");
    lua_pushcfunction(L_, &l_luau_rescan, "luau_rescan"); lua_setglobal(L_, "luau_rescan");
    lua_pushcfunction(L_, &l_luau_run_file, "luau_run_file"); lua_setglobal(L_, "luau_run_file");
    lua_pushcfunction(L_, &l_request_unload, "request_unload"); lua_setglobal(L_, "request_unload");
}

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
        } else ok = false;
    } else if (lua_isboolean(L, 3)) {
        ok = g_dr->Set(id, p, lua_toboolean(L, 3) ? "1" : "0");
    } else if (lua_isnumber(L, 3)) {
        char b[32]; snprintf(b, sizeof(b), "%f", lua_tonumber(L, 3));
        // numbers for Position{X} handled via Vec path; here scalar props
        if (p == "Position" || p == "From" || p == "To") {
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
    } else if (p == "Radius" || p == "Thickness" || p == "Size" || p == "FontSize")
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
bool LuauManager::RescanRoblox() {
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
    if (mem_ && (!mem_->IsOpen() || oldPid != newPid) && !mem_->Attach(newPid)) {
        if (oldPid) mem_->Attach(oldPid);
        return false;
    }
    *inst_ = std::move(refreshed);
    if (pidOut_) *pidOut_ = newPid;
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
void LuauManager::Poll() {
    if (!L_ || !ui_ || buttonCallbacks_.empty()) return;
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
void LuauManager::Close() {
    if (L_) {
        for (const auto& callback : buttonCallbacks_) lua_unref(L_, callback.second);
        buttonCallbacks_.clear();
    }
    if (L_) { lua_close(L_); L_ = nullptr; }
    if (g_mem == mem_) g_mem = nullptr;
    if (g_inst == inst_) g_inst = nullptr;
    if (g_ui == ui_) g_ui = nullptr;
    if (g_dr == dr_) g_dr = nullptr;
    if (g_luau == this) g_luau = nullptr;
}
