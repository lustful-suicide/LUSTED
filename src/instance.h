#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <optional>
#include <unordered_map>
#include <windows.h>
#ifdef GetClassName
#undef GetClassName
#endif

class Memory;

// Resolves everything through offsets.json so Luau features never
// hardcode numbers. Handles the rbxoffsets quirks:
//  - Instance.ChildrenEnd is "8" in some dumps (bogus) -> fallback to Start+8
//  - Instance.Name/ClassName are offsets *inside* NameContainer/ClassDescriptor
class InstanceStore {
public:
    explicit InstanceStore(Memory* mem) : mem_(mem) {}

    bool Load(const std::string& offsetsJsonPath);
    bool IsLoaded() const { return loaded_; }

    std::optional<int64_t> OffsetOf(const std::string& cls, const std::string& member);
    int64_t OffsetOr(const std::string& cls, const std::string& member, int64_t fallback);

    // low-level string: tries std::string ptr/len, SSO, and raw cstr
    std::string ReadRobloxString(uintptr_t addr, size_t maxLen = 128);
    std::string GetName(uintptr_t inst);
    std::string GetClass(uintptr_t inst);
    uintptr_t GetParent(uintptr_t inst);
    std::vector<uintptr_t> GetChildren(uintptr_t inst);
    uintptr_t FindFirstChild(uintptr_t inst, const std::string& name);
    uintptr_t FindFirstChildOfClass(uintptr_t inst, const std::string& cls);
    void GetDescendants(uintptr_t inst, std::vector<uintptr_t>& out, int depthLimit = 32);
    uintptr_t WaitForChild(uintptr_t inst, const std::string& name, int timeoutMs = 5000);
    uintptr_t ResolvePath(uintptr_t root, const std::string& dotted);

    // DataModel auto-resolve (no manual address needed) + manual override
    uintptr_t GetDataModel(DWORD pid);
    void SetDataModel(uintptr_t dm) { manualDm_ = dm; }
    bool IsValidDataModel(uintptr_t dm);
    uintptr_t GetWorkspace(DWORD pid);
    uintptr_t GetService(DWORD pid, const std::string& name);
    uintptr_t GetLocalPlayer(DWORD pid);
    uintptr_t GetCharacter(DWORD pid);
    uintptr_t GetHumanoid(DWORD pid);

    uintptr_t ModuleBase(DWORD pid, const std::string& modName);
    bool ModuleInfo(DWORD pid, const std::string& modName, uintptr_t& base, size_t& size);

private:
    Memory* mem_ = nullptr;
    bool loaded_ = false;
    std::unordered_map<std::string, std::unordered_map<std::string, int64_t>> table_;
    uintptr_t manualDm_ = 0;
    uintptr_t cachedDm_ = 0;
    std::unordered_map<uintptr_t, std::pair<uint64_t, std::vector<uintptr_t>>> childCache_;

    uintptr_t ReadPtr(uintptr_t addr);
    bool IsReadablePtr(uintptr_t p);
    bool IsValidInstance(uintptr_t inst);
    std::vector<uintptr_t> ScanChildrenByParent(uintptr_t parent);
    uintptr_t TryFakePointer(DWORD pid, uintptr_t base);
    uintptr_t TryVisualEngine(DWORD pid, uintptr_t base);
    uintptr_t TryTaskScheduler(DWORD pid, uintptr_t base);
};
