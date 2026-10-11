#include "roblox/instance.h"
#include "platform/windows/memory.h"
#include "support/log.h"
#include <fstream>
#include <thread>
#include <chrono>
#include <cctype>
#include <algorithm>
#include <cstring>
#include <iostream>
#include <sstream>
#include <tlhelp32.h>
#include <immintrin.h>
#include <nlohmann/json.hpp>

using Clock = std::chrono::steady_clock;

// ------------------------------------------------------------------ properties

namespace {

// Compact Roblox inheritance for property lookup (offsets.json is flat).
const std::unordered_map<std::string, std::string>& ParentMap() {
    static const std::unordered_map<std::string, std::string> map = {
        {"Instance","Instance"},{"PVInstance","Instance"},{"Model","PVInstance"},
        {"BasePart","PVInstance"},{"Part","BasePart"},{"WedgePart","Part"},
        {"TrussPart","BasePart"},{"SpawnLocation","Part"},{"Seat","Part"},
        {"VehicleSeat","Seat"},{"MeshPart","BasePart"},{"UnionOperation","BasePart"},
        {"Humanoid","PVInstance"},{"Tool","Item"},{"Item","PVInstance"},
        {"Accessory","PVInstance"},{"Clothing","PVInstance"},{"Decal","PVInstance"},
        {"Texture","PVInstance"},{"Script","LuaSourceContainer"},
        {"LuaSourceContainer","Instance"},{"LocalScript","LuaSourceContainer"},
        {"ModuleScript","LuaSourceContainer"},{"ClickDetector","Instance"},
        {"ProximityPrompt","GuiBase"},{"GuiBase","Instance"},{"GuiBase3d","Instance"},
        {"GuiBase2d","GuiBase3d"},{"LayerCollector","GuiBase3d"},{"GuiObject","LayerCollector"},
        {"Frame","GuiObject"},{"TextLabel","GuiObject"},{"TextButton","GuiObject"},
        {"TextBox","GuiObject"},{"ImageLabel","GuiObject"},{"ImageButton","GuiObject"},
        {"Player","PVInstance"},{"Team","PVInstance"},{"ForceField","PVInstance"},
        {"Attachment","PVInstance"},{"Weld","Instance"},{"Constraint","Instance"},
        {"AnimationTrack","Instance"},{"Animator","Instance"},{"Sound","Instance"},
        {"Lighting","PVInstance"},{"Sky","PVInstance"},{"Workspace","PVInstance"},
        {"Players","PVInstance"},{"ReplicatedStorage","PVInstance"},
        {"StarterGui","StarterPack"},{"StarterPack","PVInstance"},{"Lighting","PVInstance"},
        {"Terrain","PVInstance"},{"TerrainRegion","PVInstance"},{"Baseplate","Part"},
        {"Bone","Instance"},{"Motor6D","Instance"},{"WeldConstraint","Instance"},
        {"BillboardGui","LayerCollector"},{"SurfaceGui","GuiBase3d"},
        {"BodyMover","PVInstance"},{"BodyPosition","BodyMover"},{"BodyVelocity","BodyMover"},
        {"BodyGyro","BodyMover"},{"BodyThrust","BodyMover"},{"BodyForce","BodyMover"},
        {"AlignPosition","PVInstance"},{"AlignOrientation","PVInstance"},
        {"AngularVelocity","PVInstance"},{"LinearVelocity","PVInstance"},
        {"HumanoidRootPart","Part"},{"IntValue","ValueBase"},{"ValueBase","Instance"},
        {"NumberValue","ValueBase"},{"StringValue","ValueBase"},{"BoolValue","ValueBase"},
        {"Camera","PVInstance"},{"Selection","PVInstance"},{"SoundService","PVInstance"},
    };
    return map;
}

// Members stored as three consecutive floats.
const std::unordered_set<std::string>& Vec3Pairs() {
    static const std::unordered_set<std::string> set = {
        "Primitive.Position","Primitive.Size","Primitive.AssemblyLinearVelocity",
        "Primitive.AssemblyAngularVelocity","Attachment.Position","Attachment.Scale",
        "SpecialMesh.Scale","DataModelMesh.Scale","AirProperties.GlobalWind",
        "Humanoid.CameraOffset","Humanoid.MoveDirection","Humanoid.TargetPoint",
        "Humanoid.MoveToPoint","Humanoid.WalkDirection","Lighting.LightDirection",
        "Lighting.SunPosition","Lighting.MoonPosition","DragDetector.MaxDragTranslation",
        "DragDetector.MinDragTranslation","Sky.SkyboxOrientation","Ray.CFrame",
    };
    return set;
}

const std::unordered_set<std::string>& Color3Pairs() {
    static const std::unordered_set<std::string> set = {
        "Atmosphere.Color","Atmosphere.Decay","ColorCorrectionEffect.TintColor",
        "GuiObject.BackgroundColor3","GuiObject.BorderColor3","GuiObject.TextColor3",
        "BasePart.Color3","GuiBase3d.Color3","Color3Value.Value","BodyColors.HeadColor",
        "BodyColors.LeftArmColor3","BodyColors.RightLegColor3","SurfaceAppearance.Color",
        "SurfaceAppearance.EmissiveTint","Sparkles.SparkleColor","Light.Color",
        "Lighting.Ambient","Lighting.FogColor","Lighting.OutdoorAmbient","Lighting.ShadowColor",
        "Lighting.ColorShift_Top","Lighting.ColorShift_Bottom","Lighting.GradientTop",
        "Lighting.GradientBottom","Lighting.LightColor","Terrain.WaterColor",
        "Clothing.Color3","SelectionBox.SurfaceColor3","SelectionSphere.SurfaceColor3",
        "TextLabel.TextColor3","Frame.BackgroundColor3","BillboardGui.TextColor3",
        "ParticleEmitter.Color","Fire.Color","Smoke.Color","Beam.Color3",
    };
    return set;
}

const std::unordered_set<std::string>& RbxStringNames() {
    static const std::unordered_set<std::string> set = {
        "DisplayName","Name","JobId","ServerIP","ScriptGuid","GUID","Hash","Source",
        "LocaleId","MouseIcon","CursorIcon","ActivatedCursorIcon","SoundId","MeshId",
        "BaseTextureId","OverlayTextureId","Texture","Icon","Tooltip","Text","RichText",
        "Template","SkyboxBk","SkyboxDn","SkyboxFt","SkyboxLf","SkyboxRt","SkyboxUp",
        "SunTextureId","MoonTextureId","ColorMap","ColorMapContent","EmissiveMaskContent",
        "MetalnessMap","MetalnessMapContent","NormalMap","NormalMapContent","RoughnessMap",
        "RoughnessMapContent","EmissiveTint","LinkedSource","CachedRemoteSource",
        "ClipsDescendants","StateName","KeyName","AnimationId","WalkAnimation","RunAnimation",
        "IdleAnimation","JumpAnimation","FallAnimation","SwimAnimation","ClimbAnimation",
        "Shirt","Pants","GraphicTShirt","Face",
    };
    return set;
}

const std::unordered_set<std::string>& BoolNames() {
    static const std::unordered_set<std::string> set = {
        "Anchored","CanCollide","CanTouch","CanQuery","Locked","Massless","CastShadow",
        "Looped","IsPlaying","Enabled","Visible","Seated","Jump","Sit","PlatformStand",
        "RequiresNeck","EvaluateStateMachine","BreakJointsOnDeath","LocalPlayer",
        "GlobalWind","Frozen","Shadows","GlobalShadows","CloudsEnabled","AutoRotate","AutoJumpEnabled",
        "UseJumpPower","StreaksOn","CharacterAutoLoads","Neutral","Loaded",
        "Archivable","Robust","Persistent","KeepVelocity","AssemblyLinearVelocity",
        "TouchedGui","Grounded","Stiffness","Adornee","Bevel","ClipsDescendants",
        "ScrollBarInset","AutomaticSize","Interactable","Selectable","Draggable",
        "UseJumpPower","AutoLoadCharacter","HumanoidOnlySetCollisionsOnStateChange",
    };
    return set;
}

const std::unordered_set<std::string>& IntNames() {
    static const std::unordered_set<std::string> set = {
        "TeamColor","BrickColor","Size","Shape","Material","RigType","HealthDisplayType",
        "HealthDisplayDistance","NameDisplayDistance","NameOcclusion","DisplayDistanceType",
        "FloorMaterial","MaxSlopeAngle","CameraType","ZoomDistance","UserId","AccountAge",
        "PlaceId","GameId","CameraMode","CameraMinZoomDistance","CameraMaxZoomDistance",
        "TimeScale","Rotation","FontSize","TextSize","LayoutOrder","ZIndex","Priority",
        "PlaceVersion","JobId","MouseIcon","CameraMinZoom","SimulationRadius",
    };
    return set;
}

const std::unordered_set<std::string>& PtrNames() {
    static const std::unordered_set<std::string> set = {
        "Value","Primitive","Parent","Workspace","Team","SeatPart","HumanoidRootPart",
        "Texture","Script","LinkedSource","Adornee","Handle","MeshId","BaseTextureId",
        "OverlayTextureId","Template","SoundId","SkyboxBk","SkyboxDn","SkyboxFt","SkyboxLf",
        "SkyboxRt","SkyboxUp","SunTextureId","MoonTextureId","CameraSubject",
        "LocalPlayer","ModelInstance","Mouse","Icon","World","AirProperties","Character",
        "Animation","Animator","PrimaryPart","Part0","Part1","SoundGroup","ToolTip",
    };
    return set;
}

} // namespace

