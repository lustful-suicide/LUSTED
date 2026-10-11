-- Keep character collisions disabled while the Humanoid changes state.
ui_tab("Official")
ui_section("Player")

local function myCharacter()
    local current = get_character()
    if current then return Instance.from(current) end
    return character
end

local clipId = ui_toggle("Noclip", false)
local tickId
local cache = { character = 0, refreshedAt = 0, parts = {} }
local originalFlags = {}
local pendingCollisionReports = {}
local lastCollisionReport = 0

local function eachPart(ch, callback)
    if not ch then return end
    for _, descendant in ipairs(ch:GetDescendants()) do
        local ok, className, name = pcall(function()
            return descendant.ClassName, descendant.Name
        end)
        if ok and has_prop(className, "CanCollide") then
            callback(descendant:addr(), className, name)
        end
    end
end

local function refresh(ch, now)
    local characterAddress = ch:addr()
    if cache.character ~= characterAddress then
        originalFlags = {}
    end
    cache.character = characterAddress
    cache.refreshedAt = now
    cache.parts = {}

    eachPart(ch, function(address, className, name)
        cache.parts[#cache.parts + 1] = {
            address = address,
            className = className,
            name = name,
        }

        if not originalFlags[address] then
            originalFlags[address] = {
                canCollide = get_prop(address, "BasePart", "CanCollide"),
                canTouch = get_prop(address, "BasePart", "CanTouch"),
                canQuery = get_prop(address, "BasePart", "CanQuery"),
            }
        end
        set_prop(address, "BasePart", "CanTouch", false)
        set_prop(address, "BasePart", "CanQuery", false)
    end)

end

local function enforce()
    local ch = myCharacter()
    if not ch then return end

    local now = os.clock()
    if ch:addr() ~= cache.character or now - cache.refreshedAt >= 1 then
        refresh(ch, now)
    end

    for _, part in ipairs(cache.parts) do
        if get_prop(part.address, "BasePart", "CanCollide") ~= false then
            local wrote = set_prop(part.address, "BasePart", "CanCollide", false)
            pendingCollisionReports[part.address] = part.name .. " [" .. part.className .. "] write=" .. tostring(wrote)
        end
    end

    if now - lastCollisionReport >= 1 and next(pendingCollisionReports) then
        local report = {}
        for _, part in pairs(pendingCollisionReports) do
            report[#report + 1] = part
        end
        print("[noclip] reasserted CanCollide=false: " .. table.concat(report, ", "))
        pendingCollisionReports = {}
        lastCollisionReport = now
    end
end

local function restore()
    eachPart(myCharacter(), function(address)
        local original = originalFlags[address]
        if not original then return end
        if original.canCollide ~= nil then
            set_prop(address, "BasePart", "CanCollide", original.canCollide)
        end
        if original.canTouch ~= nil then
            set_prop(address, "BasePart", "CanTouch", original.canTouch)
        end
        if original.canQuery ~= nil then
            set_prop(address, "BasePart", "CanQuery", original.canQuery)
        end
    end)
    originalFlags = {}
    pendingCollisionReports = {}
    cache.character = 0
    cache.refreshedAt = 0
    cache.parts = {}
end

ui_on_changed(clipId, function()
    if ui_toggle_state(clipId) then
        if not tickId then tickId = on_tick(enforce) end
    else
        if tickId then
            remove_tick(tickId)
            tickId = nil
        end
        restore()
    end
end)

print("[official] noclip loaded")
