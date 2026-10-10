-- Official: jump power (mirrored JumpPower/JumpHeight, kick-safe).
ui_tab("Official")
ui_section("Movement")
local jumpId = ui_slider("JumpPower", 7, 300, 50)
ui_on_changed(jumpId, function()
    local v = ui_slider_value(jumpId)
    local ha = get_humanoid()
    local hum = (ha and Instance.from(ha)) or humanoid
    if hum then set_jumppower(v, hum) end
end)
print("[official] jump loaded")