std::vector<std::string> InstanceStore::ClassChain(const std::string& cls) const {
    std::vector<std::string> chain;
    chain.reserve(4);
    std::string cur = cls;
    for (int guard = 0; guard < 16 && !cur.empty(); ++guard) {
        chain.push_back(cur);
        auto it = ParentMap().find(cur);
        if (it == ParentMap().end() || it->second == cur) break;
        cur = it->second;
    }
    if (chain.empty()) chain.push_back(cls);
    return chain;
}

PropInfo InstanceStore::ResolveProp(uintptr_t inst, const std::string& clsIn,
                                     const std::string& member) const {
    PropInfo out;
    std::string cls = clsIn;
    if (cls.empty() && inst) {
        // class names are cached, so this stays cheap
        cls = const_cast<InstanceStore*>(this)->GetClass(inst);
    }
    if (cls.empty() || member.empty()) return out;

    // table_ is immutable after Load, so the answer for a (class, member) pair
    // never changes. Property reads happen on every script access, and the
    // resolution walks ancestors, sub-object chains and the whole table, so
    // the first miss pays for it and every later hit is a hash lookup.
    // Zero-alloc hit path: stack key + transparent lookup (old CacheKey
    // allocated a std::string per call, even on hits).
    char stack[256];
    std::string heap;
    std::string_view keyView;
    size_t need = cls.size() + 1 + member.size();
    if (need <= sizeof(stack)) {
        memcpy(stack, cls.data(), cls.size());
        stack[cls.size()] = '\x1f';
        memcpy(stack + cls.size() + 1, member.data(), member.size());
        keyView = std::string_view(stack, need);
    } else {
        heap.reserve(need);
        heap.append(cls).push_back('\x1f');
        heap.append(member);
        keyView = std::string_view(heap);
    }
    {
        std::shared_lock lk(propMutex_);
        auto it = propCache_.find(keyView);
        if (it != propCache_.end()) return it->second;
    }
    PropInfo r = ResolvePropRaw(cls, member);
    {
        std::unique_lock lk(propMutex_);
        if (propCache_.size() < (1u << 16)) {
            std::string owned;
            owned.reserve(need);
            owned.append(cls).push_back('\x1f');
            owned.append(member);
            propCache_.emplace(std::move(owned), r);
        }
    }
    return r;
}

