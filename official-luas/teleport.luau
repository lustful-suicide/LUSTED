-- Official: teleport to a player by name (writes THEIR hrp CFrame to YOUR hrp,
-- repeated until close: the server snaps lone writes back within a frame or
-- two, so one shot looks like "nothing happens").
ui_tab("Official")
ui_section("Teleport")
local nameId = ui_input("Player name", "exact display name")
local function myHRP()
    local ca = get_character()
    local ch = (ca and Instance.from(ca)) or character
    if not ch then return nil end
    return ch:FindFirstChild("HumanoidRootPart") or get_rootpart(ch)
end
local function hrpPos(hrp)
    if not hrp then return nil end
    local v = get_prop(hrp:addr(), hrp.ClassName, "Position")
    if type(v) ~= "table" then return nil end
    return v.X, v.Y, v.Z
end
local repId, repLeft, repCF, repName = nil, 0, nil, nil
local function stopRep()
    if repId then remove_tick(repId) repId = nil end
    repLeft, repCF, repName = 0, nil, nil
end
local tpId = ui_button("Teleport")
ui_on_pressed(tpId, function()
    local nm = ui_input_value(nameId)
    if not nm or nm == "" then print("[official] teleport: empty name") return end
    local me = myHRP()
    if not me then print("[official] teleport: no local HRP") return end
    local tpl = player_by_name(nm)
    if not tpl then print("[official] teleport: no such player") return end
    local tchar = tpl.ModelInstance or tpl.Character
    if not tchar then print("[official] teleport: no target character") return end
    local thrp = tchar:FindFirstChild("HumanoidRootPart") or get_rootpart(tchar)
    if not thrp then print("[official] teleport: no target HRP") return end
    local cf = get_cframe(thrp)
    if not cf then print("[official] teleport: no target cframe") return end
    stopRep()
    repCF, repName, repLeft = cf, nm, 90
    repId = on_tick(function()
        if repLeft <= 0 then stopRep() return end
        repLeft = repLeft - 1
        -- Re-resolve the target every tick (it moves; a stale copy chases air).
        local t2 = player_by_name(repName)
        local tc2 = t2 and (t2.ModelInstance or t2.Character)
        local th2 = tc2 and (tc2:FindFirstChild("HumanoidRootPart") or get_rootpart(tc2))
        if th2 then
            local dst = get_cframe(th2)
            if dst then repCF = dst end
        end
        local mine = myHRP()
        if not mine then stopRep() return end
        set_cframe(mine, repCF)
        local x1, y1, z1 = hrpPos(mine)
        if x1 and th2 then
            local qx, qy, qz = hrpPos(th2)
            if qx then
                local dx, dy, dz = x1 - qx, y1 - qy, z1 - qz
                if dx * dx + dy * dy + dz * dz < 64 then
                    print("[official] teleport: arrived")
                    stopRep()
                end
            end
        end
    end)
    print("[official] teleport: chasing " .. nm)
end)
print("[official] teleport loaded")
