// Diagnostic: queries all devices once, like the app does, and prints what came back.
#include "../src/battery.h"

#include <cstdio>
#include <windows.h>

int wmain() {
    ULONGLONG t0 = GetTickCount64();
    auto devs = PollAll(true);
    std::printf("poll took %llu ms, %zu device(s)\n", GetTickCount64() - t0, devs.size());
    for (const auto& d : devs) {
        std::wprintf(L"  %-22ls key=%-10ls ok=%d %3d%% charging=%d raw=%02X %02X %02X %02X %02X %02X\n", d.name.c_str(),
                     d.key.c_str(), d.r.ok, d.r.percent, d.r.charging, d.r.raw[0], d.r.raw[1], d.r.raw[2], d.r.raw[3],
                     d.r.raw[4], d.r.raw[5]);
    }
    return 0;
}
