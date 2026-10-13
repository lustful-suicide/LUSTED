-- Official: hold-key aimbot (closest-to-crosshair inside FOV, smoothed mouse move).
-- Needs the is_key_down / mouse_move natives; without them it runs as a
-- target indicator only (FOV circle + snap line, no mouse movement).
-- Aim with Roblox focused; the overlay center is the crosshair reference.

ui_tab("Official")
ui_section("Aimbot")

local VK_RBUTTON = 0x02
local enabledId = ui_toggle("Enabled", false)
local aimKeyId = ui_keybind("Aim key", VK_RBUTTON)
local fovId = ui_slider("FOV radius", 40, 400, 120)
local smoothId = ui_slider("Smoothing", 1, 20, 6)
local partId = ui_dropdown("Target part", { "Head", "HumanoidRootPart", "Closest to crosshair" }, 1)
local showFovId = ui_toggle("Show FOV", true)
local showSnapId = ui_toggle("Show snap line", true)
local statusId = 0
if type(ui_label) == "function" then
    local ok, id = pcall(ui_label, "Status: starting")
    if ok and type(id) == "number" then statusId = id end
end

local HAS_INPUT = type(is_key_down) == "function" and type(mouse_move) == "function"
if not HAS_INPUT then
    print("[official] aimbot: rebuild for mouse natives (indicator-only mode)")
end

local FOV_COLOR = { 120, 220, 255 }
local SNAP_COLOR = { 255, 80, 80 }
local MAX_STEP = 80
local DEADZONE = 0.5
local STICK_BONUS = 20
local PLAYER_REFRESH_TICKS = 30

local fovCircle, snapLine = 0, 0
if type(drawing_new) == "function" then
    local ok, id = pcall(drawing_new, "Circle")
    if ok and type(id) == "number" then fovCircle = id end
    ok, id = pcall(drawing_new, "Line")
    if ok and type(id) == "number" then snapLine = id end
end
if fovCircle ~= 0 then
    pcall(drawing_set, fovCircle, "Color", FOV_COLOR)
    pcall(drawing_set, fovCircle, "Thickness", 1)
    pcall(drawing_set, fovCircle, "Filled", false)
end
if snapLine ~= 0 then
    pcall(drawing_set, snapLine, "Color", SNAP_COLOR)
    pcall(drawing_set, snapLine, "Thickness", 1)
end

local function finite(v)
    return type(v) == "number" and v == v and v ~= math.huge and v ~= -math.huge
end

local function getMatrix()
    if type(get_viewmatrix) ~= "function" then return nil end
    local ok, matrix = pcall(get_viewmatrix)
    if not ok or type(matrix) ~= "table" then return nil end
    for i = 1, 16 do
        if not finite(matrix[i]) then return nil end
    end
    return matrix
end

local function getOverlaySize()
    if type(get_overlay_size) ~= "function" then return nil end
    local ok, size = pcall(get_overlay_size)
    if not ok or type(size) ~= "table" or
        not (finite(size[1]) and finite(size[2])) or size[1] <= 0 or size[2] <= 0 then
        return nil
    end
    return size[1], size[2]
end

local function project(matrix, width, height, x, y, z)
    local clipX = matrix[1] * x + matrix[2] * y + matrix[3] * z + matrix[4]
    local clipY = matrix[5] * x + matrix[6] * y + matrix[7] * z + matrix[8]
    local clipW = matrix[13] * x + matrix[14] * y + matrix[15] * z + matrix[16]
    if not finite(clipW) or clipW <= 0.1 then return nil end
    local sx = (clipX / clipW * 0.5 + 0.5) * width
    local sy = (0.5 - clipY / clipW * 0.5) * height
    if not (finite(sx) and finite(sy)) then return nil end
    return sx, sy
end

local function readPosition(root, className)
    if type(get_position) == "function" then
        local ok, x, y, z = pcall(get_position, root, className)
        if ok and finite(x) and finite(y) and finite(z) then
            return x, y, z
        end
    end
    local getter = type(get_cframe) == "function" and get_cframe or raw_get_cframe
    if type(getter) == "function" then
        local ok, cf = pcall(getter, root)
        if ok and type(cf) == "table" and finite(cf[10]) and
            finite(cf[11]) and finite(cf[12]) then
            return cf[10], cf[11], cf[12]
        end
    end
    return nil
end

local function rootClassOf(root)
    if type(get_classname) == "function" then
        local ok, className = pcall(get_classname, root)
        if ok and type(className) == "string" and className ~= "" then
            return className
        end
    end
    return nil
end

