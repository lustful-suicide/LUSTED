#pragma once
#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>

// Append-only UTC timeline used to line the tool's actions up against Roblox's
// own disconnect timestamps. Lines are staged in memory and written out in
// batches: an open/flush per event costs milliseconds on the write path, which
// is exactly where the cost cannot be paid.
class LustedLogBuffer {
public:
    void Add(const char* line) {
        SYSTEMTIME st{};
        GetSystemTime(&st);
        char head[40];
        const int n = snprintf(head, sizeof(head), "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ  ",
                               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                               st.wSecond, st.wMilliseconds);
        std::string entry;
        entry.reserve((n > 0 ? (size_t)n : 0) + strlen(line) + 1);
        if (n > 0) entry.append(head, (size_t)n);
        entry.append(line);
        entry.push_back('\n');

        std::lock_guard lk(m_);
        pending_ += entry;
        if (++lines_ >= kFlushEvery) FlushLocked();
    }

    void Flush() {
        std::lock_guard lk(m_);
        FlushLocked();
    }

private:
    static constexpr size_t kFlushEvery = 32;
    void FlushLocked() {
        if (pending_.empty()) return;
        FILE* f = nullptr;
        if (fopen_s(&f, "C:/LUSTED/lusted.log", "a") != 0 || !f) {
            // Keep the tail so a later flush still writes it; drop only once the
            // buffer would grow without bound.
            if (pending_.size() > (1u << 20)) pending_.clear(), lines_ = 0;
            return;
        }
        fwrite(pending_.data(), 1, pending_.size(), f);
        fclose(f);
        pending_.clear();
        lines_ = 0;
    }

    std::mutex m_;
    std::string pending_;
    size_t lines_ = 0;
};

inline LustedLogBuffer& LustedLogSink() {
    static LustedLogBuffer sink;
    return sink;
}

inline void LustedLog(const char* msg) { LustedLogSink().Add(msg); }
inline void LustedLogFlush() { LustedLogSink().Flush(); }

inline void LustedLogf(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    LustedLog(buf);
}
