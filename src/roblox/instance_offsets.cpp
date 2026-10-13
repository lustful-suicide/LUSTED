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

// ------------------------------------------------------------------ offsets

std::string InstanceStore::LowerKey(const std::string& s) {
    std::string o = s;
    for (char& c : o) c = (char)tolower((unsigned char)c);
    return o;
}

void InstanceStore::RefreshCachedOffsets() {
    auto pick = [&](const char* c, const char* m, int64_t fb) -> int64_t {
        auto o = OffsetOf(c, m);
        return o ? *o : fb;
    };
    oParent_ = pick("Instance", "Parent", 104);
    oDesc_ = pick("Instance", "ClassDescriptor", 24);
    oCont_ = pick("Instance", "NameContainer", 112);
    oName_ = pick("Instance", "Name", 8);
    oClassName_ = pick("Instance", "ClassName", 8);
    oChildrenStart_ = pick("Instance", "ChildrenStart", 120);
    oWorkspace_ = pick("DataModel", "Workspace", 336);
    oLocalPlayer_ = pick("Player", "LocalPlayer", 288);
    oModelInstance_ = pick("Player", "ModelInstance", 648);
    oWsCamera_ = pick("Workspace", "CurrentCamera", 1192);
    oCamPos_ = pick("Camera", "Position", 0xEC);
    oCamRot_ = pick("Camera", "Rotation", 0xC8);
    oCamFov_ = pick("Camera", "FieldOfView", 0x130);
    oCamViewport_ = pick("Camera", "ViewportSize", 0x2CC);
    // Sub-object classes: any member name that is also a class name.
    // Computed once here (was rebuilt + copied per ResolveProp miss).
    subClasses_.clear();
    {
        std::unordered_set<std::string> found;
        found.reserve(table_.size());
        for (const auto& [c, members] : table_) {
            for (const auto& [member, off] : members) {
                (void)off;
                if (member == c) continue;
                if (table_.find(member) != table_.end()) found.insert(member);
            }
        }
        subClasses_.assign(found.begin(), found.end());
        std::sort(subClasses_.begin(), subClasses_.end());
    }
}

bool InstanceStore::Load(const std::string& path) {
    table_.clear();
    lowerTable_.clear();
    { std::unique_lock lk(propMutex_); propCache_.clear(); }
    loaded_ = false;
    manualDm_ = 0;
    cachedDm_ = 0;
    InvalidateIndex();
    std::ifstream f(path);
    if (!f) return false;
    try {
        auto j = nlohmann::json::parse(f);
        auto off = j.contains("Offsets") ? j["Offsets"] : j;
        table_.reserve(off.size() * 2);
        for (auto& cls : off.items()) {
            auto& dst = table_[cls.key()];
            for (auto& m : cls.value().items())
                if (m.value().is_number()) dst[m.key()] = m.value().get<int64_t>();
        }
        // Case-insensitive mirror of the whole table, built once here so that a
        // spelling difference between the dump and the runtime costs one hash
        // lookup instead of scanning every class and member.
        lowerTable_.reserve(table_.size() * 2);
        for (const auto& [cls, members] : table_) {
            auto& dst = lowerTable_[LowerKey(cls)];
            dst.reserve(members.size());
            for (const auto& [m, off] : members)
                dst.emplace(LowerKey(m), OffsetLookup{ cls, m, off });
        }
        loaded_ = true;
        RefreshCachedOffsets();
        { std::unique_lock lk(propMutex_); propCache_.reserve(1u << 14); }
        { std::unique_lock lk(metaMutex_); nameCache_.reserve(1u << 17); }
        return true;
    } catch (...) { return false; }
}

std::optional<OffsetLookup> InstanceStore::LookupLower(const std::string& cls,
                                                       const std::string& member) const {
    auto it = lowerTable_.find(LowerKey(cls));
    if (it == lowerTable_.end()) return std::nullopt;
    auto jt = it->second.find(LowerKey(member));
    if (jt == it->second.end()) return std::nullopt;
    return jt->second;
}

std::string InstanceStore::CacheKey(const std::string& cls, const std::string& member) {
    std::string k;
    k.reserve(cls.size() + member.size() + 1);
    k.append(cls).push_back('\x1f');
    k.append(member);
    return k;
}

std::optional<int64_t> InstanceStore::OffsetOf(const std::string& c, const std::string& m) const {
    auto it = table_.find(c);
    if (it == table_.end()) return std::nullopt;
    auto jt = it->second.find(m);
    if (jt == it->second.end()) return std::nullopt;
    return jt->second;
}
int64_t InstanceStore::OffsetOr(const std::string& c, const std::string& m, int64_t fb) const {
    auto o = OffsetOf(c, m);
    return o ? *o : fb;
}

// ------------------------------------------------------------------ move

