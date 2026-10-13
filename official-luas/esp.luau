-- Player overlay using this runtime's row-major world-to-clip matrix.
local BOX_COLOR = { 255, 70, 70 }
local NAME_COLOR = { 255, 255, 255 }
local DIST_COLOR = { 170, 255, 170 }
local NAME_SIZE = 13
local DIST_SIZE = 12
local MIN_H = 12
local MAX_H = 1200
local PLAYER_REFRESH_FRAMES = 30
local CHARACTER_REFRESH_FRAMES = 45

local entries = {}
local tickCount = 0
local renderFrame = 0
local cachedPlayers
local lastPlayersFrame = -PLAYER_REFRESH_FRAMES
local cachedLocalPlayer
local cachedCameraPosition
local lastCameraFrame = -2

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

local function childModelWithRoot(player)
    local ok, children = pcall(get_children, player)
    if not ok or type(children) ~= "table" then return nil end
    local fallback
    for i = 1, #children do
        local child = children[i]
        if type(child) == "string" then
            local okClass, className = pcall(get_classname, child)
            if okClass and className == "Model" then
                fallback = fallback or child
                local okRoot, root = pcall(find_first_child, child, "HumanoidRootPart")
                if okRoot and root ~= nil then return child, root end
            end
        end
    end
    return fallback
end

local function characterRoot(player)
    if type(get_prop) == "function" and type(get_classname) == "function" then
        local okClass, className = pcall(get_classname, player)
        if okClass and type(className) == "string" and className ~= "" then
            local okModel, model = pcall(get_prop, player, className, "ModelInstance")
            if not (okModel and type(model) == "string") then
                okModel, model = pcall(get_prop, player, className, "Character")
            end
            if okModel and type(model) == "string" then
                local okRoot, root = pcall(find_first_child, model, "HumanoidRootPart")
                if okRoot and root ~= nil then return root, model end
            end
        end
    end

    if type(find_first_child_of_class) == "function" then
        local ok, model = pcall(find_first_child_of_class, player, "Model")
        if ok and type(model) == "string" then
            local rootOk, root = pcall(find_first_child, model, "HumanoidRootPart")
            if rootOk and root ~= nil then return root, model end
        end
    end
    local model, root = childModelWithRoot(player)
    if model ~= nil and root ~= nil then return root, model end
    return nil
end

local function modelHeight(model, root)
    local hipHeight = 2
    local okHumanoid, humanoid = pcall(find_first_child, model, "Humanoid")
    if okHumanoid and type(humanoid) == "string" then
        local okClass, className = pcall(get_classname, humanoid)
        if okClass and type(className) == "string" and className ~= "" then
            local okHeight, value = pcall(get_prop, humanoid, className, "HipHeight")
            if okHeight and finite(value) and value >= 0 and value < 10 then
                hipHeight = value
            end
        end
    end

    local halfRootHeight = 1
    local okClass, className = pcall(get_classname, root)
    if okClass and type(className) == "string" and className ~= "" then
        local okSize, size = pcall(get_prop, root, className, "Size")
        if okSize and type(size) == "table" and finite(size[2]) and size[2] > 0 then
            halfRootHeight = size[2] * 0.5
        end
    end

    local bottom = math.max(2.5, hipHeight + halfRootHeight)
    local top = math.max(2.5, hipHeight + halfRootHeight - 0.5)
    return top, bottom
end

local function rootHalfExtents(root)
    local okClass, className = pcall(get_classname, root)
    if okClass and type(className) == "string" and className ~= "" then
        local okSize, size = pcall(get_prop, root, className, "Size")
        if okSize and type(size) == "table" and
            finite(size[1]) and finite(size[3]) and size[1] > 0 and size[3] > 0 then
            return math.max(0.75, size[1] * 0.5), math.max(0.75, size[3] * 0.5)
        end
    end
    return 1, 0.75
end

local function hide(entry)
    if entry.shown then
        if entry.box ~= 0 then drawing_set(entry.box, "Visible", false) end
        if entry.name ~= 0 then drawing_set(entry.name, "Visible", false) end
        entry.shown = false
        entry.lastMinX = nil
    end
    if entry.distShown then
        if entry.dist ~= 0 then drawing_set(entry.dist, "Visible", false) end
        entry.distShown = false
    end
end

local function destroy(key)
    local entry = entries[key]
    if entry == nil then return end
    entries[key] = nil
    if entry.box ~= 0 then drawing_destroy(entry.box) end
    if entry.name ~= 0 then drawing_destroy(entry.name) end
    if entry.dist ~= 0 then drawing_destroy(entry.dist) end
