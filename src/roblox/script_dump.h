#pragma once
// Script discovery + decompile pipeline.
//
// Today: enumeration works (name/class/parent/address over the live tree),
// and the decompile transport is proven end-to-end against LunaUX with
// locally-compiled chunks. What is NOT here: a memory reader for script
// bytecode/source. Ten live probe rounds established the offsets DB carries
// no script source/bytecode fields (ByteCode=0 unknown; +0 is the vtable;
// CachedRemoteSource is a shared asset table, not source; ScriptContext
// holds no raw script pointers in 16KB). When the layout resolves, it lands
// in GetScriptBytecode and everything downstream lights up unchanged.
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>
class InstanceStore;

struct ScriptInfo {
    uintptr_t addr = 0;
    std::string name;
    std::string cls; // LocalScript or ModuleScript
    std::string parentName;
};

// All LocalScript/ModuleScript instances reachable from the DataModel.
bool ListClientScripts(InstanceStore& inst, DWORD pid, std::vector<ScriptInfo>& out);

// Live-reader stub: returns false until the bytecode layout resolves.
// (Kept as the seam so bindings/MCP/tools don't change when it lands.)
bool GetScriptBytecode(InstanceStore& inst, uintptr_t script, std::vector<uint8_t>& chunk);

// LunaUX round trip: base64 chunk -> Luau source. Empty string on failure.
std::string DecompileBytecode(const std::vector<uint8_t>& chunk, const std::string& filename);
