#pragma once

inline constexpr char kUiLibrary[] = R"LUA(
UI = {}

function UI.Tab(name) return ui_tab(name) end
function UI.Section(name) return ui_section(name) end
function UI.Label(text) return ui_label(text) end
function UI.SetLabel(id, text) return ui_set_label(id, text) end
function UI.Button(text, callback)
    local id = ui_button(text)
    if callback then UI.OnPressed(id, callback) end
    return id
end
function UI.Pressed(id) return ui_pressed(id) end
function UI.OnPressed(id, callback) return ui_on_pressed(id, callback) end
function UI.OnChanged(id, callback) return ui_on_changed(id, callback) end
function UI.Toggle(text, default, callback)
    local id = ui_toggle(text, default or false)
    if callback then UI.OnChanged(id, callback) end
    return id
end
function UI.ToggleState(id) return ui_toggle_state(id) end
function UI.SetToggle(id, value) return ui_set_toggle(id, value) end
function UI.Slider(text, minimum, maximum, default, callback)
    local id = ui_slider(text, minimum or 0, maximum or 100, default or 50)
    if callback then UI.OnChanged(id, callback) end
    return id
end
function UI.SliderValue(id) return ui_slider_value(id) end
function UI.SetSlider(id, value) return ui_set_slider(id, value) end
function UI.Dropdown(text, options, selected, callback)
    local id = ui_dropdown(text, options, selected or 1)
    if callback then UI.OnChanged(id, callback) end
    return id
end
function UI.DropdownValue(id) return ui_dropdown_value(id) end
function UI.SetDropdown(id, selected) return ui_set_dropdown(id, selected) end
function UI.Input(text, placeholder, callback)
    local id = ui_input(text, placeholder or "")
    if callback then UI.OnChanged(id, callback) end
    return id
end
function UI.InputValue(id) return ui_input_value(id) end
function UI.SetInput(id, value) return ui_set_input(id, value) end
function UI.Keybind(text, defaultKey, callback)
    local key = defaultKey or 0
    if type(key) == "string" and #key == 1 then key = string.byte(string.upper(key)) end
    local id = ui_keybind(text, key)
    if callback then UI.OnPressed(id, callback) end
    return id
end
function UI.KeybindValue(id) return ui_keybind_value(id) end
function UI.SetKeybind(id, key)
    if type(key) == "string" and #key == 1 then key = string.byte(string.upper(key)) end
    return ui_set_keybind(id, key)
end
function UI.Depends(controlId, toggleId, expected)
    return ui_depends(controlId, toggleId, expected ~= false)
end
function UI.ConfigNames() return ui_config_names() end
function UI.SaveConfig(name) return ui_save_config(name) end
function UI.LoadConfig(name) return ui_load_config(name) end
function UI.DeleteConfig(name) return ui_delete_config(name) end
function UI.ToggleVisible() return ui_toggle_visible() end
function UI.Rescan() return luau_rescan() end
function UI.RunFile(filename) return luau_run_file(filename) end
function UI.RequestUnload() return request_unload() end

UI.Tab("Settings")
UI.Section("Status")
local settingsStatus = UI.Label("Ready")
UI.Button("Toggle Explorer", function() ui_explorer() end)
UI.Section("Config Manager")
local configName = UI.Input("Config name", "letters, numbers, _ or -")
UI.Button("Save config", function()
    local ok = UI.SaveConfig(UI.InputValue(configName))
    UI.SetLabel(settingsStatus, ok and "Config saved" or "Save failed")
end)
UI.Button("Load config", function()
    local ok = UI.LoadConfig(UI.InputValue(configName))
    UI.SetLabel(settingsStatus, ok and "Config loaded" or "Load failed")
end)
UI.Button("Delete config", function()
    local ok = UI.DeleteConfig(UI.InputValue(configName))
    UI.SetLabel(settingsStatus, ok and "Config deleted" or "Delete failed")
end)

UI.Section("Luau Manager")
UI.Label("Version: " .. tostring(VERSION_HASH))
local scriptName = UI.Input("Script filename", "example.luau")
UI.Button("Run script", function()
    local ok = UI.RunFile(UI.InputValue(scriptName))
    UI.SetLabel(settingsStatus, ok and "Script ran" or "Script failed")
end)
UI.Button("Rescan Roblox", function()
    UI.SetLabel(settingsStatus, UI.Rescan() and "Roblox attached" or "Rescan failed")
end)

UI.Section("Keybinds")
UI.Keybind("Toggle UI", 36, function() UI.ToggleVisible() end)
UI.Keybind("Unload", 35, function() UI.RequestUnload() end)
UI.Tab("Main")
UI.Section("General")
print("[stdlib] UI loaded")
)LUA";
