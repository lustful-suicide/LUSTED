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
