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
static int l_ui_explorer(lua_State* L) {
    // ui_explorer(show?) -> visible. No arg toggles.
    if (!g_ui) { lua_pushboolean(L, 0); return 1; }
    if (lua_isnoneornil(L, 1)) g_ui->SetExplorerOpen(!g_ui->IsExplorerOpen());
    else g_ui->SetExplorerOpen(lua_toboolean(L, 1) != 0);
    lua_pushboolean(L, g_ui->IsExplorerOpen());
    return 1;
}
static int l_is_key_down(lua_State* L) {
    // is_key_down(virtualKey) -> held right now. Edge-triggered keybind
    // presses (ui_pressed) can't express hold-to-aim, so poll this per tick.
    int vk = (int)lua_tonumber(L, 1);
    if (vk <= 0 || vk >= 256) { lua_pushboolean(L, 0); return 1; }
    lua_pushboolean(L, (GetAsyncKeyState(vk) & 0x8000) != 0);
    return 1;
}
static int l_mouse_move(lua_State* L) {
    // mouse_move(dx, dy) -> moved. Relative SendInput move in pixels; the
    // aimbot loop recomputes the residual each tick, so DPI/scale mismatch
    // converges instead of accumulating.
    long dx = (long)lua_tonumber(L, 1);
    long dy = (long)lua_tonumber(L, 2);
    if (dx == 0 && dy == 0) { lua_pushboolean(L, 1); return 1; }
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_MOVE;
    input.mi.dx = dx;
    input.mi.dy = dy;
    lua_pushboolean(L, SendInput(1, &input, sizeof(input)) == 1);
    return 1;
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
    lua_pushcfunction(L_, &l_ui_explorer, "ui_explorer"); lua_setglobal(L_, "ui_explorer");
    lua_pushcfunction(L_, &l_is_key_down, "is_key_down"); lua_setglobal(L_, "is_key_down");
    lua_pushcfunction(L_, &l_mouse_move, "mouse_move"); lua_setglobal(L_, "mouse_move");
    lua_pushcfunction(L_, &l_luau_rescan, "luau_rescan"); lua_setglobal(L_, "luau_rescan");
    lua_pushcfunction(L_, &l_luau_run_file, "luau_run_file"); lua_setglobal(L_, "luau_run_file");
    lua_pushcfunction(L_, &l_request_unload, "request_unload"); lua_setglobal(L_, "request_unload");
    lua_pushcfunction(L_, &l_on_tick, "on_tick"); lua_setglobal(L_, "on_tick");
    lua_pushcfunction(L_, &l_remove_tick, "remove_tick"); lua_setglobal(L_, "remove_tick");
}
