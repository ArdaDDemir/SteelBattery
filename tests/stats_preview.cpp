// Opens the statistics window with made-up data for a few seconds and saves a screenshot of it
// to out\stats_preview.png, so the layout can be checked without days of real use.
#include "../src/history.h"
#include "../src/render.h"
#include "../src/stats.h"
#include "../src/statswin.h"

#include <objidl.h>
#include <gdiplus.h>

int wmain() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    InitGraphics();
    Stats stats(L"out\\stats_preview.csv");
    int64_t now = UnixNow();
    auto dev = [](Kind k, const wchar_t* name, const wchar_t* key, int pct, bool ch, bool stale) {
        Reading r;
        r.ok = true;
        r.percent = pct;
        r.charging = ch;
        DeviceStatus d{k, name, key, true, Link::Wireless, r};
        d.stale = stale;
        return d;
    };
    // 10 days: each day ~2-4 h of mouse use draining 5%/h, ~3 h of headset use, a charge every few days.
    int mouse = 100, head = 100;
    for (int day = 9; day >= 0; --day) {
        int64_t t = now - day * 86400 - 6 * 3600;
        int activeSteps = 24 + (day * 7) % 30;  // 5-min steps
        for (int i = 0; i < activeSteps; ++i, t += 300) {
            if (i % 12 == 11 && mouse > 10) mouse -= 5;
            if (i % 20 == 19 && head > 5) head -= 3;
            stats.Update({dev(Kind::Headset, L"Arctis Nova 5", L"headset", head, false, false),
                          dev(Kind::Mouse, L"Aerox 5 Wireless", L"mouse", mouse, false, false)},
                         t);
        }
        for (int i = 0; i < 6; ++i, t += 300)
            stats.Update({dev(Kind::Headset, L"Arctis Nova 5", L"headset", head, false, true),
                          dev(Kind::Mouse, L"Aerox 5 Wireless", L"mouse", mouse, false, true)},
                         t);
        if (mouse <= 40) {
            for (int i = 0; i < 12 && mouse < 100; ++i, t += 300) {
                mouse = mouse + 10 > 100 ? 100 : mouse + 10;
                stats.Update({dev(Kind::Mouse, L"Aerox 5 Wireless", L"mouse", mouse, true, false)}, t);
            }
        }
        if (head <= 30) head = 100;
    }
    HINSTANCE inst = GetModuleHandleW(nullptr);
    OpenStatsWindow(inst, &stats, L"out");
    HWND w = FindWindowW(L"SteelBatteryStats", nullptr);
    ULONGLONG end = GetTickCount64() + 2500;
    MSG m;
    while (GetTickCount64() < end) {
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&m);
        Sleep(15);
    }
    // Screenshot the window client area.
    RECT rc;
    GetClientRect(w, &rc);
    HDC wdc = GetDC(w), mem = CreateCompatibleDC(wdc);
    HBITMAP bmp = CreateCompatibleBitmap(wdc, rc.right, rc.bottom);
    SelectObject(mem, bmp);
    PrintWindow(w, mem, PW_CLIENTONLY);
    {
        Gdiplus::Bitmap b(bmp, nullptr);
        CLSID png;
        UINT num = 0, size = 0;
        Gdiplus::GetImageEncodersSize(&num, &size);
        std::vector<BYTE> buf(size);
        auto* codecs = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buf.data());
        Gdiplus::GetImageEncoders(num, size, codecs);
        for (UINT i = 0; i < num; ++i)
            if (!wcscmp(codecs[i].MimeType, L"image/png")) png = codecs[i].Clsid;
        b.Save(L"out\\stats_preview.png", &png, nullptr);
    }
    DestroyWindow(w);
    ShutdownGraphics();
    return 0;
}
