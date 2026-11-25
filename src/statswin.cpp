// Usage statistics window: per device totals for today / 7 / 30 days and a 14-day usage chart.
#include "statswin.h"

#include <algorithm>
using std::max;
using std::min;
#include <dwmapi.h>
#include <objidl.h>
#include <gdiplus.h>
#include <shellapi.h>

#include <string>
#include <vector>

#include "history.h"
#include "render.h"

namespace G = Gdiplus;

namespace {

HWND g_win;
Stats* g_stats;
std::wstring g_dir;
RECT g_linkFolder{}, g_linkCsv{};

constexpr UINT_PTR TIMER_REFRESH = 1;
const G::Color kBg(255, 28, 28, 30), kCard(255, 38, 38, 42), kFg(255, 240, 240, 240), kDim(255, 150, 150, 155),
    kAccent(255, 61, 220, 132), kLink(255, 110, 170, 255), kGrid(255, 60, 60, 66);

float S(HWND h, float v) { return v * GetDpiForWindow(h) / 96.f; }

std::wstring Hm(double minutes) {
    int m = (int)(minutes + 0.5);
    wchar_t b[32];
    if (m < 60)
        swprintf_s(b, L"%d dk", m);
    else
        swprintf_s(b, L"%d sa %02d dk", m / 60, m % 60);
    return b;
}

Kind KindOf(const std::wstring& key, const std::wstring& name) {
    if (key == L"headset") return Kind::Headset;
    if (key == L"mouse") return Kind::Mouse;
    return KindFromName(name);
}

void Txt(G::Graphics& g, const std::wstring& t, const G::Font& f, G::Color c, float x, float y, bool right = false) {
    G::SolidBrush b(c);
    G::RectF m;
    g.MeasureString(t.c_str(), (INT)t.size(), &f, G::PointF(0, 0), G::StringFormat::GenericTypographic(), &m);
    g.DrawString(t.c_str(), (INT)t.size(), &f, G::PointF(right ? x - m.Width : x, y), G::StringFormat::GenericTypographic(), &b);
}

float TextW(G::Graphics& g, const std::wstring& t, const G::Font& f) {
    G::RectF m;
    g.MeasureString(t.c_str(), (INT)t.size(), &f, G::PointF(0, 0), G::StringFormat::GenericTypographic(), &m);
    return m.Width;
}

void FillRound(G::Graphics& g, const G::Brush& b, G::RectF r, float rad) {
    G::GraphicsPath p;
    float d = rad * 2;
    p.AddArc(r.X, r.Y, d, d, 180, 90);
    p.AddArc(r.GetRight() - d, r.Y, d, d, 270, 90);
    p.AddArc(r.GetRight() - d, r.GetBottom() - d, d, d, 0, 90);
    p.AddArc(r.X, r.GetBottom() - d, d, d, 90, 90);
    p.CloseFigure();
    g.FillPath(&b, &p);
}

constexpr float kBlockH = 322, kHeaderH = 70, kFooterH = 56, kWidth = 640;

int WindowHeight(HWND h, size_t devices) {
    return (int)S(h, kHeaderH + max<size_t>(devices, 1) * kBlockH + kFooterH);
}

void Paint(HWND hwnd, HDC dc) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    G::Bitmap bmp(rc.right, rc.bottom, PixelFormat32bppPARGB);
    G::Graphics g(&bmp);
    g.SetSmoothingMode(G::SmoothingModeAntiAlias);
    g.SetTextRenderingHint(G::TextRenderingHintClearTypeGridFit);
    g.Clear(kBg);
    auto s = [&](float v) { return S(hwnd, v); };
    G::Font title(L"Segoe UI Semibold", s(20), G::FontStyleRegular, G::UnitPixel);
    G::Font head(L"Segoe UI Semibold", s(15), G::FontStyleRegular, G::UnitPixel);
    G::Font body(L"Segoe UI", s(13), G::FontStyleRegular, G::UnitPixel);
    G::Font bold(L"Segoe UI Semibold", s(13), G::FontStyleRegular, G::UnitPixel);
    G::Font tiny(L"Segoe UI", s(11), G::FontStyleRegular, G::UnitPixel);