local function characterModel(player)
    if type(get_prop) == "function" and type(get_classname) == "function" then
        local okClass, className = pcall(get_classname, player)
        if okClass and type(className) == "string" and className ~= "" then
            local okModel, model = pcall(get_prop, player, className, "ModelInstance")
            if not (okModel and type(model) == "string") then
                okModel, model = pcall(get_prop, player, className, "Character")
            end
            if okModel and type(model) == "string" then
                return model
            end
        end
    end
    if type(find_first_child_of_class) == "function" then
        local ok, model = pcall(find_first_child_of_class, player, "Model")
        if ok and type(model) == "string" then return model end
    end
    return nil
end

local function isAlive(model)
    if type(find_first_child) ~= "function" or type(get_prop) ~= "function" then
        return true
    end
    local ok, humanoid = pcall(find_first_child, model, "Humanoid")
    if not ok or type(humanoid) ~= "string" then return true end
    local okClass, className = pcall(get_classname, humanoid)
    if not okClass or type(className) ~= "string" or className == "" then return true end
    local okHealth, health = pcall(get_prop, humanoid, className, "Health")
    if not okHealth or type(health) ~= "number" then return true end
    return health > 0
end

local function aimRoots(model, mode)
    if model == nil or type(find_first_child) ~= "function" then return nil end
    local okHrp, hrp = pcall(find_first_child, model, "HumanoidRootPart")
    if not okHrp or hrp == nil then return nil end
    if mode == 2 then return { hrp } end
    local okHead, head = pcall(find_first_child, model, "Head")
    if mode == 1 then
        if okHead and head ~= nil then return { head } end
        return { hrp }
    end
    if okHead and head ~= nil then return { head, hrp } end
    return { hrp }
end

local tickCount = 0
local cachedPlayers = {}
local cachedLocalPlayer = nil
local lastTarget = nil
local lastFov, lastCx, lastCy = nil, nil, nil
local snapVisible, fovVisible = false, false

local function setVisible(id, show, flag)
    if id == 0 or flag == show then return show end
    pcall(drawing_set, id, "Visible", show)
    return show
end

local function hideAll()
    snapVisible = setVisible(snapLine, false, snapVisible)
    fovVisible = setVisible(fovCircle, false, fovVisible)
end

-- One-line self-diagnosis in the panel: every "it does nothing" state names
-- its cause (toggle off, old exe, key released, no target in FOV).
local function updateStatus(enabled, fov, aimKey, held, best, bestDist)
    if statusId == 0 or tickCount % 15 ~= 0 then return end
    local text
    if not HAS_INPUT then
        text = "Status: rebuild lusted.exe (no mouse natives)"
    elseif not enabled then
        text = "Status: OFF - tick Enabled"
    elseif not held then
        text = string.format("Status: ON - hold key %d | FOV %d", aimKey, fov)
    elseif best == nil then
        text = string.format("Status: ON - key held, no target in %dpx", fov)
    else
        local name = nil
        if type(get_name) == "function" then
            local ok, value = pcall(get_name, best)
            if ok and type(value) == "string" and value ~= "" then
                name = string.sub(value, 1, 24)
            end
        end
        text = string.format("Status: aiming %s [%dpx]",
            name or best, math.floor(bestDist + 0.5))
    end
    pcall(ui_set_label, statusId, text)
end

