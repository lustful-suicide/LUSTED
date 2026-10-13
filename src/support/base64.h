#pragma once
// Minimal base64 (encode/decode), header-only. No dependency; used for
// bytecode transport to the decompiler API and MCP binary payloads.
#include <cstdint>
#include <string>
#include <vector>

namespace b64 {

inline const char* Alphabet() {
    return "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
}

inline std::string Encode(const uint8_t* data, size_t len) {
    const char* ab = Alphabet();
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        const uint32_t a = data[i];
        const uint32_t b = (i + 1 < len) ? data[i + 1] : 0;
        const uint32_t c = (i + 2 < len) ? data[i + 2] : 0;
        const uint32_t v = (a << 16) | (b << 8) | c;
        out.push_back(ab[(v >> 18) & 63]);
        out.push_back(ab[(v >> 12) & 63]);
        out.push_back(i + 1 < len ? ab[(v >> 6) & 63] : '=');
        out.push_back(i + 2 < len ? ab[v & 63] : '=');
    }
    return out;
}

inline std::string Encode(const std::string& s) {
    return Encode((const uint8_t*)s.data(), s.size());
}

inline std::string Encode(const std::vector<uint8_t>& v) {
    return Encode(v.data(), v.size());
}

inline int Digit(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

// Returns false on alphabet violation. Whitespace is rejected, not skipped:
// callers pass exact payloads.
inline bool Decode(const std::string& s, std::vector<uint8_t>& out) {
    out.clear();
    if (s.size() % 4 != 0) return false;
    out.reserve(s.size() / 4 * 3);
    for (size_t i = 0; i < s.size(); i += 4) {
        int d[4];
        for (int k = 0; k < 4; ++k) {
            if (s[i + k] == '=') d[k] = 0;
            else {
                d[k] = Digit(s[i + k]);
                if (d[k] < 0) {
                    out.clear();
                    return false;
                }
            }
        }
        const uint32_t v = ((uint32_t)d[0] << 18) | ((uint32_t)d[1] << 12) |
                           ((uint32_t)d[2] << 6) | (uint32_t)d[3];
        out.push_back((uint8_t)((v >> 16) & 0xFF));
        if (s[i + 2] != '=') out.push_back((uint8_t)((v >> 8) & 0xFF));
        if (s[i + 3] != '=') out.push_back((uint8_t)(v & 0xFF));
    }
    return true;
}

} // namespace b64