InstanceStore::InstanceStore(InstanceStore&& o) noexcept
    : mem_(o.mem_), loaded_(o.loaded_), table_(std::move(o.table_)),
      lowerTable_(std::move(o.lowerTable_)), propCache_(std::move(o.propCache_)),
      manualDm_(o.manualDm_), cachedDm_(o.cachedDm_),
      moduleCacheName_(std::move(o.moduleCacheName_)),
      moduleCachePid_(o.moduleCachePid_),
      moduleCacheBase_(o.moduleCacheBase_), moduleCacheSize_(o.moduleCacheSize_),
      oParent_(o.oParent_), oDesc_(o.oDesc_), oCont_(o.oCont_),
      oName_(o.oName_), oClassName_(o.oClassName_),
      oChildrenStart_(o.oChildrenStart_), oWorkspace_(o.oWorkspace_),
      oLocalPlayer_(o.oLocalPlayer_), oModelInstance_(o.oModelInstance_),
      oWsCamera_(o.oWsCamera_), oCamPos_(o.oCamPos_), oCamRot_(o.oCamRot_),
      oCamFov_(o.oCamFov_), oCamViewport_(o.oCamViewport_),
      children_(std::move(o.children_)),
      instances_(std::move(o.instances_)),
      indexReady_(o.indexReady_), indexStamp_(o.indexStamp_), indexPid_(o.indexPid_),
      indexBytes_(o.indexBytes_), indexMs_(o.indexMs_),
      subClasses_(std::move(o.subClasses_)),
      nameCache_(std::move(o.nameCache_)) {
    indexBuilding_.store(false, std::memory_order_relaxed);
    indexCancel_.store(false, std::memory_order_relaxed);
    lastSweepMs_.store(o.lastSweepMs_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    lastIndexRebuildMs_.store(o.lastIndexRebuildMs_.load(std::memory_order_relaxed), std::memory_order_relaxed);
}

InstanceStore& InstanceStore::operator=(InstanceStore&& o) noexcept {
    if (this == &o) return *this;
    mem_ = o.mem_; loaded_ = o.loaded_; table_ = std::move(o.table_);
    lowerTable_ = std::move(o.lowerTable_);
    {
        std::unique_lock lk(propMutex_);
        std::unique_lock lk2(o.propMutex_);
        propCache_ = std::move(o.propCache_);
    }
    manualDm_ = o.manualDm_; cachedDm_ = o.cachedDm_;
    moduleCacheName_ = std::move(o.moduleCacheName_);
    moduleCachePid_ = o.moduleCachePid_;
    moduleCacheBase_ = o.moduleCacheBase_;
    moduleCacheSize_ = o.moduleCacheSize_;
    oParent_ = o.oParent_; oDesc_ = o.oDesc_; oCont_ = o.oCont_;
    oName_ = o.oName_; oClassName_ = o.oClassName_;
    oChildrenStart_ = o.oChildrenStart_; oWorkspace_ = o.oWorkspace_;
    oLocalPlayer_ = o.oLocalPlayer_; oModelInstance_ = o.oModelInstance_;
    oWsCamera_ = o.oWsCamera_; oCamPos_ = o.oCamPos_; oCamRot_ = o.oCamRot_;
    oCamFov_ = o.oCamFov_; oCamViewport_ = o.oCamViewport_;
    children_ = std::move(o.children_);
    instances_ = std::move(o.instances_);
    indexReady_ = o.indexReady_; indexStamp_ = o.indexStamp_; indexPid_ = o.indexPid_;
    indexBytes_ = o.indexBytes_; indexMs_ = o.indexMs_;
    subClasses_ = std::move(o.subClasses_);
    lastSweepMs_.store(o.lastSweepMs_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    lastIndexRebuildMs_.store(o.lastIndexRebuildMs_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    indexBuilding_.store(false, std::memory_order_relaxed);
    indexCancel_.store(false, std::memory_order_relaxed);
    {
        std::unique_lock mk(metaMutex_);
        std::unique_lock mk2(o.metaMutex_);
        nameCache_ = std::move(o.nameCache_);
    }
    return *this;
}

// ------------------------------------------------------------------ raw reads

uintptr_t InstanceStore::ReadPtr(uintptr_t a) {
    uintptr_t v = 0;
    if (!mem_ || !a || !mem_->CustomRead(a, &v, sizeof(v))) return 0;
    return v;
}

// Roblox string layout: [0]=ptr-or-cap, [8..0x17]=inline chars (16), [0x18]=size
std::string InstanceStore::ReadRobloxString(uintptr_t addr, size_t maxLen) const {
    if (!mem_ || !addr) return "";
    if (maxLen > 256) maxLen = 256;
    uint8_t buf[0x20] = {};
    if (!mem_->CustomRead(addr, buf, sizeof(buf))) return "";
    auto printable = [](const char* s, size_t n) {
        if (n == 0 || n > 256) return false;
        for (size_t i = 0; i < n; ++i)
            if (!isprint((unsigned char)s[i]) && s[i] != '\0') return false;
        return true;
    };
    uint64_t len = *(uint64_t*)(buf + 0x18);
    // inline: short names live in the 16-byte buffer
    if (len > 0 && len <= 16 && buf[8 + len] == '\0' && printable((char*)buf + 8, (size_t)len))
        return std::string((char*)buf + 8, (size_t)len);
    // heap: pointer in the union slot
    uintptr_t ptr = *(uintptr_t*)buf;
    if (len > 0 && len <= maxLen && IsReadablePtr(ptr)) {
        std::string s((size_t)len, '\0');
        if (mem_->CustomRead(ptr, s.data(), (size_t)len) && printable(s.data(), (size_t)len)) {
            size_t z = s.find('\0');
            if (z != std::string::npos) s.resize(z);
            return s;
        }
    }
    // last resort: NUL-terminated chars right here
    char raw[64] = {};
    if (mem_->CustomRead(addr, raw, sizeof(raw))) {
        size_t n = strnlen(raw, sizeof(raw));
        if (n > 0 && n <= maxLen && printable(raw, n)) return std::string(raw, n);
    }
    return "";
}

std::string InstanceStore::GetName(uintptr_t inst) {
    if (!inst) return "";
    {
        std::shared_lock lk(metaMutex_);
        auto it = nameCache_.find(inst);
        if (it != nameCache_.end() && !it->second.first.empty()) return it->second.first;
    }
    const int64_t nameOff = oName_;
    const int64_t containerOff = oCont_;
    std::string name;
    uintptr_t container = ReadPtr(inst + (uintptr_t)containerOff);
    if (FastReadable(container)) {
        name = ReadRobloxString(container + (uintptr_t)nameOff);
        if (name.empty()) name = ReadRobloxString(container);
    }
    if (name.empty()) name = ReadRobloxString(inst + (uintptr_t)containerOff);
    if (name.empty()) {
        uintptr_t p = ReadPtr(inst + (uintptr_t)nameOff);
        if (FastReadable(p)) name = ReadRobloxString(p);
    }
    if (!name.empty()) {
        std::unique_lock lk(metaMutex_);
        if (nameCache_.size() > 300000) nameCache_.clear();
        auto& slot = nameCache_[inst];
        if (slot.first.empty()) slot.first = name;
        else name = slot.first;
    }
    return name;
}

std::string InstanceStore::GetClass(uintptr_t inst) {
    if (!inst) return "";
    {
        std::shared_lock lk(metaMutex_);
        auto it = nameCache_.find(inst);
        if (it != nameCache_.end() && !it->second.second.empty()) return it->second.second;
    }
    const int64_t descOff = oDesc_;
    const int64_t cnOff = oClassName_;
    // A class name is a short printable ASCII token; anything else means the
    // descriptor pointer was misread, so reject it instead of returning junk.
    auto validClass = [](const std::string& s) {
        if (s.empty() || s.size() > 48) return false;
        for (char c : s)
            if (!isalnum((unsigned char)c)) return false;
        return true;
    };
    std::string cls;
    uintptr_t desc = ReadPtr(inst + (uintptr_t)descOff);
    if (FastReadable(desc)) {
        cls = ReadRobloxString(desc + (uintptr_t)cnOff);
        if (!validClass(cls)) {
            uintptr_t p = ReadPtr(desc + (uintptr_t)cnOff);
            if (FastReadable(p)) cls = ReadRobloxString(p);
        }
        if (!validClass(cls)) {
            uintptr_t p = ReadPtr(desc + (uintptr_t)cnOff);
            if (FastReadable(p)) {
                uintptr_t p2 = ReadPtr(p);
                if (FastReadable(p2)) cls = ReadRobloxString(p2);
            }
        }
        if (!validClass(cls)) cls.clear();
    }
    if (!cls.empty()) {
        std::unique_lock lk(metaMutex_);
        if (nameCache_.size() > 300000) nameCache_.clear();
        auto& slot = nameCache_[inst];
        if (slot.second.empty()) slot.second = cls;
        else cls = slot.second;
    }
    return cls;
}

uintptr_t InstanceStore::GetParent(uintptr_t inst) {
    if (!inst) return 0;
    uintptr_t p = ReadPtr(inst + (uintptr_t)oParent_);
    return FastReadable(p) ? p : 0;
}

bool InstanceStore::IsAlive(uintptr_t inst) {
    if (!FastReadable(inst)) return false;
    return ReadPtr(inst + 8) == inst;
}

bool InstanceStore::IsValidInstance(uintptr_t inst) {
    if (!FastReadable(inst)) return false;
    if (ReadPtr(inst + 8) != inst) return false;                     // This
    if (!FastReadable(ReadPtr(inst + (uintptr_t)oDesc_))) return false;
    if (!FastReadable(ReadPtr(inst + (uintptr_t)oCont_))) return false;
    return true;
}

