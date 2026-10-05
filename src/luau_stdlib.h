#pragma once

inline constexpr char kInstanceLibrary[] = R"LUA(
local Vector3Meta = {}
Vector3Meta.__index = Vector3Meta
Vector3Meta.__tostring = function(v)
    return string.format("%.2f, %.2f, %.2f", v.X, v.Y, v.Z)
end
Vector3Meta.__add = function(a, b) return Vector3(a.X + b.X, a.Y + b.Y, a.Z + b.Z) end
Vector3Meta.__sub = function(a, b) return Vector3(a.X - b.X, a.Y - b.Y, a.Z - b.Z) end
Vector3Meta.__mul = function(a, b)
    if type(b) == "number" then return Vector3(a.X * b, a.Y * b, a.Z * b) end
    return Vector3(a.X * b.X, a.Y * b.Y, a.Z * b.Z)
end
Vector3Meta.__div = function(a, b)
    if type(b) == "number" then return Vector3(a.X / b, a.Y / b, a.Z / b) end
    return Vector3(a.X / b.X, a.Y / b.Y, a.Z / b.Z)
end
Vector3Meta.__eq = function(a, b) return a.X == b.X and a.Y == b.Y and a.Z == b.Z end
Vector3Meta.__len = function(v) return math.sqrt(v.X * v.X + v.Y * v.Y + v.Z * v.Z) end
Vector3Meta.Magnitude = function(self) return math.sqrt(self.X * self.X + self.Y * self.Y + self.Z * self.Z) end
Vector3Meta.Dot = function(self, other) return self.X * other.X + self.Y * other.Y + self.Z * other.Z end
Vector3Meta.Unit = function(self)
    local m = self:Magnitude()
    if m == 0 then return Vector3(0, 0, 0) end
    return Vector3(self.X / m, self.Y / m, self.Z / m)
end

local Instance = {}
Instance.__index = Instance

function Instance.from(address)
    assert(address ~= nil, "nil instance address")
    return setmetatable({ _address = address }, Instance)
end

function Instance:addr()
    return self._address
end

function Instance:GetParent()
    local address = get_parent(self._address)
    return address and Instance.from(address) or nil
end