    int64_t now = UnixNow();
    float pad = s(20), W = (float)rc.right;
    Txt(g, L"Kullanım istatistikleri", title, kFg, pad, s(18));
    Txt(g, L"Aktif = cihaz uyanık ve pilde. Süreler bilgisayar açıkken ölçülür.", tiny, kDim, pad, s(46));

    auto keys = g_stats->Keys();
    float y = s(kHeaderH);
    if (keys.empty()) {
        Txt(g, L"Henüz veri yok. Uygulama çalıştıkça burası dolacak.", body, kDim, pad, y + s(20));
    }
    for (const auto& key : keys) {
        std::wstring name = g_stats->NameOf(key);
        G::SolidBrush card(kCard);
        FillRound(g, card, {pad - s(8), y, W - 2 * pad + s(16), s(kBlockH - 14)}, s(10));
        HDC hdc = g.GetHDC();
        PaintDeviceIcon(hdc, KindOf(key, name), pad + s(4), y + s(12), s(22), RGB(240, 240, 240));
        g.ReleaseHDC(hdc);
        Txt(g, name, head, kFg, pad + s(36), y + s(14));

        // Table: label | today | 7 days | 30 days
        StatSummary sum[3] = {g_stats->Summarize(key, 1, now), g_stats->Summarize(key, 7, now),
                              g_stats->Summarize(key, 30, now)};
        const wchar_t* cols[] = {L"Bugün", L"7 gün", L"30 gün"};
        float colX[] = {W - pad - s(250), W - pad - s(130), W - pad - s(10)};
        float ty = y + s(48);
        for (int c = 0; c < 3; ++c) Txt(g, cols[c], bold, kDim, colX[c], ty, true);
        struct Row {
            const wchar_t* label;
            std::wstring (*fmt)(const StatSummary&);
        };
        const Row rows[] = {
            {L"Aktif kullanım", [](const StatSummary& x) { return Hm(x.activeMin); }},
            {L"Uykuda", [](const StatSummary& x) { return Hm(x.idleMin); }},
            {L"Şarjda", [](const StatSummary& x) { return Hm(x.chargeMin); }},
            {L"Şarj sayısı", [](const StatSummary& x) { return std::to_wstring(x.chargeCount); }},
            {L"Harcanan pil", [](const StatSummary& x) { return L"%" + std::to_wstring(x.used); }},
            {L"Saatte tüketim", [](const StatSummary& x) {
                 wchar_t b[32];
                 if (x.DrainPerHour() <= 0) return std::wstring(L"—");
                 swprintf_s(b, L"%%%.1f / sa", x.DrainPerHour());
                 return std::wstring(b);
             }},
            {L"Tam şarjla", [](const StatSummary& x) {
                 if (x.FullChargeHours() <= 0) return std::wstring(L"—");
                 return L"~" + Hm(x.FullChargeHours() * 60);
             }},
        };
        for (const auto& r : rows) {
            ty += s(22);
            Txt(g, r.label, body, kFg, pad + s(36), ty);
            for (int c = 0; c < 3; ++c) Txt(g, r.fmt(sum[c]), body, kFg, colX[c], ty, true);
        }

        // 14-day bar chart of active use.
        auto daily = g_stats->DailyActive(key, 14, now);
        double peak = 60;
        for (double v : daily) peak = max(peak, v);
        float cx = pad + s(36), cw = W - pad - s(10) - cx, cy = ty + s(42), ch = s(40);
        Txt(g, L"Son 14 gün aktif kullanım", tiny, kDim, cx, cy - s(16));
        G::Pen grid(kGrid, 1);
        g.DrawLine(&grid, cx, cy + ch, cx + cw, cy + ch);
        float bw = cw / 14;
        G::SolidBrush bar(kAccent), barToday(G::Color(255, 120, 240, 170));
        for (int i = 0; i < 14; ++i) {
            float h = (float)(daily[i] / peak) * ch;
            if (daily[i] > 0 && h < s(2)) h = s(2);
            if (h > 0) FillRound(g, i == 13 ? barToday : bar, {cx + i * bw + s(3), cy + ch - h, bw - s(6), h}, s(2));
        }
        Txt(g, L"14 gün önce", tiny, kDim, cx, cy + ch + s(4));
        Txt(g, L"bugün", tiny, kDim, cx + cw, cy + ch + s(4), true);
        std::wstring peakTxt = L"en çok " + Hm(peak);
        Txt(g, peakTxt, tiny, kDim, cx + cw, cy - s(16), true);
        y += s(kBlockH);
    }

