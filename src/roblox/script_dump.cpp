#include "roblox/script_dump.h"
#include "roblox/instance.h"
#include "roblox/offsets_fetcher.h"
#include "support/base64.h"
#include "support/log.h"
#include <iostream>

namespace {

constexpr const char* kDecompileUrl = "https://lunaux-decompiler.dev/api/decompile";

std::string JsonEscape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if ((unsigned char)c < 0x20) {
                    char b[8];
                    snprintf(b, sizeof(b), "\\u%04x", c);
                    o += b;
                } else
                    o += c;
        }
    }
    return o;
}

} // namespace

bool ListClientScripts(InstanceStore& inst, DWORD pid, std::vector<ScriptInfo>& out) {
    out.clear();
    const uintptr_t dm = inst.GetDataModel(pid);
    if (!dm) return false;
    std::vector<uintptr_t> all;
    inst.GetDescendants(dm, all);
    out.reserve(256);
    for (uintptr_t a : all) {
        const std::string cls = inst.GetClass(a);
        if (cls != "LocalScript" && cls != "ModuleScript") continue;
        ScriptInfo si;
        si.addr = a;
        si.name = inst.GetName(a);
        si.cls = cls;
        const uintptr_t p = inst.GetParent(a);
        if (p) si.parentName = inst.GetName(p);
        out.push_back(std::move(si));
    }
    return true;
}

bool GetScriptBytecode(InstanceStore&, uintptr_t, std::vector<uint8_t>&) {
    // Layout unresolved (see header). Returning false, not garbage.
    return false;
}

std::string DecompileBytecode(const std::vector<uint8_t>& chunk, const std::string& filename) {
    if (chunk.empty() || chunk.size() > (1u << 20)) return "";
    const std::string body = std::string("{\"bytecode\":\"") + b64::Encode(chunk) +
                             "\",\"filename\":\"" + JsonEscape(filename.empty() ? "chunk.luau" : filename) +
                             "\",\"options\":{}}";
    bool ok = false;
    const std::string resp = OffsetsFetcher::HttpPostJson(kDecompileUrl, body, ok);
    if (resp.rfind("ERROR:", 0) == 0 || !ok || resp.empty()) {
        std::cerr << "[decompile] " << (resp.empty() ? std::string("request failed") : resp)
                  << std::endl;
        return "";
    }
    return resp;
}