PropInfo InstanceStore::ResolvePropRaw(const std::string& cls,
                                       const std::string& member) const {
    PropInfo out;
    auto chain = ClassChain(cls);
    auto typeFor = [&](const std::string& c, const std::string& m) {
        PropInfo p;
        const std::string key = c + "." + m;
        if (Vec3Pairs().count(key)) p.type = PropType::Vec3;
        else if (Color3Pairs().count(key)) p.type = PropType::C3;
        else if (RbxStringNames().count(m)) p.type = PropType::RbxString;
        else if (BoolNames().count(m)) p.type = PropType::Bool;
        else if (PtrNames().count(m)) p.type = PropType::Ptr;
        else if (IntNames().count(m)) p.type = PropType::I32;
        else p.type = PropType::F32;
        return p;
    };

    // CFrame is not a field of its own: Primitive stores a 3x3 rotation
    // (9 floats) immediately followed by the position (3 floats). offsets.json
    // is authoritative, so the block is only accepted when the two entries it
    // reports are actually adjacent - otherwise there is no contiguous CFrame.
    if (member == "CFrame") {
        auto rot = OffsetOf("Primitive", "Rotation");
        auto pos = OffsetOf("Primitive", "Position");
        if (rot && pos && *pos == *rot + (int64_t)(sizeof(float) * 9)) {
            for (const auto& c : chain) {
                auto hop = OffsetOf(c, "Primitive");
                if (!hop) continue;
                out.type = PropType::CF;
                out.offset = *rot;
                out.hops = { *hop };
                return out;
            }
        }
    }

    // 1) direct field on the class or an ancestor
    for (const auto& c : chain) {
        if (auto o = OffsetOf(c, member)) {
            out = typeFor(c, member);
            out.offset = *o;
            out.hops.clear();
            return out;
        }
        // offsets.json spells some members differently by case from the runtime
        // name (dump "Walkspeed" vs Roblox "WalkSpeed"); exact match won above,
        // so this only fires when there is no exact spelling to prefer.
        if (auto hit = LookupLower(c, member)) {
            out = typeFor(hit->realClass, hit->realMember);
            out.offset = hit->offset;
            out.hops.clear();
            return out;
        }
    }

    // 2) sub-object chains: Part -> BasePart.Primitive -> Primitive.Position,
    //    Workspace -> World -> World.Gravity. Each hop is a pointer member.
    //    subs is the Load-time cached list (old code rebuilt+copied it per miss).
    {
        const auto& subs = subClasses_;
        std::vector<int64_t> hops;
        hops.reserve(3);
        std::vector<std::string> frontier = chain;
        for (int depth = 0; depth < 3 && !frontier.empty(); ++depth) {
            std::vector<std::string> next;
            for (const auto& c : frontier) {
                for (const auto& sub : subs) {
                    auto hop = OffsetOf(c, sub);
                    if (!hop) continue;
                    int64_t fieldOff = 0;
                    std::string realMember = member;
                    if (auto field = OffsetOf(sub, member)) fieldOff = *field;
                    else if (auto hit = LookupLower(sub, member)) {
                        fieldOff = hit->offset;
                        realMember = hit->realMember;
                    } else continue;
                    PropInfo p = typeFor(sub, realMember);
                    p.offset = fieldOff;
                    p.hops = hops;
                    p.hops.push_back(*hop);
                    return p;
                }
            }
            for (const auto& c : frontier) {
                for (const auto& sub : subs) {
                    if (!OffsetOf(c, sub)) continue;
                    auto hop = OffsetOf(c, sub);
                    hops.push_back(*hop);
                    next.push_back(sub);
                    break;
                }
            }
            frontier = next;
        }
    }

    // 3) unique global match (member exists on exactly one class).
    // Bitfield owners (*Flags, e.g. PrimitiveFlags.CanCollide=8) are resolved
    // as bit ops on the matching Flags field, NEVER as raw offsets: the raw
    // value is a bit index, and a byte write there lands on unrelated memory
    // (for parts: base+8, the This pointer) and corrupts the target.
    {
        const std::string* owner = nullptr;
        int count = 0;
        for (const auto& [c, members] : table_) {
            if (!members.count(member)) continue;
            ++count;
            owner = &c;
        }
        if (count == 1 && owner) {
            const std::string& oc = *owner;
            if (oc.size() > 5 && oc.compare(oc.size() - 5, 5, "Flags") == 0) {
                // "PrimitiveFlags" -> base object "Primitive" via the chain hop
                // (BasePart.Primitive), field "Flags" on it (Primitive.Flags).
                // The dump value is a BITMASK (CanCollide=8 tests bit 3).
                const std::string base = oc.substr(0, oc.size() - 5);
                auto maskVal = OffsetOf(oc, member);
                if (maskVal && *maskVal > 0) {
                    for (const auto& c : chain) {
                        auto hop = OffsetOf(c, base);
                        auto field = OffsetOf(base, "Flags");
                        if (!hop || !field) continue;
                        out.type = PropType::Bool;
                        out.offset = *field;
                        out.hops = { *hop };
                        out.mask = *maskVal;
                        return out;
                    }
                }
                return out; // !ok: safe fail, never a raw byte write
            }
            out = typeFor(oc, member);
            out.offset = *OffsetOf(oc, member);
            out.hops.clear();
            return out;
        }
        if (count == 0) {
            // no exact spelling anywhere: accept an unambiguous case-only match
            const std::string lm = LowerKey(member);
            const OffsetLookup* hit = nullptr;
            int hits = 0;
            for (const auto& [lowerCls, members] : lowerTable_) {
                auto it = members.find(lm);
                if (it == members.end()) continue;
                ++hits;
                hit = &it->second;
            }
            if (hits == 1 && hit) {
                // Same bitfield guard as above: never hand out a Flags bit
                // index as a raw byte offset.
                if (hit->realClass.size() > 5 &&
                    hit->realClass.compare(hit->realClass.size() - 5, 5, "Flags") == 0)
                    return out;
                out = typeFor(hit->realClass, hit->realMember);
                out.offset = hit->offset;
                out.hops.clear();
                return out;
            }
        }
    }
    return out;
}