end

local function getEntry(key)
    local entry = entries[key]
    if entry ~= nil then return entry end
    local box, name, dist = 0, 0, 0
    if type(drawing_new) == "function" then
        local okBox, boxId = pcall(drawing_new, "Box")
        if okBox and type(boxId) == "number" then box = boxId end
        local okName, nameId = pcall(drawing_new, "Text")
        if okName and type(nameId) == "number" then name = nameId end
        local okDist, distId = pcall(drawing_new, "Text")
        if okDist and type(distId) == "number" then dist = distId end
    end
    entry = {
        box = box, name = name, dist = dist,
        shown = true, distShown = true,
        root = nil, model = nil, lastMetadataFrame = -CHARACTER_REFRESH_FRAMES,
        topOffset = 2.5, bottomOffset = 2.5, halfX = 1, halfZ = 0.75,
        label = nil, distanceText = nil,
    }
    entries[key] = entry
    if box ~= 0 then
        pcall(drawing_set, box, "Color", BOX_COLOR)
        pcall(drawing_set, box, "Thickness", 1)
        pcall(drawing_set, box, "Filled", false)
    end
    if name ~= 0 then
        pcall(drawing_set, name, "Color", NAME_COLOR)
        pcall(drawing_set, name, "Size", NAME_SIZE)
        pcall(drawing_set, name, "Center", true)
    end
    if dist ~= 0 then
        pcall(drawing_set, dist, "Color", DIST_COLOR)
        pcall(drawing_set, dist, "Size", DIST_SIZE)
        pcall(drawing_set, dist, "Center", true)
    end
    hide(entry)
    return entry
end

local function refreshCharacter(entry, player)
    local root, model = characterRoot(player)
    entry.root, entry.model = root, model
    entry.lastMetadataFrame = renderFrame
    if root == nil or model == nil then return false end
    entry.topOffset, entry.bottomOffset = modelHeight(model, root)
    entry.rootClass = nil
    if type(get_classname) == "function" then
        local ok, value = pcall(get_classname, root)
        if ok and type(value) == "string" then entry.rootClass = value end
    end
    entry.halfX, entry.halfZ = rootHalfExtents(root)
    entry.label = nil
    if type(get_name) == "function" then
        local ok, value = pcall(get_name, player)
        if ok and type(value) == "string" and value ~= "" then
            entry.label = string.sub(value, 1, 48)
        end
    end
    return true
end

local function setShown(entry)
    if entry.shown then return end
    if entry.box ~= 0 then drawing_set(entry.box, "Visible", true) end
    if entry.name ~= 0 then drawing_set(entry.name, "Visible", true) end
    entry.shown = true
end

