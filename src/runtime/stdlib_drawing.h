#pragma once

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
