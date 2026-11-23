// Renders sample OLED frames to out\oled_*.png so the 128x40 layout can be checked by eye.
// Pass --send to also push the charging sample to the keyboard through SteelSeries GG for 15 s.
#include "../src/oled.h"
#include "../src/render.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

int wmain(int argc, wchar_t** argv) {
    InitGraphics();
    auto dev = [](Kind k, int pct, bool ch, bool ok = true, bool stale = false) {
        Reading r;
        r.ok = ok;
        r.percent = pct;
        r.charging = ch;
        DeviceStatus d{k, L"", L"", true, Link::Wireless, r};
        d.stale = stale;
        return d;
    };
    struct Case {
        const wchar_t* file;
        std::vector<DeviceStatus> devs;
        int phase;
    };
    const Case cases[] = {
        {L"out\\oled_1_charging.png", {dev(Kind::Headset, 76, true), dev(Kind::Mouse, 75, false)}, 0},
        {L"out\\oled_2_charging_blink.png", {dev(Kind::Headset, 76, true), dev(Kind::Mouse, 75, false)}, 1},
        {L"out\\oled_3_asleep.png", {dev(Kind::Headset, 100, false), dev(Kind::Mouse, 70, false, true, true)}, 0},
        {L"out\\oled_4_low.png", {dev(Kind::Headset, 45, false), dev(Kind::Mouse, 10, false)}, 0},
        {L"out\\oled_5_single.png", {dev(Kind::Gamepad, 30, true)}, 0},
        {L"out\\oled_6_none.png", {}, 0},
    };
    bool ok = true;
    for (const auto& c : cases) ok &= SaveOledPng(RenderOled(c.devs, 15, c.phase), c.file, 4);
    std::printf(ok ? "oled previews written\n" : "oled preview FAILED\n");
    if (argc > 1 && wcscmp(argv[1], L"--send") == 0) {
        OledStart();
        OledSet({RenderOled(cases[0].devs, 15, 0), RenderOled(cases[0].devs, 15, 1)});
        std::this_thread::sleep_for(std::chrono::seconds(15));
        OledStop();
    }
    ShutdownGraphics();
    return ok ? 0 : 1;
}