local function drawPlayer(matrix, width, height, cameraPosition, player)
    local entry = getEntry(player)
    if renderFrame - entry.lastMetadataFrame >= CHARACTER_REFRESH_FRAMES then
        refreshCharacter(entry, player)
    end
    if entry.root == nil or entry.model == nil then hide(entry); return end
    local x, y, z = readPosition(entry.root, entry.rootClass)
    if x == nil then
        if not refreshCharacter(entry, player) then hide(entry); return end
        x, y, z = readPosition(entry.root, entry.rootClass)
    end
    if x == nil then hide(entry); return end
    -- Two projections (head/feet) instead of the full 8-corner box: the same
    -- on-screen box for an upright character at a quarter of the Lua cost.
    local headX, headY = project(matrix, width, height, x, y + entry.topOffset, z)
    if headX == nil then hide(entry); return end
    local feetX, feetY = project(matrix, width, height, x, y - entry.bottomOffset, z)
    if feetX == nil then hide(entry); return end
    local centerX = (headX + feetX) * 0.5
    local minY = headY < feetY and headY or feetY
    local maxY = headY < feetY and feetY or headY

    local boxHeight = maxY - minY
    if boxHeight < MIN_H then
        local centerY = (minY + maxY) * 0.5
        minY, maxY = centerY - MIN_H * 0.5, centerY + MIN_H * 0.5
        boxHeight = MIN_H
    elseif boxHeight > MAX_H then
        local centerY = (minY + maxY) * 0.5
        minY, maxY = centerY - MAX_H * 0.5, centerY + MAX_H * 0.5
        boxHeight = MAX_H
    end

    local boxWidth = boxHeight / 2.2
    local minX = centerX - boxWidth * 0.5
    local maxX = centerX + boxWidth * 0.5
    if maxX < 0 or minX > width or maxY < 0 or minY > height then
        hide(entry)
        return
    end

    local nameText = entry.label or "?"

    local distanceText
    if type(cameraPosition) == "table" and finite(cameraPosition[1]) and
        finite(cameraPosition[2]) and finite(cameraPosition[3]) then
        local dx, dy, dz = x - cameraPosition[1], y - cameraPosition[2], z - cameraPosition[3]
        distanceText = string.format("[%dm]", math.floor(math.sqrt(dx * dx + dy * dy + dz * dz) + 0.5))
    end

    -- Skip the native round-trips when the box did not move: idle players
    -- re-dirtied the overlay every frame for zero visual change, and the
    -- repaint cost grew with the player count.
    local moved = entry.lastMinX == nil
        or math.abs(minX - entry.lastMinX) >= 0.75
        or math.abs(minY - entry.lastMinY) >= 0.75
        or math.abs(maxX - entry.lastMaxX) >= 0.75
        or math.abs(maxY - entry.lastMaxY) >= 0.75
    if moved then
        entry.lastMinX, entry.lastMinY = minX, minY
        entry.lastMaxX, entry.lastMaxY = maxX, maxY
        if entry.box ~= 0 then
            drawing_set(entry.box, "Position", minX, minY)
            drawing_set(entry.box, "Size", maxX - minX, maxY - minY)
        end
        if entry.name ~= 0 then
            drawing_set(entry.name, "Position", centerX, math.max(0, minY - NAME_SIZE - 2))
        end
        if entry.dist ~= 0 and distanceText ~= nil then
            local distY = math.min(height - DIST_SIZE - 2, maxY + 2)
            drawing_set(entry.dist, "Position", centerX, math.max(0, distY))
        end
    end
    if entry.name ~= 0 and entry.lastDrawnLabel ~= nameText then
        drawing_set(entry.name, "Text", nameText)
        entry.lastDrawnLabel = nameText
    end
    if entry.dist ~= 0 then
        if distanceText ~= nil then
            if entry.distanceText ~= distanceText then
                drawing_set(entry.dist, "Text", distanceText)
                entry.distanceText = distanceText
            end
            if not entry.distShown then
                drawing_set(entry.dist, "Visible", true)
                entry.distShown = true
            end
        elseif entry.distShown then
            drawing_set(entry.dist, "Visible", false)
            entry.distShown = false
        end
    end
    setShown(entry)
end

local function tick()
    tickCount += 1
    if tickCount % 4 ~= 0 then return end
    renderFrame += 1

    if cachedPlayers == nil or renderFrame - lastPlayersFrame >= PLAYER_REFRESH_FRAMES then
        local okPlayers, players = pcall(raw_get_players)
        if okPlayers and type(players) == "table" then cachedPlayers = players end
        if cachedPlayers == nil then cachedPlayers = {} end
        lastPlayersFrame = renderFrame
        if type(get_localplayer) == "function" then
            local ok, value = pcall(get_localplayer)
            if ok and type(value) == "string" then cachedLocalPlayer = value end
        end
    end
    local players = cachedPlayers

    local seen = {}
    for i = 1, #players do
        if type(players[i]) == "string" then seen[players[i]] = true end
    end
    for key in pairs(entries) do
        if not seen[key] then destroy(key) end
    end

    local matrix = getMatrix()
    local width, height = getOverlaySize()
    if matrix == nil or width == nil then
        for _, entry in pairs(entries) do hide(entry) end
        return
    end

    if renderFrame - lastCameraFrame >= 4 then
        if type(get_camera) == "function" then
            local ok, camera = pcall(get_camera)
            if ok and type(camera) == "table" and type(camera.pos) == "table" and
                finite(camera.pos[1]) and finite(camera.pos[2]) and finite(camera.pos[3]) then
                cachedCameraPosition = camera.pos
            end
        end
        lastCameraFrame = renderFrame
    end

    for i = 1, #players do
        local player = players[i]
        if type(player) == "string" and player ~= cachedLocalPlayer then
            local ok = pcall(drawPlayer, matrix, width, height, cachedCameraPosition, player)
            if not ok and entries[player] ~= nil then hide(entries[player]) end
        end
    end
end

if type(on_tick) == "function" then
    pcall(on_tick, tick)
end

print("[official] esp loaded")
