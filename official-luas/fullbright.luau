-- Official: fullbright (max ambient lighting while toggled on).
ui_tab("Official")
ui_section("Visual")
local brightId = ui_toggle("Fullbright", false)
local saved = nil
local function lighting()
    return game and game:FindFirstChild("Lighting")
end
local function grab(li, member)
    return li:GetProperty("Lighting", member)
end
local function apply(li, member, value)
    return li:SetProperty("Lighting", member, value)
end
ui_on_changed(brightId, function()
    local on = ui_toggle_state(brightId)
    local li = lighting()
    if not li then return end
    if on then
        saved = {
            Ambient = grab(li, "Ambient"),
            OutdoorAmbient = grab(li, "OutdoorAmbient"),
            Brightness = grab(li, "Brightness"),
            ExposureCompensation = grab(li, "ExposureCompensation"),
            FogEnd = grab(li, "FogEnd"),
            GlobalShadows = grab(li, "GlobalShadows"),
            ClockTime = grab(li, "ClockTime"),
        }
        apply(li, "Ambient", { 1, 1, 1 })
        apply(li, "OutdoorAmbient", { 1, 1, 1 })
        apply(li, "Brightness", 3)
        apply(li, "ExposureCompensation", 1.5)
        apply(li, "FogEnd", 100000)
        apply(li, "GlobalShadows", false)
        apply(li, "ClockTime", 12)
    elseif saved then
        for member, value in pairs(saved) do
            if value ~= nil then apply(li, member, value) end
        end
        saved = nil
    end
end)
print("[official] fullbright loaded")