function Instance:GetChildren()
    local result = {}
    for _, address in ipairs(get_children(self._address) or {}) do
        result[#result + 1] = Instance.from(address)
    end
    return result
end

function Instance:GetDescendants()
    local result = {}
    for _, address in ipairs(get_descendants(self._address) or {}) do
        result[#result + 1] = Instance.from(address)
    end
    return result
end

function Instance:FindFirstChild(name, recursive)
    if not recursive then
        local address = find_first_child(self._address, name)
        return address and Instance.from(address) or nil
    end
    for _, descendant in ipairs(self:GetDescendants()) do
        if descendant.Name == name then return descendant end
    end
    return nil
end

function Instance:FindFirstChildOfClass(className)
    local address = find_first_child_of_class(self._address, className)
    return address and Instance.from(address) or nil
end

function Instance:WaitForChild(name, timeout)
    local address = wait_for_child(self._address, name, timeout or 5000)
    return address and Instance.from(address) or nil
end

function Instance:Resolve(path)
    local address = resolve_path(self._address, path)
    return address and Instance.from(address) or nil
end

function Instance:GetProperty(className, memberName)
    return get_prop(self._address, className or self.ClassName, memberName)
end

function Instance:SetProperty(className, memberName, value)
    return set_prop(self._address, className or self.ClassName, memberName, value)
end

function Instance:HasProperty(memberName)
    return has_prop(self.ClassName, memberName)
end

function Instance:PropOffset(memberName)
    return prop_offset(self.ClassName, memberName)
end

-- property -> child -> nil
Instance.__index = function(self, key)
    local method = rawget(Instance, key)
    if method ~= nil then return method end
    if key == "Name" then return get_name(self._address) end
    if key == "ClassName" then return get_classname(self._address) end
    if key == "Parent" then return self:GetParent() end
    if key == "Children" then return self:GetChildren() end
    local address = self._address
    local value = get_prop(address, get_classname(address), key)
    if value ~= nil then
        if type(value) == "string" and value:match("^0x") then
            return Instance.from(value)
        end
        return value
    end
    local child = find_first_child(address, key)
    return child and Instance.from(child) or nil
end

function refresh_game()
    local dataModel = get_datamodel()
    game = dataModel and Instance.from(dataModel) or nil
    local workspaceAddress = get_workspace()
    workspace = workspaceAddress and Instance.from(workspaceAddress) or
        (game and game:FindFirstChild("Workspace"))
    players = game and game:FindFirstChild("Players") or nil
    local playerAddress = get_localplayer()
    localplayer = playerAddress and Instance.from(playerAddress) or nil
    local characterAddress = get_character()
    character = characterAddress and Instance.from(characterAddress) or nil
    local humanoidAddress = get_humanoid()
    humanoid = humanoidAddress and Instance.from(humanoidAddress) or nil
    return game
end

function get_health(instance)
    instance = instance or humanoid
    return instance and get_prop_float(instance:addr(), "Humanoid", "Health") or nil
end

function set_walkspeed(value, instance)
    instance = instance or humanoid
    return instance and set_prop_float(instance:addr(), "Humanoid", "Walkspeed", value) or false
end

-- CFrame block: {r00..r22, px, py, pz}
function get_cframe(instance)
    if not instance then return nil end
    local addr = instance.addr and instance:addr() or instance
    return raw_get_cframe(addr, get_classname(addr))
end

function set_cframe(instance, cf)
    if not instance or type(cf) ~= "table" then return false end
    local addr = instance.addr and instance:addr() or instance
    return raw_set_cframe(addr, get_classname(addr), cf)
end

-- position-only write; keeps the current rotation block intact
function set_position(instance, x, y, z)
    if not instance then return false end
    local addr = instance.addr and instance:addr() or instance
    return set_prop(addr, get_classname(addr), "Position", { X = x, Y = y, Z = z })
end

function get_position(instance)
    if not instance then return nil end
    local addr = instance.addr and instance:addr() or instance
    return get_prop(addr, get_classname(addr), "Position")
end

function set_jumppower(value, instance)
    instance = instance or humanoid
    return instance and set_prop_float(instance:addr(), "Humanoid", "JumpPower", value) or false
end

function get_rootpart(instance)
    instance = instance or character
    if instance then
        local part = instance:FindFirstChild("HumanoidRootPart")
        if part then return part end
        -- R6 nests the root part one level down under Torso.
        for _, limb in ipairs(instance:GetChildren()) do
            local nested = limb:FindFirstChild("HumanoidRootPart")
            if nested then return nested end
        end
    end
    local address = raw_get_rootpart()
    return address and Instance.from(address) or nil
end

function print_tree(instance, depth)
    instance = instance or game
    depth = depth or 2
    local function visit(node, level)
        if level > depth then return end
        print(string.rep("  ", level) .. tostring(node.Name) .. " [" .. tostring(node.ClassName) .. "]")
        for _, child in ipairs(node:GetChildren()) do visit(child, level + 1) end
    end
    if instance then visit(instance, 0) end
end

-- Vector3 helpers (returned tables from get_prop)
function Vector3(x, y, z)
    return setmetatable({ X = x or 0, Y = y or 0, Z = z or 0 }, Vector3Meta)
end
function IsVector3(value)
    return type(value) == "table" and getmetatable(value) == Vector3Meta
end

function get_players()
    local result = {}
    for _, address in ipairs(raw_get_players() or {}) do
        result[#result + 1] = Instance.from(address)
    end
    return result
end

function get_player_count()
    return raw_get_player_count()
end

function player_by_name(target)
    target = tostring(target):lower()
    for _, player in ipairs(get_players()) do
        if tostring(player.Name):lower() == target then return player end
    end
    return nil
end

-- HumanoidRootPart of any player (or a Character model).
function get_player_root(target)
    local player = type(target) == "string" and player_by_name(target) or target
    if not player then return nil end
    local char = player.Character or player.ModelInstance
    if not char then return nil end
    return get_rootpart(char)
end

function refresh_index(force)
    return raw_refresh_index(force == true)
end

function index_stats()
    return raw_index_stats()
end

refresh_game()
print("[stdlib] Instance loaded; game=" .. tostring(game and game:addr()))
)LUA";

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

inline constexpr char kDrawingLibrary[] = R"LUA(
Drawing = {}
Drawing.__index = Drawing

function Drawing.new(drawType)
    drawType = drawType or "Line"
    local id = drawing_new(drawType)
    assert(id ~= 0, "unsupported drawing type: " .. tostring(drawType))
    return setmetatable({ _id = id }, Drawing)
end

function Drawing:Remove()
    if self._id ~= 0 then
        drawing_destroy(self._id)
        rawset(self, "_id", 0)
    end
end
function Drawing:Set(property, value) return drawing_set(self._id, property, value) end
function Drawing:Get(property) return drawing_get(self._id, property) end
function Drawing.clear() return drawing_clear() end

Drawing.__newindex = function(self, key, value)
    if key == "_id" then rawset(self, key, value); return end
    assert(drawing_set(self._id, key, value), "unsupported drawing property: " .. tostring(key))
end
Drawing.__index = function(self, key)
    local method = rawget(Drawing, key)
    if method ~= nil then return method end
    return drawing_get(self._id, key)
end
print("[stdlib] Drawing loaded")
)LUA";
