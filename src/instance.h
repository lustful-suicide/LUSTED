#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <shared_mutex>
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
    uintptr_t FindFirstChildOfClass(uintptr_t inst, const std::string& cls);
    uintptr_t DirectChildByName(uintptr_t parent, const std::string& name);
    void GetDescendants(uintptr_t inst, std::vector<uintptr_t>& out, int depthLimit = 32);
    uintptr_t WaitForChild(uintptr_t inst, const std::string& name, int timeoutMs = 5000);
    uintptr_t ResolvePath(uintptr_t root, const std::string& dotted);

    // index control
    bool BuildIndex(DWORD pid, bool force = false);
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
    uintptr_t manualDm_ = 0;
    uintptr_t cachedDm_ = 0;

    // ---- index ----
    mutable std::shared_mutex indexMutex_;
    std::unordered_map<uintptr_t, std::vector<uintptr_t>> children_;
    std::unordered_map<uintptr_t, uint64_t> leafStamps_; // confirmed childless nodes
    std::unordered_set<uintptr_t> instances_;
    bool indexReady_ = false;
    DWORD indexPid_ = 0;
    uint64_t lastServiceRefresh_ = 0;
    uint64_t lastPlayersRefresh_ = 0;
    uint64_t indexStamp_ = 0;
    size_t indexBytes_ = 0;
    double indexMs_ = 0;

    // ---- lazy name/class cache ----
    mutable std::shared_mutex metaMutex_;
    std::unordered_map<uintptr_t, std::pair<std::string, std::string>> nameCache_;

    // ---- property helpers ----
    std::vector<std::string> ClassChain(const std::string& cls) const;
    std::vector<std::string> SubObjectClasses() const;

    uintptr_t ReadPtr(uintptr_t addr);
    bool IsReadablePtr(uintptr_t p) const;
    bool IsValidInstance(uintptr_t inst);
    bool IsValidDataModelCheap(uintptr_t dm);
    std::vector<uintptr_t> ScanChildrenByParent(uintptr_t parent);
    std::vector<std::pair<uintptr_t, uintptr_t>> ScanHeapRegion(HANDLE h, uintptr_t base, size_t size);
    uintptr_t TryFakePointer(DWORD pid, uintptr_t base);
    uintptr_t TryVisualEngine(DWORD pid, uintptr_t base);
    uintptr_t TryTaskScheduler(DWORD pid, uintptr_t base);
};