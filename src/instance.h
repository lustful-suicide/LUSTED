#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <shared_mutex>
#include <atomic>
#include <mutex>
#include <windows.h>
#ifdef GetClassName
#undef GetClassName
#endif

class Memory;

enum class PropType { None, F32, U32, I32, U64, Bool, Ptr, Vec3, Vec2, C3, String, RbxString, CF };

struct PropInfo {
    PropType type = PropType::None;
    int64_t offset = 0;    // offset of the field inside its owning object
    // sub-object hops: base+hop[0] holds a pointer, whose +hop[1] holds the next
    // pointer, and so on; the field is then at offset from the last pointer.
    std::vector<int64_t> hops;
    bool ok() const { return type != PropType::None; }
};

// offsets.json can spell a member differently by case from the runtime name
// (Roblox reads WalkSpeed, the dump stores "Walkspeed"). Both spellings are
// indexed once at load, so a case-only miss stays a hash hit rather than a
// full-table scan with a string allocation per entry.
struct OffsetLookup {
    std::string realClass;
    std::string realMember;
    int64_t offset = 0;
};

// Transparent hasher: allows lookup by string_view without allocating the
// "cls\x1fmember" key on every ResolveProp hit (that alloc showed up hot:
// property reads happen per frame per part).
struct TransparentStrHash {
    using is_transparent = void;
    size_t operator()(std::string_view s) const noexcept {
        // FNV-1a 64-bit: fast for short keys, no seeding overhead.
        size_t h = 14695981039346656037ull;
        for (char c : s) { h ^= (unsigned char)c; h *= 1099511628211ull; }
        return h;
    }
    size_t operator()(const std::string& s) const noexcept {
        return (*this)(std::string_view(s));
    }
};
struct TransparentStrEq {
    using is_transparent = void;
    bool operator()(std::string_view a, std::string_view b) const noexcept { return a == b; }
    bool operator()(const std::string& a, std::string_view b) const noexcept { return std::string_view(a) == b; }
    bool operator()(std::string_view a, const std::string& b) const noexcept { return a == std::string_view(b); }
    bool operator()(const std::string& a, const std::string& b) const noexcept { return a == b; }
};

// Single-pass, parallel instance index. Every tree lookup after the build is an
// in-memory hash hit instead of re-reading (and re-scanning) remote memory.
class InstanceStore {
public:
    explicit InstanceStore(Memory* mem) : mem_(mem) {}
    InstanceStore(const InstanceStore&) = delete;
    InstanceStore& operator=(const InstanceStore&) = delete;
    InstanceStore(InstanceStore&& other) noexcept;
    InstanceStore& operator=(InstanceStore&& other) noexcept;

    bool Load(const std::string& offsetsJsonPath);
    bool IsLoaded() const { return loaded_; }

    std::optional<int64_t> OffsetOf(const std::string& cls, const std::string& member) const;
    int64_t OffsetOr(const std::string& cls, const std::string& member, int64_t fallback) const;

    // strings / identity
    std::string ReadRobloxString(uintptr_t addr, size_t maxLen = 128) const;
    std::string GetName(uintptr_t inst);
    std::string GetClass(uintptr_t inst);
    uintptr_t GetParent(uintptr_t inst);

    // tree (index-backed, instant)
    std::vector<uintptr_t> GetChildren(uintptr_t inst, bool allowScan = true);
    uintptr_t FindFirstChild(uintptr_t inst, const std::string& name);
    // Snapshot + live vector only, never a heap sweep. For __index fallbacks
    // and probe loops, where a miss is the common case (player.Character is a
    // property, not a child) and a multi-second sweep per miss is fatal.
    uintptr_t FindChildCached(uintptr_t inst, const std::string& name);
    uintptr_t FindFirstChildOfClass(uintptr_t inst, const std::string& cls);
    uintptr_t DirectChildByName(uintptr_t parent, const std::string& name);
    void GetDescendants(uintptr_t inst, std::vector<uintptr_t>& out, int depthLimit = 32);
    uintptr_t WaitForChild(uintptr_t inst, const std::string& name, int timeoutMs = 5000);
    uintptr_t ResolvePath(uintptr_t root, const std::string& dotted);

    // index control
    bool BuildIndex(DWORD pid, bool force = false);
    bool IsIndexBuilding() const { return indexBuilding_.load(std::memory_order_relaxed); }
    void RequestIndexCancel() { indexCancel_.store(true, std::memory_order_relaxed); }
    // ms since the last completed pass (UINT64_MAX when no pass ever landed);
    // WaitForChild uses it so a missing child cannot retrigger a full rebuild
    // every 120 ms.
    uint64_t IndexAgeMs() const;
    void InvalidateIndex();
    bool IsIndexReady() const;
    size_t IndexInstanceCount() const;
    std::string IndexStats(DWORD pid) const;

    // properties (inheritance + sub-object chains + typed)
    PropInfo ResolveProp(uintptr_t inst, const std::string& cls, const std::string& member) const;
    uintptr_t PropBase(const PropInfo& p, uintptr_t inst) const;
    bool HasProp(const std::string& cls, const std::string& member) const;
    bool ReadPropFloat(const PropInfo& p, uintptr_t inst, float& out) const;
    bool WritePropFloat(const PropInfo& p, uintptr_t inst, float value) const;
    // Replication-safe mirrored float write (UCRobloxExternal dual-write fix):
    // Humanoid Walkspeed <-> WalkspeedCheck and JumpPower <-> JumpHeight stay
    // in sync or the server kicks. Case-insensitive, best-effort on mirrors.
    bool WritePropFloatMirrored(uintptr_t inst, const std::string& cls, const std::string& member, float value) const;
    std::optional<int64_t> OffsetOfInsensitive(const std::string& cls, const std::string& member) const;
    bool ReadPropVec3(const PropInfo& p, uintptr_t inst, float out[3]) const;
    bool WritePropVec3(const PropInfo& p, uintptr_t inst, const float v[3]) const;
    // CFrame = Primitive.Rotation (9 floats) immediately followed by
    // Primitive.Position (3 floats); resolved as one contiguous 48-byte block.
    bool ReadPropCFrame(const PropInfo& p, uintptr_t inst, float out[12]) const;
    bool WritePropCFrame(const PropInfo& p, uintptr_t inst, const float v[12]) const;
    std::string ReadPropString(const PropInfo& p, uintptr_t inst) const;