bool InstanceStore::HasProp(const std::string& cls, const std::string& member) const {
    return ResolveProp(0, cls, member).ok();
}

uintptr_t InstanceStore::PropBase(const PropInfo& p, uintptr_t inst) const {
    if (!p.ok() || !inst) return 0;
    uintptr_t cur = inst;
    for (int64_t hop : p.hops) {
        uintptr_t next = 0;
        if (!mem_->CustomRead(cur + (uintptr_t)hop, &next, sizeof(next))) return 0;
        if (!FastReadable(next)) return 0;
        cur = next;
    }
    return cur;
}

bool InstanceStore::ReadPropFloat(const PropInfo& p, uintptr_t inst, float& out) const {
    if (!p.ok() || !inst || !mem_) return false;
    uintptr_t base = PropBase(p, inst);
    if (!base) return false;
    return mem_->CustomRead(base + (uintptr_t)p.offset, &out, sizeof(float));
}
bool InstanceStore::WritePropFloat(const PropInfo& p, uintptr_t inst, float v) const {
    if (!p.ok() || !inst || !mem_) return false;
    uintptr_t base = PropBase(p, inst);
    if (!base) return false;
    return mem_->Write<float>(base + (uintptr_t)p.offset, v);
}
bool InstanceStore::ReadPropBool(const PropInfo& p, uintptr_t inst, bool& out) const {
    if (!p.ok() || p.type != PropType::Bool || !inst || !mem_) return false;
    uintptr_t base = PropBase(p, inst);
    if (!base) return false;
    const uintptr_t field = base + (uintptr_t)p.offset;
    if (p.mask != 0) {
        if (p.mask <= 0xFF) {
            uint8_t value = 0;
            if (!mem_->CustomRead(field, &value, sizeof(value))) return false;
            out = (value & (uint8_t)p.mask) != 0;
            return true;
        }
        if (p.mask <= 0xFFFFFFFFLL) {
            uint32_t w = 0;
            if (!mem_->CustomRead(field, &w, sizeof(w))) return false;
            out = (w & (uint32_t)p.mask) != 0;
            return true;
        }
        uint64_t w = 0;
        if (!mem_->CustomRead(field, &w, sizeof(w))) return false;
        out = (w & (uint64_t)p.mask) != 0;
        return true;
    }
    uint8_t v = 0;
    if (!mem_->CustomRead(field, &v, sizeof(v))) return false;
    out = v != 0;
    return true;
}
bool InstanceStore::WritePropBool(const PropInfo& p, uintptr_t inst, bool value) const {
    if (!p.ok() || p.type != PropType::Bool || !inst || !mem_) return false;
    uintptr_t base = PropBase(p, inst);
    if (!base) return false;
    const uintptr_t field = base + (uintptr_t)p.offset;
    if (p.mask != 0) {
        // Preserve neighboring flag bits while touching only the stored width.
        if (p.mask <= 0xFF) {
            uint8_t flags = 0;
            if (!mem_->CustomRead(field, &flags, sizeof(flags))) return false;
            if (value) flags |= (uint8_t)p.mask;
            else flags &= (uint8_t)~p.mask;
            return mem_->Write<uint8_t>(field, flags);
        }
        if (p.mask <= 0xFFFFFFFFLL) {
            uint32_t w = 0;
            if (!mem_->CustomRead(field, &w, sizeof(w))) return false;
            if (value) w |= (uint32_t)p.mask;
            else w &= ~(uint32_t)p.mask;
            return mem_->Write<uint32_t>(field, w);
        }
        uint64_t w = 0;
        if (!mem_->CustomRead(field, &w, sizeof(w))) return false;
        if (value) w |= (uint64_t)p.mask;
        else w &= ~(uint64_t)p.mask;
        return mem_->Write<uint64_t>(field, w);
    }
    const uint8_t v = value ? 1 : 0;
    return mem_->CustomWrite(field, &v, sizeof(v));
}
std::optional<int64_t> InstanceStore::OffsetOfInsensitive(const std::string& cls, const std::string& member) const {
    if (auto o = OffsetOf(cls, member)) return o;
    auto hit = LookupLower(cls, member);
    return hit ? std::optional<int64_t>(hit->offset) : std::nullopt;
}
bool InstanceStore::WritePropFloatMirrored(uintptr_t inst, const std::string& clsIn, const std::string& memberIn, float value) const {
    if (!inst || !mem_) return false;
    std::string cls = clsIn;
    if (cls.empty() && inst) cls = const_cast<InstanceStore*>(this)->GetClass(inst);
    if (cls.empty()) cls = "Humanoid";
    std::string lc = LowerKey(cls), lm = LowerKey(memberIn);
    std::vector<std::string> mirrors;
    if (lc == "humanoid") {
        if (lm == "walkspeed" || lm == "walkspeedcheck") mirrors = {"Walkspeed", "WalkspeedCheck"};
        else if (lm == "jumppower" || lm == "jumpheight") mirrors = {"JumpPower", "JumpHeight"};
    }
    if (!mirrors.empty()) {
        std::vector<std::pair<std::string, PropInfo>> targets;
        for (auto& mm : mirrors) {
            PropInfo p = ResolveProp(inst, cls, mm);
            if (!p.ok()) {
                auto off = OffsetOfInsensitive(cls, mm);
                if (!off) continue;
                p.type = PropType::F32;
                p.offset = *off;
                p.hops.clear();
            }
            targets.emplace_back(mm, p);
        }
        if (targets.empty()) return false;
        bool allOk = true;
        for (auto& [name, p] : targets) {
            uintptr_t base = PropBase(p, inst);
            if (!base) { allOk = false; continue; }
            if (!mem_->Write<float>(base + (uintptr_t)p.offset, value)) allOk = false;
        }
        return allOk;
    }
    PropInfo p = ResolveProp(inst, cls, memberIn);
    if (!p.ok()) {
        auto off = OffsetOfInsensitive(cls, memberIn);
        if (!off) return false;
        p.type = PropType::F32;
        p.offset = *off;
        p.hops.clear();
    }
    uintptr_t base = PropBase(p, inst);
    if (!base) return false;
    return mem_->Write<float>(base + (uintptr_t)p.offset, value);
}
bool InstanceStore::ReadPropVec3(const PropInfo& p, uintptr_t inst, float out[3]) const {
    if (!p.ok() || !inst || !mem_) return false;
    uintptr_t base = PropBase(p, inst);
    if (!base) return false;
    return mem_->CustomRead(base + (uintptr_t)p.offset, out, sizeof(float) * 3);
}
bool InstanceStore::WritePropVec3(const PropInfo& p, uintptr_t inst, const float v[3]) const {
    if (!p.ok() || !inst || !mem_) return false;
    uintptr_t base = PropBase(p, inst);
    if (!base) return false;
    return mem_->CustomWrite(base + (uintptr_t)p.offset, v, sizeof(float) * 3);
}
bool InstanceStore::ReadPropCFrame(const PropInfo& p, uintptr_t inst, float out[12]) const {
    if (!p.ok() || p.type != PropType::CF || !inst || !mem_) return false;
    uintptr_t base = PropBase(p, inst);
    if (!base) return false;
    return mem_->CustomRead(base + (uintptr_t)p.offset, out, sizeof(float) * 12);
}
bool InstanceStore::WritePropCFrame(const PropInfo& p, uintptr_t inst, const float v[12]) const {
    if (!p.ok() || p.type != PropType::CF || !inst || !mem_) return false;
    uintptr_t base = PropBase(p, inst);
    if (!base) return false;
    return mem_->CustomWriteFast(base + (uintptr_t)p.offset, v, sizeof(float) * 12);
}
std::string InstanceStore::ReadPropString(const PropInfo& p, uintptr_t inst) const {
    if (!p.ok() || !inst) return "";
    uintptr_t base = PropBase(p, inst);
    if (!base) return "";
    return ReadRobloxString(base + (uintptr_t)p.offset, 128);
}

