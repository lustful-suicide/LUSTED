-- Official: movement speed (mirrored Walkspeed/WalkspeedCheck, kick-safe).
ui_tab("Official")
ui_section("Movement")
local speedId = ui_slider("WalkSpeed", 16, 250, 16)
ui_on_changed(speedId, function()
    local v = ui_slider_value(speedId)
    local ha = get_humanoid()
    local hum = (ha and Instance.from(ha)) or humanoid
    if hum then set_walkspeed(v, hum) end
end)
print("[official] speed loaded")
