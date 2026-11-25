#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "stats.h"

// Opens (or brings to front) the usage statistics window. `stats` must outlive the window.
void OpenStatsWindow(HINSTANCE inst, Stats* stats, const std::wstring& dataDir);
