#pragma once
#include <string>

// Small diagnostic log at %LOCALAPPDATA%\SteelBattery\log.txt (thread-safe, rotates at 512 KB to log.old.txt).
void LogInit(const std::wstring& dir);
void Log(const wchar_t* fmt, ...);
std::wstring LogPath();
