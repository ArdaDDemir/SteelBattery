#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace {
std::mutex g_mu;
std::wstring g_path, g_old;
constexpr LONGLONG kMaxBytes = 512 * 1024;
}  // namespace

void LogInit(const std::wstring& dir) {
    std::lock_guard l(g_mu);
    g_path = dir + L"\\log.txt";
    g_old = dir + L"\\log.old.txt";
}

std::wstring LogPath() { return g_path; }

void Log(const wchar_t* fmt, ...) {
    wchar_t msg[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(msg, _TRUNCATE, fmt, ap);
    va_end(ap);
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t line[1200];
    int n = swprintf_s(line, L"%04d-%02d-%02d %02d:%02d:%02d.%03d  %s\r\n", st.wYear, st.wMonth, st.wDay, st.wHour,
                       st.wMinute, st.wSecond, st.wMilliseconds, msg);
    // UTF-8 so the file opens fine in Notepad.
    char utf8[3600];
    int len = WideCharToMultiByte(CP_UTF8, 0, line, n, utf8, sizeof utf8, nullptr, nullptr);

    std::lock_guard l(g_mu);
    if (g_path.empty()) return;
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (GetFileAttributesExW(g_path.c_str(), GetFileExInfoStandard, &fa) &&
        ((LONGLONG)fa.nFileSizeHigh << 32 | fa.nFileSizeLow) > kMaxBytes)
        MoveFileExW(g_path.c_str(), g_old.c_str(), MOVEFILE_REPLACE_EXISTING);
    HANDLE f = CreateFileW(g_path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD w;
    WriteFile(f, utf8, (DWORD)len, &w, nullptr);
    CloseHandle(f);
}
