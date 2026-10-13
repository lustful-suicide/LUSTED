#include "mcp/mcp_server.h"

#include "platform/windows/memory.h"
#include "roblox/instance.h"
#include "roblox/script_dump.h"
#include "roblox/roblox_finder.h"
#include "roblox/offsets_fetcher.h"
#include "runtime/luau_manager.h"

#include "lua.h"
#include "lualib.h"
#include "Luau/Compiler.h"

#include <nlohmann/json.hpp>

#include <iostream>
#include <string>
#include <vector>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <optional>
#include <algorithm>

using json = nlohmann::json;

namespace {

// ---------------------------------------------------------------- helpers

struct RpcError {
    int code = -32602;
    std::string message;
    RpcError(int c, std::string m) : code(c), message(std::move(m)) {}
};

struct NullBuf : std::streambuf {
    int overflow(int c) override { return c; }
};

// InstanceStore helpers print diagnostics to std::cout ([datamodel], index
// progress). On this transport stdout is the JSON-RPC channel, so any C++
// that can touch those paths runs muted; responses are written unmuted.
struct CoutMute {
    std::streambuf* old_;
    CoutMute() : old_(std::cout.rdbuf()) {
        static NullBuf nb;
        std::cout.rdbuf(&nb);
    }
    ~CoutMute() { std::cout.rdbuf(old_); }
};

static std::string FormatAddr(uintptr_t a) {
    char b[32];
    std::snprintf(b, sizeof(b), "0x%llX", (unsigned long long)a);
    return std::string(b);
}

static bool ParseAddrValue(const json& v, uintptr_t& out) {
    try {
        if (v.is_string()) {
            std::string s = v.get<std::string>();
            size_t i = 0;
            while (i < s.size() && std::isspace((unsigned char)s[i])) ++i;
            if (i >= s.size()) return false;
            const char* beg = s.c_str() + i;
            char* end = nullptr;
            unsigned long long x = std::strtoull(beg, &end, 0);
            if (end == beg) return false;
            out = (uintptr_t)x;
            return true;
        }
        if (v.is_number_unsigned()) {
            out = (uintptr_t)v.get<unsigned long long>();
            return true;
        }
        if (v.is_number_integer()) {
            long long x = v.get<long long>();
            if (x < 0) return false;
            out = (uintptr_t)(unsigned long long)x;
            return true;
        }
        if (v.is_number_float()) {
            double d = v.get<double>();
            if (d < 0) return false;
            out = (uintptr_t)(unsigned long long)d;
            return true;
        }
    } catch (...) {
        return false;
    }
    return false;
}

static bool GetAddrArg(const json& args, const char* key, uintptr_t& out) {
    auto it = args.find(key);
    if (it == args.end() || it->is_null()) return false;
    return ParseAddrValue(*it, out);
}

static uintptr_t NeedAddr(const json& args, const char* key) {
    uintptr_t a = 0;
    if (!GetAddrArg(args, key, a) || a == 0)
        throw RpcError(-32602, std::string("invalid params: '") + key +
                                  "' must be a non-zero address (\"0x...\" hex string)");
    return a;
}

static std::string NeedStr(const json& args, const char* key) {
    auto it = args.find(key);
    if (it == args.end() || !it->is_string() || it->get<std::string>().empty())
        throw RpcError(-32602, std::string("invalid params: '") + key +
                                  "' must be a non-empty string");
    return it->get<std::string>();
}

static size_t NeedLength(const json& args) {
    auto it = args.find("length");
    if (it == args.end() || it->is_null())
        throw RpcError(-32602, "invalid params: 'length' is required");
    long long v = -1;
    try {
        if (it->is_number_unsigned()) v = (long long)it->get<unsigned long long>();
        else if (it->is_number_integer()) v = it->get<long long>();
        else if (it->is_number_float()) v = (long long)it->get<double>();
    } catch (...) {
        v = -1;
    }
    if (v <= 0 || v > 65536)
        throw RpcError(-32602, "invalid params: 'length' must be 1..65536");
    return (size_t)v;
}

static bool ParsePidValue(const json& v, DWORD& out) {
    unsigned long long x = 0;
    try {
        if (v.is_string()) {
            std::string s = v.get<std::string>();
            size_t i = 0;
            while (i < s.size() && std::isspace((unsigned char)s[i])) ++i;
            if (i >= s.size()) return false;
            char* end = nullptr;
            x = std::strtoull(s.c_str() + i, &end, 0);
            if (end == s.c_str() + i) return false;
        } else if (v.is_number_unsigned()) {
            x = v.get<unsigned long long>();
        } else if (v.is_number_integer()) {
            long long n = v.get<long long>();
            if (n <= 0) return false;
            x = (unsigned long long)n;
        } else if (v.is_number_float()) {
            double d = v.get<double>();
            if (d <= 0) return false;
            x = (unsigned long long)d;
        } else {
            return false;
        }
    } catch (...) {
        return false;
    }
    if (x == 0 || x > 0xFFFFFFFFull) return false;
    out = (DWORD)x;
    return true;
}

// ---------------------------------------------------------------- base64

static const char kB64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string B64Encode(const uint8_t* d, size_t n) {
    std::string o;
    o.reserve(((n + 2) / 3) * 4);
    for (size_t i = 0; i < n; i += 3) {
        uint32_t a = d[i];
        uint32_t b = (i + 1 < n) ? d[i + 1] : 0;
        uint32_t c = (i + 2 < n) ? d[i + 2] : 0;
        uint32_t t = (a << 16) | (b << 8) | c;
        o.push_back(kB64[(t >> 18) & 63]);
        o.push_back(kB64[(t >> 12) & 63]);
        o.push_back(i + 1 < n ? kB64[(t >> 6) & 63] : '=');
        o.push_back(i + 2 < n ? kB64[t & 63] : '=');
    }
    return o;
}

static std::string B64EncodeStr(const std::string& s) {
    return B64Encode(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

static bool B64Decode(const std::string& in, std::vector<uint8_t>& out, std::string& err) {
    static int T[256];
    static bool init = false;
    if (!init) {
        for (int i = 0; i < 256; ++i) T[i] = -1;
        for (int i = 0; kB64[i]; ++i) T[(unsigned char)kB64[i]] = i;
        init = true;
    }
    out.clear();
    int val = 0, valb = -8;
    bool seenPad = false;
    for (size_t i = 0; i < in.size(); ++i) {
        char c = in[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        if (c == '=') {
            seenPad = true;
            continue;
        }
        if (seenPad) {
            err = "invalid base64";
            return false;
        }
        int d = T[(unsigned char)c];
        if (d < 0) {
            err = "invalid base64";
            return false;
        }
        val = (val << 6) | d;
        valb += 6;
        if (valb >= 0) {
            out.push_back((uint8_t)((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return true;
}

// ---------------------------------------------------------------- session

struct McpSession {
    Memory mem;
    InstanceStore inst;
    DWORD pid = 0;
    std::string versionHash;
    std::string offsetsPath;
    bool offsetsLoaded = false;
    bool luauReady = false;
    DWORD indexPid = 0;
    bool indexDone = false;
    LuauManager luau;

    McpSession() : inst(&mem), luau(&mem, &inst, &pid, nullptr, nullptr, nullptr) {}
    McpSession(const McpSession&) = delete;
    McpSession& operator=(const McpSession&) = delete;
};

static bool SessAttached(const McpSession& s) {
    return s.pid != 0 && s.mem.IsOpen();
}

static bool SessOffsetsReady(const McpSession& s) {
    return s.offsetsLoaded && s.inst.IsLoaded();
}

// Lazy index: build once per pid, rebuild when the pid changes.
static bool SessEnsureIndex(McpSession& s) {
    if (s.pid == 0 || !s.mem.IsOpen() || !s.inst.IsLoaded()) return false;
    if (s.indexDone && s.indexPid == s.pid && s.inst.IsIndexReady()) return true;
    s.inst.BuildIndex(s.pid);
    if (s.inst.IsIndexReady()) {
        s.indexDone = true;
        s.indexPid = s.pid;
        return true;
    }
    return false;
}

static bool SessEnsureLuau(McpSession& s, std::string& err) {
    if (s.luauReady && s.luau.State()) return true;
    if (!SessAttached(s)) {
        err = "not attached (call attach first)";
        return false;
    }
    if (!SessOffsetsReady(s)) {
        err = "offsets not loaded (call attach first)";
        return false;
    }
    if (s.versionHash.empty() || s.offsetsPath.empty()) {
        err = "version/offsets unknown (call attach first)";
        return false;
    }
    if (!s.luau.Init(s.versionHash, s.offsetsPath)) {
        err = "luau init failed";
        return false;
    }
    s.luauReady = true;
    return true;
}

// ---------------------------------------------------------------- run_luau

static std::string* s_mcpPrintCap = nullptr;

static int McpPrintThunk(lua_State* L) {
    int n = lua_gettop(L);
    std::string line;
    for (int i = 1; i <= n; ++i) {
        if (i > 1) line.push_back('\t');
        if (lua_isnoneornil(L, i)) {
            line += "nil";
        } else if (lua_isboolean(L, i)) {
            line += (lua_toboolean(L, i) ? "true" : "false");
        } else {
            const char* s = lua_tostring(L, i);
            if (s) {
                line += s;
            } else if (lua_istable(L, i)) {
                line += "table";
            } else if (lua_isfunction(L, i)) {
                line += "function";
            } else {
                line += "?";
            }
        }
    }
    line.push_back('\n');
    if (s_mcpPrintCap) s_mcpPrintCap->append(line);
    return 0;
}

static json DoRunLuau(McpSession& s, const std::string& code, bool& okOut) {
    std::string initErr;
    if (!SessEnsureLuau(s, initErr)) {
        okOut = false;
        return json{{"ok", false}, {"error", initErr}, {"output", ""}};
    }
    lua_State* L = s.luau.State();
    if (!L) {
        okOut = false;
        return json{{"ok", false}, {"error", "luau state missing"}, {"output", ""}};
    }
    std::string cap;
    s_mcpPrintCap = &cap;
    lua_getglobal(L, "print");
    lua_pushcfunction(L, &McpPrintThunk, "mcp_print");
    lua_setglobal(L, "print");
    bool ok = true;
    std::string err;
    try {
        Luau::CompileOptions co;
        std::string bc = Luau::compile(code, co);
        if (bc.empty()) {
            err = "compile error";
            ok = false;
        } else if (luau_load(L, "@mcp", bc.data(), bc.size(), 0) != 0) {
            const char* m = lua_tostring(L, -1);
            err = m ? m : "compile error";
            lua_pop(L, 1);
            ok = false;
        } else if (lua_pcall(L, 0, 0, 0) != 0) {
            const char* m = lua_tostring(L, -1);
            err = m ? m : "runtime error";
            lua_pop(L, 1);
            ok = false;
        }
    } catch (const std::exception& e) {
        err = e.what();
        ok = false;
    } catch (...) {
        err = "compile exception";
        ok = false;
    }
    lua_setglobal(L, "print"); // pop saved global, restoring the old print
    s_mcpPrintCap = nullptr;
    std::string output = cap;
    if (!ok && !err.empty()) {
        output += err;
        if (!output.empty() && output.back() != '\n') output.push_back('\n');
    }
    okOut = ok;
    return json{{"ok", ok}, {"output", output}};
}

// ---------------------------------------------------------------- tool I/O

static json ToolOk(const json& payload) {
    json r;
    r["content"] = json::array();
    r["content"].push_back(json{{"type", "text"}, {"text", payload.dump()}});
    return r;
}

static json ToolErr(const json& payload) {
    json r = ToolOk(payload);
    r["isError"] = true;
    return r;
}

static json NodeInfo(McpSession& s, uintptr_t a) {
    return json{{"address", FormatAddr(a)},
                {"name", s.inst.GetName(a)},
                {"class", s.inst.GetClass(a)}};
}

static json DoAttach(McpSession& s, const json& args) {
    bool havePid = false;
    DWORD newPid = 0;
    auto it = args.find("pid");
    if (it != args.end() && !it->is_null()) {
        if (!ParsePidValue(*it, newPid))
            throw RpcError(-32602, "invalid params: 'pid' must be a process id (number or \"0x...\" hex)");
        havePid = true;
    }

    std::string vh;
    std::optional<RobloxInstall> found;
    try {
        found = FindRobloxPlayer();
    } catch (...) {
        found.reset();
    }

    if (havePid) {
        if (found && !found->versionHash.empty()) vh = found->versionHash;
        else vh = s.versionHash;
    } else {
        if (!found || !found->pid) {
            json p = json{{"ok", false}, {"error", "Roblox player not running or not found"}};
            return ToolErr(p);
        }
        newPid = found->pid;
        vh = found->versionHash;
    }
    if (newPid == 0) {
        json p = json{{"ok", false}, {"error", "no pid to attach to"}};
        return ToolErr(p);
    }

    bool pidChanged = (newPid != s.pid);
    if (!s.mem.Attach(newPid)) {
        s.pid = newPid;
        json p = json{{"ok", false}, {"error", "Attach(pid) failed"}, {"pid", newPid}};
        return ToolErr(p);
    }
    s.pid = newPid;
    if (!vh.empty()) s.versionHash = vh;
    if (s.versionHash.empty()) {
        json p = json{{"ok", true},
                      {"pid", s.pid},
                      {"attached", true},
                      {"offsets_loaded", false},
                      {"warning", "version hash unknown; offsets not loaded"}};
        return ToolOk(p);
    }

    OffsetsFetcher fetcher(s.versionHash);
    OffsetBundle bundle = fetcher.FetchAll();
    std::string path = bundle.offsetsJson.string();
    if (!s.inst.Load(path)) {
        json p = json{{"ok", false},
                      {"error", "offsets load failed"},
                      {"pid", s.pid},
                      {"versionHash", s.versionHash},
                      {"offsets_path", path}};
        return ToolErr(p);
    }
    s.offsetsPath = path;
    s.offsetsLoaded = true;
    if (pidChanged) {
        s.indexDone = false;
        s.indexPid = 0;
    }
    if (s.luauReady) {
        s.luau.Close();
        s.luauReady = false;
    }
    json p = json{{"ok", true},
                  {"pid", s.pid},
                  {"attached", true},
                  {"versionHash", s.versionHash},
                  {"offsets_loaded", true},
                  {"offsets_path", s.offsetsPath}};
    return ToolOk(p);
}

static json DoStatus(McpSession& s) {
    json p;
    p["attached"] = SessAttached(s);
    p["pid"] = s.pid;
    p["versionHash"] = s.versionHash;
    p["offsets_loaded"] = SessOffsetsReady(s);
    p["offsets_path"] = s.offsetsPath;
    p["index_ready"] = s.inst.IsIndexReady();
    p["index_instances"] = (unsigned long long)s.inst.IndexInstanceCount();
    if (SessAttached(s) && SessOffsetsReady(s)) {
        uintptr_t dm = s.inst.GetDataModel(s.pid);
        p["datamodel"] = dm ? json(FormatAddr(dm)) : json(nullptr);
    } else {
        p["datamodel"] = json(nullptr);
    }
    return ToolOk(p);
}

} // namespace

// ---------------------------------------------------------------- tools/list

static json BuildToolsList() {
    json t = json::array();
    auto add = [&](const char* name, const char* desc, const json& schema) {
        t.push_back(json{{"name", name}, {"description", desc}, {"inputSchema", schema}});
    };
    json empty = json{{"type", "object"}, {"properties", json::object()}};
    add("status", "Attach/index/offsets session state (no memory writes).", empty);
    add("attach", "Attach to Roblox: find player pid (or use pid), mem.Attach, load offsets. Optional pid (number or hex string).",
        json{{"type", "object"},
             {"properties",
              json{{"pid",
                    json{{"type", json::array({"integer", "string"})},
                         {"description", "Target pid; omitted = auto-find RobloxPlayerBeta"}}}}}});
    add("index_stats", "Index readiness, instance count and raw stats string.", empty);
    add("refresh_index", "Rebuild the instance index. Optional force/deep booleans.",
        json{{"type", "object"},
             {"properties",
              json{{"force", json{{"type", "boolean"}}},
                   {"deep", json{{"type", "boolean"}}}}}});
    add("get_datamodel", "Resolve the DataModel pointer for the attached pid.", empty);
    add("get_service", "Resolve a child service of DataModel by name.",
        json{{"type", "object"},
             {"properties", json{{"name", json{{"type", "string"}}}}},
             {"required", json::array({"name"})}});
    add("get_players", "List Player instances (address, name, class).", empty);
    add("get_children", "Children of an instance (address, name, class each).",
        json{{"type", "object"},
             {"properties", json{{"addr", json{{"type", "string"}, {"description", "Instance address as 0x... hex"}}}}},
             {"required", json::array({"addr"})}});
    add("get_name", "Instance Name property.",
        json{{"type", "object"},
             {"properties", json{{"addr", json{{"type", "string"}}}}},
             {"required", json::array({"addr"})}});
    add("get_class", "Instance ClassName.",
        json{{"type", "object"},
             {"properties", json{{"addr", json{{"type", "string"}}}}},
             {"required", json::array({"addr"})}});
    add("get_parent", "Parent instance address (null when none).",
        json{{"type", "object"},
             {"properties", json{{"addr", json{{"type", "string"}}}}},
             {"required", json::array({"addr"})}});
    add("find_child", "FindFirstChild by name under an instance.",
        json{{"type", "object"},
             {"properties",
              json{{"addr", json{{"type", "string"}}}, {"name", json{{"type", "string"}}}}},
             {"required", json::array({"addr", "name"})}});
    add("resolve_path", "Resolve a dotted child path (e.g. Workspace.Part) from a root instance.",
        json{{"type", "object"},
             {"properties",
              json{{"root", json{{"type", "string"}}}, {"dotted", json{{"type", "string"}}}}},
             {"required", json::array({"root", "dotted"})}});
    add("read_bytes", "Raw memory read; returns base64. length 1..65536.",
        json{{"type", "object"},
             {"properties",
              json{{"addr", json{{"type", "string"}}},
                   {"length", json{{"type", "integer"}, {"minimum", 1}, {"maximum", 65536}}}}},
             {"required", json::array({"addr", "length"})}});
    add("write_bytes", "Raw memory write from base64 (max 65536 decoded bytes).",
        json{{"type", "object"},
             {"properties",
              json{{"addr", json{{"type", "string"}}},
                   {"data", json{{"type", "string"}, {"description", "base64 payload"}}}}},
             {"required", json::array({"addr", "data"})}});
    add("run_luau", "Run Luau in the lusted stdlib state; print() output is captured. Returns {ok, output}.",
        json{{"type", "object"},
             {"properties", json{{"code", json{{"type", "string"}}}}},
             {"required", json::array({"code"})}});
    add("get_camera", "Camera instance, position, rotation, fov and viewport.", empty);
    add("get_module_base", "Base address and size of a module loaded in the target.",
        json{{"type", "object"},
             {"properties", json{{"name", json{{"type", "string"}}}}},
             {"required", json::array({"name"})}});
    add("get_bytecode", "Script chunk bytes as base64. Fails honestly until the bytecode layout resolves for this build (no layouts guessed).",
        json{{"type", "object"},
             {"properties", json{{"addr", json{{"type", "string"}}}}},
             {"required", json::array({"addr"})}});
    add("decompile", "LunaUX decompile of a base64 bytecode chunk. Optional filename saves a copy.",
        json{{"type", "object"},
             {"properties",
              json{{"bytecode", json{{"type", "string"}}},
                   {"filename", json{{"type", "string"}}}}},
             {"required", json::array({"bytecode"})}});
    add("list_scripts", "All LocalScript/ModuleScript instances: address, name, class, parent.",
        empty);
    add("decompile_selftest", "Proves the decompile integration: compiles a snippet locally and decompiles it.",
        empty);
    return t;
}

// ---------------------------------------------------------------- dispatch

static json DispatchTool(McpSession& s, const std::string& name, const json& args) {
    if (name == "status") {
        CoutMute mute;
        return DoStatus(s);
    }
    if (name == "attach") {
        CoutMute mute;
        return DoAttach(s, args);
    }
    if (name == "index_stats") {
        CoutMute mute;
        json p;
        p["pid"] = s.pid;
        p["ready"] = s.inst.IsIndexReady();
        p["instances"] = (unsigned long long)s.inst.IndexInstanceCount();
        p["stats"] = s.inst.IndexStats(s.pid);
        return ToolOk(p);
    }
    if (name == "refresh_index") {
        if (!SessAttached(s))
            return ToolErr(json{{"ok", false}, {"error", "not attached (call attach first)"}});
        if (!SessOffsetsReady(s))
            return ToolErr(json{{"ok", false}, {"error", "offsets not loaded (call attach first)"}});
        bool force = false, deep = false;
        auto it = args.find("force");
        if (it != args.end() && it->is_boolean()) force = it->get<bool>();
        it = args.find("deep");
        if (it != args.end() && it->is_boolean()) deep = it->get<bool>();
        bool ok = false;
        std::string stats;
        {
            CoutMute mute;
            ok = s.inst.BuildIndex(s.pid, force, deep);
            if (s.inst.IsIndexReady()) {
                s.indexDone = true;
                s.indexPid = s.pid;
            }
            stats = s.inst.IndexStats(s.pid);
        }
        json p = json{{"ok", ok},
                      {"ready", s.inst.IsIndexReady()},
                      {"instances", (unsigned long long)s.inst.IndexInstanceCount()},
                      {"stats", stats}};
        return ok ? ToolOk(p) : ToolErr(p);
    }
    if (name == "get_datamodel") {
        if (!SessAttached(s))
            return ToolErr(json{{"error", "not attached (call attach first)"}});
        CoutMute mute;
        uintptr_t dm = s.inst.GetDataModel(s.pid);
        if (!dm) return ToolErr(json{{"error", "datamodel not resolved"}});
        return ToolOk(NodeInfo(s, dm));
    }
    if (name == "get_service") {
        std::string svc = NeedStr(args, "name");
        if (!SessAttached(s))
            return ToolErr(json{{"error", "not attached (call attach first)"}});
        CoutMute mute;
        SessEnsureIndex(s);
        uintptr_t a = s.inst.GetService(s.pid, svc);
        if (!a) return ToolErr(json{{"error", "service not found"}, {"name", svc}});
        return ToolOk(NodeInfo(s, a));
    }
    if (name == "get_players") {
        if (!SessAttached(s))
            return ToolErr(json{{"error", "not attached (call attach first)"}});
        CoutMute mute;
        SessEnsureIndex(s);
        std::vector<uintptr_t> v = s.inst.GetPlayers(s.pid);
        json arr = json::array();
        for (uintptr_t a : v) arr.push_back(NodeInfo(s, a));
        return ToolOk(json{{"count", v.size()}, {"players", arr}});
    }
    if (name == "get_children") {
        uintptr_t a = NeedAddr(args, "addr");
        if (!SessAttached(s))
            return ToolErr(json{{"error", "not attached (call attach first)"}});
        CoutMute mute;
        SessEnsureIndex(s);
        std::vector<uintptr_t> v = s.inst.GetChildren(a);
        const size_t cap = 2000;
        bool truncated = v.size() > cap;
        json arr = json::array();
        for (size_t i = 0; i < v.size() && i < cap; ++i) arr.push_back(NodeInfo(s, v[i]));
        return ToolOk(
            json{{"address", FormatAddr(a)}, {"count", v.size()}, {"children", arr}, {"truncated", truncated}});
    }
    if (name == "get_name") {
        uintptr_t a = NeedAddr(args, "addr");
        if (!SessAttached(s))
            return ToolErr(json{{"error", "not attached (call attach first)"}});
        CoutMute mute;
        return ToolOk(json{{"address", FormatAddr(a)}, {"name", s.inst.GetName(a)}});
    }
    if (name == "get_class") {
        uintptr_t a = NeedAddr(args, "addr");
        if (!SessAttached(s))
            return ToolErr(json{{"error", "not attached (call attach first)"}});
        CoutMute mute;
        return ToolOk(json{{"address", FormatAddr(a)}, {"class", s.inst.GetClass(a)}});
    }
    if (name == "get_parent") {
        uintptr_t a = NeedAddr(args, "addr");
        if (!SessAttached(s))
            return ToolErr(json{{"error", "not attached (call attach first)"}});
        CoutMute mute;
        uintptr_t p = s.inst.GetParent(a);
        return ToolOk(json{{"address", FormatAddr(a)},
                           {"parent", p ? json(FormatAddr(p)) : json(nullptr)}});
    }
    if (name == "find_child") {
        uintptr_t a = NeedAddr(args, "addr");
        std::string child = NeedStr(args, "name");
        if (!SessAttached(s))
            return ToolErr(json{{"error", "not attached (call attach first)"}});
        CoutMute mute;
        SessEnsureIndex(s);
        uintptr_t f = s.inst.FindFirstChild(a, child);
        if (!f) return ToolErr(json{{"error", "child not found"}, {"name", child}});
        return ToolOk(NodeInfo(s, f));
    }
    if (name == "resolve_path") {
        uintptr_t root = NeedAddr(args, "root");
        std::string dotted = NeedStr(args, "dotted");
        if (!SessAttached(s))
            return ToolErr(json{{"error", "not attached (call attach first)"}});
        CoutMute mute;
        SessEnsureIndex(s);
        uintptr_t r = s.inst.ResolvePath(root, dotted);
        if (!r) return ToolErr(json{{"error", "path not resolved"}, {"dotted", dotted}});
        return ToolOk(NodeInfo(s, r));
    }
    if (name == "read_bytes") {
        uintptr_t a = NeedAddr(args, "addr");
        size_t n = NeedLength(args);
        if (!SessAttached(s))
            return ToolErr(json{{"error", "not attached (call attach first)"}});
        std::vector<uint8_t> buf(n);
        bool ok = false;
        {
            CoutMute mute;
            ok = s.mem.CustomRead(a, buf.data(), n);
        }
        if (!ok) return ToolErr(json{{"error", "read failed"}, {"address", FormatAddr(a)}});
        return ToolOk(json{{"address", FormatAddr(a)},
                           {"length", n},
                           {"base64", B64Encode(buf.data(), buf.size())}});
    }
    if (name == "write_bytes") {
        uintptr_t a = NeedAddr(args, "addr");
        std::string data = NeedStr(args, "data");
        if (!SessAttached(s))
            return ToolErr(json{{"error", "not attached (call attach first)"}});
        std::vector<uint8_t> raw;
        std::string derr;
        if (!B64Decode(data, raw, derr))
            throw RpcError(-32602, std::string("invalid params: 'data' is not valid base64"));
        if (raw.empty())
            throw RpcError(-32602, "invalid params: 'data' decodes to zero bytes");
        if (raw.size() > 65536)
            throw RpcError(-32602, "invalid params: 'data' exceeds 65536 decoded bytes");
        bool ok = false;
        {
            CoutMute mute;
            ok = s.mem.CustomWrite(a, raw.data(), raw.size());
        }
        if (!ok) return ToolErr(json{{"ok", false}, {"error", "write failed"}, {"address", FormatAddr(a)}});
        return ToolOk(
            json{{"ok", true}, {"address", FormatAddr(a)}, {"bytes_written", raw.size()}});
    }
    if (name == "run_luau") {
        std::string code = NeedStr(args, "code");
        bool ok = false;
        json payload;
        {
            CoutMute mute;
            payload = DoRunLuau(s, code, ok);
        }
        return ok ? ToolOk(payload) : ToolErr(payload);
    }
    if (name == "get_camera") {
        if (!SessAttached(s))
            return ToolErr(json{{"error", "not attached (call attach first)"}});
        uintptr_t cam = 0;
        float pos[3] = {}, rot[9] = {}, fov = 0, vpW = 0, vpH = 0;
        bool ok = false;
        {
            CoutMute mute;
            ok = s.inst.GetCamera(s.pid, cam, pos, rot, fov, vpW, vpH);
        }
        if (!ok || !cam) return ToolErr(json{{"error", "camera unavailable"}});
        json p;
        p["address"] = FormatAddr(cam);
        p["pos"] = {pos[0], pos[1], pos[2]};
        p["rot"] = {rot[0], rot[1], rot[2], rot[3], rot[4], rot[5], rot[6], rot[7], rot[8]};
        p["fov"] = fov;
        p["viewport"] = {vpW, vpH};
        return ToolOk(p);
    }
    if (name == "get_module_base") {
        std::string mod = NeedStr(args, "name");
        if (!SessAttached(s))
            return ToolErr(json{{"error", "not attached (call attach first)"}});
        uintptr_t base = 0;
        size_t size = 0;
        {
            CoutMute mute;
            s.inst.ModuleInfo(s.pid, mod, base, size);
        }
        if (!base) return ToolErr(json{{"error", "module not found"}, {"name", mod}});
        return ToolOk(json{{"name", mod},
                           {"base", FormatAddr(base)},
                           {"size", (unsigned long long)size}});
    }
    if (name == "get_bytecode") {
        uintptr_t a = NeedAddr(args, "addr");
        if (!SessAttached(s))
            return ToolErr(json{{"error", "not attached (call attach first)"}});
        std::string cls, nm;
        std::vector<uint8_t> chunk;
        bool got = false;
        {
            CoutMute mute;
            cls = s.inst.GetClass(a);
            nm = s.inst.GetName(a);
            got = GetScriptBytecode(s.inst, a, chunk);
        }
        if (!got)
            return ToolErr(json{{"error", "bytecode layout unresolved for this build"},
                                {"address", FormatAddr(a)},
                                {"class", cls},
                                {"name", nm}});
        json p;
        p["address"] = FormatAddr(a);
        p["class"] = cls;
        p["name"] = nm;
        p["encoding"] = "luau-chunk-base64";
        p["byte_length"] = chunk.size();
        p["bytecode_base64"] =
            B64EncodeStr(std::string((const char*)chunk.data(), chunk.size()));
        return ToolOk(p);
    }
    if (name == "decompile") {
        std::string b64 = NeedStr(args, "bytecode");
        std::string filename;
        auto it = args.find("filename");
        if (it != args.end() && !it->is_null()) {
            if (!it->is_string() || it->get<std::string>().empty())
                throw RpcError(-32602, "invalid params: 'filename' must be a non-empty string");
            filename = it->get<std::string>();
        }
        std::vector<uint8_t> chunk;
        std::string derr;
        if (!B64Decode(b64, chunk, derr))
            throw RpcError(-32602, std::string("invalid params: 'bytecode' is not valid base64"));
        if (chunk.empty() || chunk.size() > (1u << 20))
            throw RpcError(-32602, "invalid params: 'bytecode' decodes to 0 or >1MiB bytes");
        std::string src;
        {
            CoutMute mute;
            src = DecompileBytecode(chunk, filename.empty() ? "chunk.luau" : filename);
        }
        if (src.empty())
            return ToolErr(json{{"error", "decompile request failed"}});
        json p;
        p["bytes"] = chunk.size();
        p["source"] = src;
        if (!filename.empty()) {
            std::ofstream o(filename, std::ios::binary);
            if (o && (o << src)) {
                p["saved_to"] = filename;
            } else {
                p["warning"] = std::string("save failed: ") + filename;
            }
        }
        return ToolOk(p);
    }
    if (name == "list_scripts") {
        if (!SessAttached(s))
            return ToolErr(json{{"error", "not attached (call attach first)"}});
        std::vector<ScriptInfo> out;
        {
            CoutMute mute;
            SessEnsureIndex(s);
            if (!ListClientScripts(s.inst, s.pid, out))
                return ToolErr(json{{"error", "datamodel not resolved"}});
        }
        json arr = json::array();
        for (auto& e : out)
            arr.push_back(json{{"address", FormatAddr(e.addr)},
                               {"name", e.name},
                               {"class", e.cls},
                               {"parent", e.parentName}});
        return ToolOk(json{{"count", out.size()}, {"scripts", arr}});
    }
    if (name == "decompile_selftest") {
        std::string src;
        {
            CoutMute mute;
            if (!LuauDecompileSelfTest(src))
                return ToolErr(json{{"error", "self-test failed (compile or POST)"}});
        }
        return ToolOk(json{{"ok", true}, {"source", src}});
    }
    throw RpcError(-32602, std::string("unknown tool: ") + name);
}

// ---------------------------------------------------------------- server

static void Send(const json& r) {
    std::cout << r.dump() << "\n" << std::flush;
}

int RunMcpServer() {
    McpSession sess;
    std::string line;
    while (true) {
        if (!std::getline(std::cin, line)) break; // EOF -> shutdown
        if (!line.empty() && line.back() == '\r') line.pop_back();
        bool blank = true;
        for (char c : line) {
            if (!std::isspace((unsigned char)c)) {
                blank = false;
                break;
            }
        }
        if (blank) continue;

        json req;
        try {
            req = json::parse(line);
        } catch (...) {
            json r;
            r["jsonrpc"] = "2.0";
            r["id"] = nullptr;
            r["error"] = json{{"code", -32700}, {"message", "Parse error"}};
            Send(r);
            continue;
        }
        if (!req.is_object()) {
            json r;
            r["jsonrpc"] = "2.0";
            r["id"] = nullptr;
            r["error"] = json{{"code", -32600}, {"message", "Invalid Request"}};
            Send(r);
            continue;
        }
        bool hasId = req.contains("id");
        json id = hasId ? req["id"] : json(nullptr);
        std::string method;
        try {
            method = req.value("method", "");
        } catch (...) {
            method.clear();
        }
        if (method.empty()) {
            if (!hasId) continue;
            json r;
            r["jsonrpc"] = "2.0";
            r["id"] = id;
            r["error"] = json{{"code", -32600}, {"message", "Invalid Request"}};
            Send(r);
            continue;
        }
        json params = json::object();
        try {
            auto itp = req.find("params");
            if (itp != req.end() && itp->is_object()) params = *itp;
        } catch (...) {
            params = json::object();
        }

        if (method == "notifications/initialized") continue;
        if (method.size() > 14 && method.compare(0, 14, "notifications/") == 0) continue;

        if (method == "initialize") {
            json r;
            r["jsonrpc"] = "2.0";
            r["id"] = id;
            r["result"] = json{{"protocolVersion", "2024-11-05"},
                               {"capabilities",
                                json{{"tools", json::object()},
                                     {"prompts", json::object()},
                                     {"resources", json::object()}}},
                               {"serverInfo", json{{"name", "lusted"}, {"version", "1.0"}}}};
            Send(r);
            continue;
        }
        if (method == "ping") {
            if (!hasId) continue;
            json r;
            r["jsonrpc"] = "2.0";
            r["id"] = id;
            r["result"] = json::object();
            Send(r);
            continue;
        }
        if (method == "tools/list") {
            if (!hasId) continue;
            json r;
            r["jsonrpc"] = "2.0";
            r["id"] = id;
            r["result"] = json{{"tools", BuildToolsList()}};
            Send(r);
            continue;
        }
        if (method == "prompts/list") {
            if (!hasId) continue;
            json r;
            r["jsonrpc"] = "2.0";
            r["id"] = id;
            r["result"] = json{{"prompts", json::array()}};
            Send(r);
            continue;
        }
        if (method == "resources/list") {
            if (!hasId) continue;
            json r;
            r["jsonrpc"] = "2.0";
            r["id"] = id;
            r["result"] = json{{"resources", json::array()}};
            Send(r);
            continue;
        }
        if (method == "logging/setLevel") {
            if (!hasId) continue;
            json r;
            r["jsonrpc"] = "2.0";
            r["id"] = id;
            r["result"] = json::object();
            Send(r);
            continue;
        }
        if (method == "tools/call") {
            if (!hasId) continue;
            std::string toolName;
            json args = json::object();
            try {
                toolName = params.value("name", "");
                auto ita = params.find("arguments");
                if (ita != params.end() && ita->is_object()) args = *ita;
            } catch (...) {
                toolName.clear();
            }
            if (toolName.empty()) {
                json r;
                r["jsonrpc"] = "2.0";
                r["id"] = id;
                r["error"] = json{{"code", -32602}, {"message", "invalid params: 'name' is required"}};
                Send(r);
                continue;
            }
            try {
                json envelope = DispatchTool(sess, toolName, args);
                json r;
                r["jsonrpc"] = "2.0";
                r["id"] = id;
                r["result"] = envelope;
                Send(r);
            } catch (const RpcError& e) {
                json r;
                r["jsonrpc"] = "2.0";
                r["id"] = id;
                r["error"] = json{{"code", e.code}, {"message", e.message}};
                Send(r);
            } catch (const std::exception& e) {
                json r;
                r["jsonrpc"] = "2.0";
                r["id"] = id;
                r["error"] = json{{"code", -32603}, {"message", e.what()}};
                Send(r);
            } catch (...) {
                json r;
                r["jsonrpc"] = "2.0";
                r["id"] = id;
                r["error"] = json{{"code", -32603}, {"message", "Internal error"}};
                Send(r);
            }
            continue;
        }
        if (!hasId) continue;
        json r;
        r["jsonrpc"] = "2.0";
        r["id"] = id;
        r["error"] = json{{"code", -32601}, {"message", "Method not found"}};
        Send(r);
    }
    return 0;
}