    // DataModel auto-resolve (cheap validation) + manual override
    uintptr_t GetDataModel(DWORD pid);
    void SetDataModel(uintptr_t dm) { manualDm_ = dm; }
    bool IsValidDataModel(uintptr_t dm);
    uintptr_t GetWorkspace(DWORD pid);
    uintptr_t GetService(DWORD pid, const std::string& name);
    std::vector<uintptr_t> GetPlayers(DWORD pid);
    uintptr_t GetLocalPlayer(DWORD pid);
    uintptr_t GetCharacter(DWORD pid);
    uintptr_t GetHumanoid(DWORD pid);
    uintptr_t GetRootPart(DWORD pid);

    uintptr_t ModuleBase(DWORD pid, const std::string& modName);
    bool ModuleInfo(DWORD pid, const std::string& modName, uintptr_t& base, size_t& size);

private:
    Memory* mem_ = nullptr;
    friend struct PropInfo;
    bool loaded_ = false;
    std::unordered_map<std::string, std::unordered_map<std::string, int64_t>> table_;
    // lower(class) -> lower(member) -> spelling + offset
    struct LowerMembers : std::unordered_map<std::string, OffsetLookup> {};
    std::unordered_map<std::string, LowerMembers> lowerTable_;
    // ResolveProp is a pure function of table_, which never changes after Load,
    // so results are memoized. Shared across threads (property reads run from
    // the UI, the Luau thread and the index worker). Transparent lookup avoids
    // allocating the composite key on hits.
    mutable std::shared_mutex propMutex_;
    mutable std::unordered_map<std::string, PropInfo, TransparentStrHash, TransparentStrEq> propCache_;
    uintptr_t manualDm_ = 0;
    uintptr_t cachedDm_ = 0;

    // ---- hot offsets, snapshotted at Load() so per-instance calls avoid
    // two string hashes + temporary std::string construction each time.
    // GetName/GetClass/GetParent/GetChildren run per child per lookup.
    int64_t oParent_ = 104;
    int64_t oDesc_ = 24;
    int64_t oCont_ = 112;
    int64_t oName_ = 8;
    int64_t oClassName_ = 8;
    int64_t oChildrenStart_ = 120;
    int64_t oWorkspace_ = 336;
    int64_t oLocalPlayer_ = 288;
    int64_t oModelInstance_ = 648;
    void RefreshCachedOffsets();
    static std::string CacheKey(const std::string& cls, const std::string& member);

    // ---- index ----
    mutable std::shared_mutex indexMutex_;
    std::atomic<bool> indexBuilding_{ false };
    std::atomic<bool> indexCancel_{ false };
    // Last wall-clock ms a fallback heap sweep started. Misses share one
    // result per window instead of each paying seconds for its own pass.
    mutable std::atomic<uint64_t> lastSweepMs_{ 0 };
    // Rebuilds are expensive; a single stale lookup should not launch a new
    // full sweep every loop while the index is aging out.
    mutable std::atomic<uint64_t> lastIndexRebuildMs_{ 0 };
    std::unordered_map<uintptr_t, std::vector<uintptr_t>> children_;
    std::unordered_set<uintptr_t> instances_;
    bool indexReady_ = false;
    DWORD indexPid_ = 0;
    uint64_t indexStamp_ = 0;
    size_t indexBytes_ = 0;
    double indexMs_ = 0;
    // Sub-object classes computed once at Load (was recomputed + copied per
    // ResolveProp miss via a static keyed only on table size).
    std::vector<std::string> subClasses_;

    // ---- lazy name/class cache ----
    mutable std::shared_mutex metaMutex_;
    std::unordered_map<uintptr_t, std::pair<std::string, std::string>> nameCache_;

    // ---- property helpers ----
    std::vector<std::string> ClassChain(const std::string& cls) const;
    const std::vector<std::string>& SubObjectClasses() const { return subClasses_; }
    // uncached body of ResolveProp; table_ is immutable after Load()
    PropInfo ResolvePropRaw(const std::string& cls, const std::string& member) const;
    std::optional<OffsetLookup> LookupLower(const std::string& cls, const std::string& member) const;

    uintptr_t ReadPtr(uintptr_t addr);
    static __forceinline bool FastReadable(uintptr_t p) noexcept {
        return p > 0x10000 && p < 0x0000800000000000ULL;
    }
    bool IsReadablePtr(uintptr_t p) const { return FastReadable(p); }
    bool IsValidInstance(uintptr_t inst);
    bool IsValidDataModelCheap(uintptr_t dm);
    std::vector<uintptr_t> ScanChildrenByParent(uintptr_t parent);
    void LogSlowLookup(uintptr_t inst, uint64_t t0, const char* won) const;
    std::vector<std::pair<uintptr_t, uintptr_t>> ScanHeapRegion(HANDLE h, uintptr_t base, size_t size,
        int64_t parentOff, int64_t descOff, int64_t contOff);
    uintptr_t TryFakePointer(DWORD pid, uintptr_t base);
    uintptr_t TryVisualEngine(DWORD pid, uintptr_t base);
    uintptr_t TryTaskScheduler(DWORD pid, uintptr_t base);
};
