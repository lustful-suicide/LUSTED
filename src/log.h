#pragma once
#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <mutex>

inline void LustedLog(const char* msg) {
    static std::mutex m;
    std::lock_guard lk(m);
    FILE* f = nullptr;
    if (fopen_s(&f, "C:/LUSTED/lusted.log", "a") != 0 || !f) return;
    SYSTEMTIME st{};
    GetSystemTime(&st);
    fprintf(f, "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ  %s\n",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
            st.wMilliseconds, msg);
    fclose(f);
}

inline void LustedLogf(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    LustedLog(buf);
}
