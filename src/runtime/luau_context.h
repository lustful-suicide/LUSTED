#pragma once
#include "runtime/luau_manager.h"

// Shared Lua-context globals, defined once in luau_manager.cpp.
// (Formerly file-static in luau_manager.cpp; extern here so the split
// binding translation units share a single instance.)
extern Memory* g_mem;
extern InstanceStore* g_inst;
extern DWORD* g_pid;
extern UiManager* g_ui;
extern DrawingManager* g_dr;
extern LuauManager* g_luau;

// Lua helpers owned by luau_manager.cpp but referenced by RegisterUi
// (needs external linkage to take their addresses cross-TU).
int l_luau_rescan(lua_State* L);
int l_luau_run_file(lua_State* L);
int l_request_unload(lua_State* L);
int l_on_tick(lua_State* L);
int l_remove_tick(lua_State* L);
