-- Official: teleport to a player by name.
ui_tab("Official")
ui_section("Teleport")

local nameId = ui_input("Player name", "exact display name")
local function myHRP()
    local ca = get_character()
    local ch = (ca and Instance.from(ca)) or character
    if not ch then return nil end
    return ch:FindFirstChild("HumanoidRootPart") or get_rootpart(ch)
end

local function targetCFrame(name)
    local player = player_by_name(name)
    local model = player and (player.ModelInstance or player.Character)
    local root = model and (model:FindFirstChild("HumanoidRootPart") or get_rootpart(model))
    local cf = root and get_cframe(root)
    if type(cf) ~= "table" then return nil end
    for i = 1, 12 do
        local v = cf[i]
        if type(v) ~= "number" or v ~= v or v == math.huge or v == -math.huge then
            return nil
        end
    end
    return cf
end

local function positionOf(root)
    if not root then return nil end
    local value = get_prop(root:addr(), root.ClassName, "Position")
    if type(value) ~= "table" then return nil end
    local x, y, z = value.X, value.Y, value.Z
    if type(x) ~= "number" or type(y) ~= "number" or type(z) ~= "number" then
        return nil
    end
    return x, y, z
end

local function distanceSquared(ax, ay, az, bx, by, bz)
    local dx, dy, dz = ax - bx, ay - by, az - bz
    return dx * dx + dy * dy + dz * dz
end

local SLAM_SECONDS, VERIFY_SECONDS, MAX_TRIES = 0.4, 0.15, 6
local TARGET_REFRESH_SECONDS, ROOT_REFRESH_SECONDS = 0.08, 0.1
local ARRIVAL_DISTANCE_SQUARED = 9
local tpId = ui_button("Teleport")
local busy = false

ui_on_pressed(tpId, function()
    if busy then return end
    busy = true

    local ok, result = pcall(function()
        local name = ui_input_value(nameId)
        if not name or name == "" then return "empty name" end
        if not targetCFrame(name) then return "no such player/character" end
        print("[official] teleport: chasing " .. name)

        for attempt = 1, MAX_TRIES do
            local destination = targetCFrame(name)
            local mine = myHRP()
            if not destination or not mine then return "lost target or local root" end

            local targetX, targetY, targetZ = destination[10], destination[11], destination[12]
            local started = os.clock()
            local nextTargetRefresh = started + TARGET_REFRESH_SECONDS
            local nextRootRefresh = started + ROOT_REFRESH_SECONDS
            local writes, successfulWrites = 0, 0
            local alive = true

            -- Dense writes are intentional: isolated transform writes are
            -- reverted by the simulation before the body adopts the move.
            while os.clock() - started < SLAM_SECONDS do
                local now = os.clock()
                if now >= nextTargetRefresh then
                    local fresh = targetCFrame(name)
                    if fresh then
                        destination = fresh
                        targetX, targetY, targetZ = fresh[10], fresh[11], fresh[12]
                    end
                    nextTargetRefresh = now + TARGET_REFRESH_SECONDS
                end
                if now >= nextRootRefresh then
                    local freshRoot = myHRP()
                    if not freshRoot then alive = false break end
                    mine = freshRoot
                    nextRootRefresh = now + ROOT_REFRESH_SECONDS
                end

                if set_cframe(mine, destination) then successfulWrites = successfulWrites + 1 end
                writes = writes + 1
            end

            if alive and writes > 0 and successfulWrites > 0 then
                -- Do not write during verification. A fresh root lookup also
                -- prevents a respawn from making stale-instance reads look valid.
                local verifyRoot = myHRP()
                if verifyRoot and verifyRoot:addr() == mine:addr() then
                    local samples, near = 0, 0
                    local verifyStarted = os.clock()
                    local nextSample = verifyStarted
                    while os.clock() - verifyStarted < VERIFY_SECONDS do
                        local now = os.clock()
                        if now >= nextSample then
                            local x, y, z = positionOf(verifyRoot)
                            if x then
                                samples = samples + 1
                                if distanceSquared(x, y, z, targetX, targetY, targetZ)
                                    < ARRIVAL_DISTANCE_SQUARED then
                                    near = near + 1
                                end
                            end
                            nextSample = now + 0.025
                        end
                    end
                    if samples >= 3 and near / samples >= 0.8 then
                        return "arrived (try " .. attempt .. ", " .. successfulWrites .. "/" .. writes .. " writes)"
                    end
                end
            elseif not alive then
                return "local character respawned during teleport"
            end
        end
        return "gave up: destination did not hold after successful writes"
    end)

    busy = false
    if ok then
        print("[official] teleport: " .. result)
    else
        print("[official] teleport failed: " .. tostring(result))
    end
end)

print("[official] teleport loaded")
