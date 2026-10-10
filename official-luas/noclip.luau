-- Official: noclip (collision flags off on every part of YOUR character,
-- enforced every frame: the server restores flags on physics state changes
-- (jump/land), so a one-shot write never survives. Touch/Query go too so
-- sensor-based zones can't fake a wall mid-air either.
ui_tab("Official")
ui_section("Player")
local PART_CLASSES = {
    Part = true, MeshPart = true, BasePart = true, WedgePart = true,
    CornerWedgePart = true, TrussPart = true, UnionOperation = true,
    Seat = true, VehicleSeat = true, SpawnLocation = true, Handle = true,
}
local FLAG_MEMBERS = { "CanCollide", "CanTouch", "CanQuery" }
local function myChar()
    local ca = get_character()
    if ca then return Instance.from(ca) end
    return character
end
local function partsOf(model)
    local parts = {}
    if not model then return parts end
    for _, d in ipairs(model:GetDescendants()) do
        local ok, cn = pcall(function() return d.ClassName end)
        if ok and PART_CLASSES[cn] then parts[#parts + 1] = d end
    end
    return parts
end
local clipId = ui_toggle("Noclip", false)
local tickId = nil
local function enforce()
    -- Unconditional writes every tick: no read-check gap for replication wins.
    local ch = myChar()
    if not ch then return end
    for _, p in ipairs(partsOf(ch)) do
        local a, cn = p:addr(), p.ClassName
        set_prop(a, cn, "CanCollide", false)
        set_prop(a, cn, "CanTouch", false)
        set_prop(a, cn, "CanQuery", false)
    end
end
local function restore()
    local ch = myChar()
    if not ch then return end
    for _, p in ipairs(partsOf(ch)) do
        local a, cn = p:addr(), p.ClassName
        set_prop(a, cn, "CanCollide", true)
        set_prop(a, cn, "CanTouch", true)
        set_prop(a, cn, "CanQuery", true)
    end
end
ui_on_changed(clipId, function()
    if ui_toggle_state(clipId) then
        if not tickId then tickId = on_tick(enforce) end
    else
        if tickId then remove_tick(tickId) tickId = nil end
        restore()
    end
end)
print("[official] noclip loaded")