local function tick()
    tickCount += 1

    if tickCount % PLAYER_REFRESH_TICKS == 0 or #cachedPlayers == 0 then
        local okPlayers, players = pcall(raw_get_players)
        if okPlayers and type(players) == "table" then cachedPlayers = players end
        if type(get_localplayer) == "function" then
            local ok, value = pcall(get_localplayer)
            if ok and type(value) == "string" then cachedLocalPlayer = value end
        end
    end

    local enabled = false
    if type(ui_toggle_state) == "function" then
        local ok, value = pcall(ui_toggle_state, enabledId)
        if ok then enabled = value == true end
    end
    local fov = 120
    if type(ui_slider_value) == "function" then
        local ok, value = pcall(ui_slider_value, fovId)
        if ok and type(value) == "number" and value > 0 then fov = value end
    end
    local smoothing = 6
    if type(ui_slider_value) == "function" then
        local ok, value = pcall(ui_slider_value, smoothId)
        if ok and type(value) == "number" and value >= 1 then smoothing = value end
    end
    local partMode = 1
    if type(ui_dropdown_value) == "function" then
        local ok, value = pcall(ui_dropdown_value, partId)
        if ok and type(value) == "number" and value >= 1 and value <= 3 then partMode = value end
    end
    local showFov, showSnap = true, true
    if type(ui_toggle_state) == "function" then
        local ok, value = pcall(ui_toggle_state, showFovId)
        if ok then showFov = value == true end
        ok, value = pcall(ui_toggle_state, showSnapId)
        if ok then showSnap = value == true end
    end
    local aimKey = VK_RBUTTON
    if type(ui_keybind_value) == "function" then
        local ok, value = pcall(ui_keybind_value, aimKeyId)
        if ok and type(value) == "number" and value > 0 and value < 256 then aimKey = value end
    end
    local held = false
    if HAS_INPUT then
        local ok, down = pcall(is_key_down, aimKey)
        if ok then held = down == true end
    end

    local matrix = getMatrix()
    local width, height = getOverlaySize()
    if matrix == nil or width == nil then hideAll(); return end
    local cx, cy = width * 0.5, height * 0.5

    if fovCircle ~= 0 then
        if showFov and enabled then
            if lastFov ~= fov or lastCx ~= cx or lastCy ~= cy then
                pcall(drawing_set, fovCircle, "Position", cx, cy)
                pcall(drawing_set, fovCircle, "Radius", fov)
                lastFov, lastCx, lastCy = fov, cx, cy
            end
            fovVisible = setVisible(fovCircle, true, fovVisible)
        else
            fovVisible = setVisible(fovCircle, false, fovVisible)
        end
    end

    local best, bestX, bestY, bestDist = nil, nil, nil, nil
    local stickX, stickY, stickDist = nil, nil, nil
    for i = 1, #cachedPlayers do
        local player = cachedPlayers[i]
        if type(player) == "string" and player ~= cachedLocalPlayer then
            local ok, model = pcall(characterModel, player)
            if ok and model ~= nil then
                local aliveOk, alive = pcall(isAlive, model)
                if aliveOk and alive then
                    local rootsOk, roots = pcall(aimRoots, model, partMode)
                    if rootsOk and type(roots) == "table" then
                        for r = 1, #roots do
                            local root = roots[r]
                            local className = rootClassOf(root)
                            local x, y, z = readPosition(root, className)
                            if x ~= nil then
                                local sx, sy = project(matrix, width, height, x, y, z)
                                if sx ~= nil and sx >= 0 and sx <= width and sy >= 0 and sy <= height then
                                    local dx, dy = sx - cx, sy - cy
                                    local dist = math.sqrt(dx * dx + dy * dy)
                                    if dist <= fov and (bestDist == nil or dist < bestDist) then
                                        best, bestX, bestY, bestDist = player, sx, sy, dist
                                    end
                                    if player == lastTarget and (stickDist == nil or dist < stickDist) then
                                        stickX, stickY, stickDist = sx, sy, dist
                                    end
                                end
                            end
                        end
                    end
                end
            end
        end
    end

    -- Prefer the sticky target while the key is held: keeps aim from flicking
    -- between two overlapping targets inside the same FOV.
    if best ~= nil and lastTarget ~= nil and stickDist ~= nil and best ~= lastTarget
        and stickDist <= bestDist + STICK_BONUS then
        best, bestX, bestY, bestDist = lastTarget, stickX, stickY, stickDist
    end
    lastTarget = best

    if snapLine ~= 0 then
        if showSnap and enabled and best ~= nil then
            pcall(drawing_set, snapLine, "From", cx, cy)
            pcall(drawing_set, snapLine, "To", bestX, bestY)
            snapVisible = setVisible(snapLine, true, snapVisible)
        else
            snapVisible = setVisible(snapLine, false, snapVisible)
        end
    end

    if best == nil or not enabled or not HAS_INPUT then
        updateStatus(enabled, fov, aimKey, held, best, bestDist)
        return
    end
    if not held then
        updateStatus(enabled, fov, aimKey, held, best, bestDist)
        return
    end

    local dx, dy = bestX - cx, bestY - cy
    if math.abs(dx) < DEADZONE and math.abs(dy) < DEADZONE then
        updateStatus(enabled, fov, aimKey, held, best, bestDist)
        return
    end
    dx, dy = dx / smoothing, dy / smoothing
    local step = math.sqrt(dx * dx + dy * dy)
    if step > MAX_STEP then
        dx, dy = dx / step * MAX_STEP, dy / step * MAX_STEP
    end
    pcall(mouse_move, math.floor(dx + 0.5), math.floor(dy + 0.5))
    updateStatus(enabled, fov, aimKey, held, best, bestDist)
end

if type(on_tick) == "function" then
    pcall(on_tick, tick)
end

print("[official] aimbot loaded")
