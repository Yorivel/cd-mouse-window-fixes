#include "log.h"

#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <share.h>

namespace {
FILE* g_file = nullptr;
std::mutex g_mutex;
}

void logx::open(const wchar_t* path)
{
    std::lock_guard lock(g_mutex);
    if (!g_file)
        g_file = _wfsopen(path, L"w", _SH_DENYNO); // others may read it while the game runs
}

void logx::close()
{
    std::lock_guard lock(g_mutex);
    if (g_file) std::fclose(g_file);
    g_file = nullptr;
}

void logx::write(const char* fmt, ...)
{
    std::lock_guard lock(g_mutex);
    if (!g_file)
        return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    std::fprintf(g_file, "[%02u:%02u:%02u.%03u] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list args;
    va_start(args, fmt);
    std::vfprintf(g_file, fmt, args);
    va_end(args);
    std::fputc('\n', g_file);
    std::fflush(g_file);
}