    // Footer with links.
    float fy = rc.bottom - s(kFooterH) + s(16);
    std::wstring a = L"Veri klasörünü aç", b = L"stats.csv'yi aç (Excel)";
    Txt(g, a, body, kLink, pad, fy);
    float aw = TextW(g, a, body);
    Txt(g, b, body, kLink, pad + aw + s(24), fy);
    float bw2 = TextW(g, b, body);
    g_linkFolder = {(LONG)pad, (LONG)fy, (LONG)(pad + aw), (LONG)(fy + s(18))};
    g_linkCsv = {(LONG)(pad + aw + s(24)), (LONG)fy, (LONG)(pad + aw + s(24) + bw2), (LONG)(fy + s(18))};

    G::Graphics out(dc);
    out.DrawImage(&bmp, 0, 0);
}

LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        Paint(hwnd, dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_TIMER: InvalidateRect(hwnd, nullptr, FALSE); return 0;
    case WM_SETCURSOR: {
        POINT p;
        GetCursorPos(&p);
        ScreenToClient(hwnd, &p);
        if (PtInRect(&g_linkFolder, p) || PtInRect(&g_linkCsv, p)) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
        break;
    }
    case WM_LBUTTONUP: {
        POINT p{(short)LOWORD(lp), (short)HIWORD(lp)};
        if (PtInRect(&g_linkFolder, p)) ShellExecuteW(hwnd, L"open", g_dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        if (PtInRect(&g_linkCsv, p)) {
            g_stats->Save();
            ShellExecuteW(hwnd, L"open", g_stats->File().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        return 0;
    }
    case WM_DPICHANGED: {
        auto* r = reinterpret_cast<RECT*>(lp);
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER);
        return 0;
    }
    case WM_DESTROY:
        KillTimer(hwnd, TIMER_REFRESH);
        g_win = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

void OpenStatsWindow(HINSTANCE inst, Stats* stats, const std::wstring& dataDir) {
    g_stats = stats;
    g_dir = dataDir;
    if (g_win) {
        ShowWindow(g_win, SW_RESTORE);
        SetForegroundWindow(g_win);
        InvalidateRect(g_win, nullptr, FALSE);
        return;
    }
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        wc.lpfnWndProc = Proc;
        wc.lpszClassName = L"SteelBatteryStats";
        RegisterClassW(&wc);
        registered = true;
    }
    g_win = CreateWindowExW(0, L"SteelBatteryStats", L"SteelBattery — Kullanım istatistikleri",
                            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, CW_USEDEFAULT, CW_USEDEFAULT, 100,
                            100, nullptr, nullptr, inst, nullptr);
    BOOL dark = TRUE;
    DwmSetWindowAttribute(g_win, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof dark);
    // Size the client area for the devices we have data for, centred on the screen.
    RECT r{0, 0, (LONG)S(g_win, kWidth), WindowHeight(g_win, stats->Keys().size())};
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE, 0, GetDpiForWindow(g_win));
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(MonitorFromWindow(g_win, MONITOR_DEFAULTTOPRIMARY), &mi);
    int w = r.right - r.left, h = min((LONG)(r.bottom - r.top), mi.rcWork.bottom - mi.rcWork.top);
    SetWindowPos(g_win, nullptr, (mi.rcWork.left + mi.rcWork.right - w) / 2, (mi.rcWork.top + mi.rcWork.bottom - h) / 2, w, h,
                 SWP_NOZORDER);
    SetTimer(g_win, TIMER_REFRESH, 30000, nullptr);
    ShowWindow(g_win, SW_SHOW);
    SetForegroundWindow(g_win);
}