std::vector<std::string> InstanceStore::MembersOfClass(const std::string& cls) const {
    std::vector<std::string> out;
    if (cls.empty()) return out;
    std::unordered_set<std::string> seen;
    for (const auto& c : ClassChain(cls)) {
        auto it = table_.find(c);
        if (it == table_.end()) continue;
        for (const auto& [m, off] : it->second) {
            (void)off;
            if (seen.insert(m).second) out.push_back(m);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::string InstanceStore::ReadPropDisplay(uintptr_t inst, const std::string& cls,
                                           const std::string& member) const {
    if (!inst || !mem_) return "-";
    PropInfo p = ResolveProp(inst, cls, member);
    if (!p.ok()) return "-";
    uintptr_t base = PropBase(p, inst);
    if (!base) return "-";
    const uintptr_t field = base + (uintptr_t)p.offset;
    char b[128];
    switch (p.type) {
        case PropType::Vec3:
        case PropType::C3: {
            float v[3] = {};
            if (!mem_->CustomRead(field, v, sizeof(v))) return "?";
            snprintf(b, sizeof(b), "(%.4g, %.4g, %.4g)", v[0], v[1], v[2]);
            return b;
        }
        case PropType::CF: {
            float v[12] = {};
            if (!ReadPropCFrame(p, inst, v)) return "?";
            snprintf(b, sizeof(b), "pos(%.3g, %.3g, %.3g)", v[9], v[10], v[11]);
            return b;
        }
        case PropType::Bool: {
            bool v = false;
            if (!ReadPropBool(p, inst, v)) return "?";
            return v ? "true" : "false";
        }
        case PropType::I32: {
            int32_t v = 0;
            if (!mem_->CustomRead(field, &v, 4)) return "?";
            snprintf(b, sizeof(b), "%d", v);
            return b;
        }
        case PropType::U32: {
            uint32_t v = 0;
            if (!mem_->CustomRead(field, &v, 4)) return "?";
            snprintf(b, sizeof(b), "%u", v);
            return b;
        }
        case PropType::U64:
        case PropType::Ptr: {
            uintptr_t v = 0;
            if (!mem_->CustomRead(field, &v, 8)) return "?";
            if (!v) return "nil";
            snprintf(b, sizeof(b), "0x%llX", (unsigned long long)v);
            return b;
        }
        case PropType::Vec2: {
            float v[2] = {};
            if (!mem_->CustomRead(field, v, sizeof(v))) return "?";
            snprintf(b, sizeof(b), "(%.4g, %.4g)", v[0], v[1]);
            return b;
        }
        case PropType::RbxString:
        case PropType::String: {
            std::string v = ReadPropString(p, inst);
            if (v.size() > 64) v.resize(64);
            return v.empty() ? "\"\"" : ("\"" + v + "\"");
        }
        default: {
            float v = 0;
            if (!ReadPropFloat(p, inst, v)) return "?";
            snprintf(b, sizeof(b), "%.4g", v);
            return b;
        }
    }
}

