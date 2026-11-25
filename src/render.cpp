// Drawing: custom vector icons, battery gauge, band strip and hover card (GDI+, per-pixel alpha).
#include "render.h"

#include <algorithm>
using std::max;
using std::min;
#include <objidl.h>
#include <gdiplus.h>

#include <ctime>
#include <memory>

namespace G = Gdiplus;

namespace {

ULONG_PTR g_token;

// Layout constants at 96 DPI.
constexpr float kPad = 8, kGapDevices = 16, kIcon = 18, kGapIcon = 6, kBatW = 22, kBatH = 11, kCap = 3, kGapText = 5;
constexpr float kTextPx = 13;

struct Palette {
    G::Color fg, dim, outline, track;
};

Palette Pal(bool light) {
    if (light) return {G::Color(240, 17, 17, 17), G::Color(110, 17, 17, 17), G::Color(170, 17, 17, 17), G::Color(30, 0, 0, 0)};
    return {G::Color(245, 255, 255, 255), G::Color(105, 255, 255, 255), G::Color(185, 255, 255, 255),
            G::Color(38, 255, 255, 255)};
}

const G::Color kGreen(255, 61, 220, 132), kAmber(255, 255, 176, 32), kRed(255, 255, 90, 90);
const G::Color kBolt(255, 255, 212, 59), kBoltStroke(230, 20, 20, 20);

int g_low = 15;  // low-battery threshold of the current draw call

G::Color LevelColor(const Reading& r) {
    if (r.charging) return kGreen;
    if (r.percent <= g_low) return kRed;
    if (r.percent <= max(35, g_low + 10)) return kAmber;
    return kGreen;
}

bool IsLow(const Reading& r) { return r.ok && !r.charging && r.percent <= g_low; }

void AddRoundRect(G::GraphicsPath& p, G::RectF r, float rad) {
    rad = min(rad, min(r.Width, r.Height) / 2);
    float d = rad * 2;
    p.AddArc(r.X, r.Y, d, d, 180, 90);
    p.AddArc(r.GetRight() - d, r.Y, d, d, 270, 90);
    p.AddArc(r.GetRight() - d, r.GetBottom() - d, d, d, 0, 90);
    p.AddArc(r.X, r.GetBottom() - d, d, d, 90, 90);
    p.CloseFigure();
}

void FillRound(G::Graphics& g, const G::Brush& b, G::RectF r, float rad) {
    G::GraphicsPath p;
    AddRoundRect(p, r, rad);
    g.FillPath(&b, &p);
}

void StrokeRound(G::Graphics& g, const G::Pen& pen, G::RectF r, float rad) {
    G::GraphicsPath p;
    AddRoundRect(p, r, rad);
    g.DrawPath(&pen, &p);
}

void Round(G::Pen& pen) {
    pen.SetStartCap(G::LineCapRound);
    pen.SetEndCap(G::LineCapRound);
    pen.SetLineJoin(G::LineJoinRound);
}

// Icons are designed on a 16x16 grid and scaled to `size`.
void DrawHeadsetIcon(G::Graphics& g, float x, float y, float size, G::Color c) {
    G::GraphicsState saved = g.Save();
    g.TranslateTransform(x, y);
    g.ScaleTransform(size / 16, size / 16);
    G::Pen pen(c, 1.7f);
    Round(pen);
    G::GraphicsPath band;
    band.AddLine(2.4f, 11.f, 2.4f, 7.8f);
    band.AddArc(2.4f, 2.2f, 11.2f, 11.2f, 180, 180);
    band.AddLine(13.6f, 7.8f, 13.6f, 11.f);
    g.DrawPath(&pen, &band);
    G::SolidBrush b(c);
    FillRound(g, b, {0.9f, 8.9f, 4.3f, 6.4f}, 1.7f);
    FillRound(g, b, {10.8f, 8.9f, 4.3f, 6.4f}, 1.7f);
    g.Restore(saved);
}

void DrawMouseIcon(G::Graphics& g, float x, float y, float size, G::Color c) {
    G::GraphicsState saved = g.Save();
    g.TranslateTransform(x, y);
    g.ScaleTransform(size / 16, size / 16);
    G::Pen body(c, 1.6f);
    Round(body);
    StrokeRound(g, body, {3.5f, 0.9f, 9.f, 14.2f}, 4.5f);
    G::Pen wheel(c, 2.f);
    Round(wheel);
    g.DrawLine(&wheel, 8.f, 3.6f, 8.f, 6.f);
    g.Restore(saved);
}

void DrawGamepadIcon(G::Graphics& g, float x, float y, float size, G::Color c) {
    G::GraphicsState saved = g.Save();
    g.TranslateTransform(x, y);
    g.ScaleTransform(size / 16, size / 16);
    G::Pen body(c, 1.5f);
    Round(body);
    StrokeRound(g, body, {0.9f, 3.8f, 14.2f, 8.8f}, 4.2f);
    G::Pen pad(c, 1.4f);
    Round(pad);
    g.DrawLine(&pad, 2.9f, 8.2f, 5.7f, 8.2f);
    g.DrawLine(&pad, 4.3f, 6.8f, 4.3f, 9.6f);
    G::SolidBrush b(c);
    g.FillEllipse(&b, 10.2f, 6.3f, 1.8f, 1.8f);
    g.FillEllipse(&b, 11.7f, 8.3f, 1.8f, 1.8f);
    g.Restore(saved);
}

void DrawBluetoothIcon(G::Graphics& g, float x, float y, float size, G::Color c) {
    G::GraphicsState saved = g.Save();
    g.TranslateTransform(x, y);
    g.ScaleTransform(size / 16, size / 16);
    G::Pen pen(c, 1.6f);
    Round(pen);
    const G::PointF pts[] = {{4.4f, 4.6f}, {11.6f, 11.1f}, {8.f, 14.6f}, {8.f, 1.4f}, {11.6f, 4.9f}, {4.4f, 11.4f}};
    g.DrawLines(&pen, pts, 6);
    g.Restore(saved);
}

void DrawDeviceIcon(G::Graphics& g, Kind k, float x, float y, float size, G::Color c) {
    switch (k) {
    case Kind::Headset: DrawHeadsetIcon(g, x, y, size, c); break;
    case Kind::Mouse: DrawMouseIcon(g, x, y, size, c); break;
    case Kind::Gamepad: DrawGamepadIcon(g, x, y, size, c); break;
    default: DrawBluetoothIcon(g, x, y, size, c); break;
    }
}

// Lightning bolt with a dark outline so it reads on top of any fill.
void DrawBolt(G::Graphics& g, float cx, float cy, float h, float s) {
    const G::PointF unit[] = {{6.4f, 0}, {0.8f, 9.2f}, {4.8f, 9.2f}, {3.6f, 16}, {9.2f, 6.6f}, {5.2f, 6.6f}};
    float f = h / 16;
    G::PointF pts[6];
    for (int i = 0; i < 6; ++i) pts[i] = {cx + (unit[i].X - 5) * f, cy + (unit[i].Y - 8) * f};
    G::Pen stroke(kBoltStroke, 1.4f * s);
    stroke.SetLineJoin(G::LineJoinRound);
    g.DrawPolygon(&stroke, pts, 6);
    G::SolidBrush fill(kBolt);
    g.FillPolygon(&fill, pts, 6);
}

// Battery gauge; (x, y) = top-left of the body, w/h = body size (cap is drawn to the right).
void DrawBattery(G::Graphics& g, float x, float y, float w, float h, float s, const Reading& r, const Palette& p) {
    G::Color outline = r.ok ? p.outline : p.dim;
    G::Pen pen(outline, 1.2f * s);
    StrokeRound(g, pen, {x + 0.6f * s, y + 0.6f * s, w - 1.2f * s, h - 1.2f * s}, 2.8f * s);
    G::SolidBrush cap(outline);
    FillRound(g, cap, {x + w + 0.9f * s, y + h * 0.3f, 1.9f * s, h * 0.4f}, 0.9f * s);
    if (!r.ok) return;
    float inset = 2.3f * s;
    float fullW = w - 2 * inset;
    float fillW = max(r.percent > 0 ? 1.5f * s : 0.f, fullW * r.percent / 100.f);
    G::SolidBrush fill(LevelColor(r));
    FillRound(g, fill, {x + inset, y + inset, fillW, h - 2 * inset}, 1.2f * s);
    if (r.charging) DrawBolt(g, x + w / 2, y + h / 2, h + 5 * s, s);
}

std::wstring PercentText(const Reading& r) { return r.ok ? std::to_wstring(r.percent) + L"%" : L"—"; }

struct Fonts {
    G::Font* semi13;
    G::Font* semi15;
    G::Font* reg12;
    G::Font* reg11;
    ~Fonts() {
        delete semi13;
        delete semi15;
        delete reg12;
        delete reg11;
    }
};

G::Font* MakeFont(const wchar_t* family, float px, int style = G::FontStyleRegular) {
    auto* f = new G::Font(family, px, style, G::UnitPixel);
    if (f->GetLastStatus() != G::Ok) {
        delete f;
        f = new G::Font(L"Segoe UI", px, style == G::FontStyleRegular ? G::FontStyleBold : style, G::UnitPixel);
    }
    return f;
}

Fonts MakeFonts(float s) {
    return {MakeFont(L"Segoe UI Semibold", kTextPx * s), MakeFont(L"Segoe UI Semibold", 15 * s),
            MakeFont(L"Segoe UI", 12 * s), MakeFont(L"Segoe UI", 11 * s)};
}

G::RectF Measure(G::Graphics& g, const std::wstring& t, const G::Font& f) {
    G::RectF out;
    g.MeasureString(t.c_str(), (INT)t.size(), &f, G::PointF(0, 0), G::StringFormat::GenericTypographic(), &out);
    return out;
}

void Text(G::Graphics& g, const std::wstring& t, const G::Font& f, G::Color c, float x, float y) {
    G::SolidBrush b(c);
    g.DrawString(t.c_str(), (INT)t.size(), &f, G::PointF(x, y), G::StringFormat::GenericTypographic(), &b);
}

void Setup(G::Graphics& g) {
    g.SetSmoothingMode(G::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(G::PixelOffsetModeHighQuality);
    g.SetTextRenderingHint(G::TextRenderingHintAntiAliasGridFit);
}

float PieceWidth(G::Graphics& g, const DeviceStatus& d, const G::Font& f, float s) {
    return (kIcon + kGapIcon + kBatW + kCap + kGapText) * s + Measure(g, PercentText(d.r), f).Width;
}

// Draws one device (icon, battery, percentage) vertically centred on midY.
void DrawPiece(G::Graphics& g, const DeviceStatus& d, float x, float midY, float s, const Palette& p, const G::Font& f) {
    G::Color iconColor = d.r.ok && !d.stale ? p.fg : p.dim;  // dim icon = asleep, value is the last reading
    DrawDeviceIcon(g, d.kind, x, midY - kIcon * s / 2, kIcon * s, iconColor);
    x += (kIcon + kGapIcon) * s;
    DrawBattery(g, x, midY - kBatH * s / 2, kBatW * s, kBatH * s, s, d.r, p);
    x += (kBatW + kCap + kGapText) * s;
    std::wstring t = PercentText(d.r);
    G::RectF m = Measure(g, t, f);
    G::Color tc = !d.r.ok ? p.dim : IsLow(d.r) ? kRed : p.fg;
    Text(g, t, f, tc, x, midY - m.Height / 2);
}

// 32-bit top-down DIB that GDI+ draws into directly (premultiplied alpha).
struct Canvas {
    HDC dc = nullptr;
    HBITMAP bmp = nullptr;
    HGDIOBJ old = nullptr;
    void* bits = nullptr;
    int w, h;
    Canvas(int w_, int h_) : w(max(w_, 1)), h(max(h_, 1)) {
        BITMAPINFO bi{};
        bi.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB};
        dc = CreateCompatibleDC(nullptr);
        bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        old = SelectObject(dc, bmp);
    }
    ~Canvas() {
        SelectObject(dc, old);
        DeleteObject(bmp);
        DeleteDC(dc);
    }
    bool Push(HWND hwnd, const POINT* dst) {
        SIZE sz{w, h};
        POINT src{0, 0};
        BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        HDC screen = GetDC(nullptr);
        BOOL ok = UpdateLayeredWindow(hwnd, screen, const_cast<POINT*>(dst), &sz, dc, &src, 0, &bf, ULW_ALPHA);
        ReleaseDC(nullptr, screen);
        return ok != FALSE;
    }
};

}  // namespace

void InitGraphics() {
    G::GdiplusStartupInput in;
    G::GdiplusStartup(&g_token, &in, nullptr);
}

void ShutdownGraphics() { G::GdiplusShutdown(g_token); }

int MeasureBand(const std::vector<DeviceStatus>& devs, const Style& st, bool horizontal) {
    g_low = st.lowPercent;
    float s = st.dpi / 96.f;
    size_t n = max<size_t>(devs.size(), 1);
    if (!horizontal) return (int)(26 * s * n + 8 * s);
    G::Bitmap tmp(1, 1, PixelFormat32bppPARGB);
    G::Graphics g(&tmp);
    Setup(g);
    Fonts f = MakeFonts(s);
    float w = 2 * kPad * s;
    for (size_t i = 0; i < devs.size(); ++i) w += PieceWidth(g, devs[i], *f.semi13, s) + (i ? kGapDevices * s : 0);
    if (devs.empty()) w += (kIcon + kGapIcon) * s + Measure(g, L"—", *f.semi13).Width;
    return (int)(w + 1);
}

void RenderBand(HWND band, const std::vector<DeviceStatus>& devs, const Style& st) {
    RECT rc;
    GetClientRect(band, &rc);
    Canvas cv(rc.right, rc.bottom);
    g_low = st.lowPercent;
    float s = st.dpi / 96.f;
    Palette p = Pal(st.light);
    {
        G::Bitmap bmp(cv.w, cv.h, cv.w * 4, PixelFormat32bppPARGB, static_cast<BYTE*>(cv.bits));
        G::Graphics g(&bmp);
        Setup(g);
        g.Clear(G::Color(1, 0, 0, 0));  // alpha 1: invisible but still catches the mouse for hover/clicks
        Fonts f = MakeFonts(s);
        bool horizontal = cv.w > cv.h;
        if (devs.empty()) {
            DeviceStatus none{Kind::Other, L"", L"", false, Link::Wireless, {}};
            DrawPiece(g, none, kPad * s, cv.h / 2.f, s, p, *f.semi13);
        } else if (horizontal) {
            float x = kPad * s;
            for (const auto& d : devs) {
                DrawPiece(g, d, x, cv.h / 2.f, s, p, *f.semi13);
                x += PieceWidth(g, d, *f.semi13, s) + kGapDevices * s;
            }
        } else {
            float y = 4 * s + 13 * s;
            for (const auto& d : devs) {
                DrawPiece(g, d, max(2 * s, (cv.w - PieceWidth(g, d, *f.semi13, s)) / 2), y, s, p, *f.semi13);
                y += 26 * s;
            }
        }
    }
    cv.Push(band, nullptr);
}

namespace {

std::wstring SpanLabel(int64_t span) {
    int h = (int)((span + 1800) / 3600);
    return std::to_wstring(max(h, 1)) + L" sa önce";
}

// Step chart of the battery history over the last `span` seconds (max 24 h).
void DrawHistory(G::Graphics& g, const std::vector<Sample>& hist, int64_t now, G::RectF area, float s,
                 const Palette& p, G::Color color, const Fonts& f) {
    G::Pen grid(p.track, 1);
    for (int i = 0; i <= 2; ++i) {
        float gy = area.Y + area.Height * i / 2.f;
        g.DrawLine(&grid, area.X, gy, area.GetRight(), gy);
    }
    size_t valid = 0;
    for (const auto& smp : hist) valid += smp.percent >= 0;
    if (valid < 2) {
        std::wstring t = L"Geçmiş birikiyor…";
        G::RectF m = Measure(g, t, *f.reg11);
        Text(g, t, *f.reg11, p.dim, area.X + (area.Width - m.Width) / 2, area.Y + (area.Height - m.Height) / 2);
        return;
    }
    int64_t span = std::clamp<int64_t>(now - hist.front().t, 3600, 24 * 3600);
    int64_t t0 = now - span;
    auto X = [&](int64_t t) { return area.X + area.Width * (float)(max(t, t0) - t0) / (float)span; };
    auto Y = [&](int pct) { return area.GetBottom() - area.Height * pct / 100.f; };

    G::SolidBrush fill(G::Color(60, color.GetR(), color.GetG(), color.GetB()));
    G::Pen line(G::Color(230, color.GetR(), color.GetG(), color.GetB()), 1.5f * s);
    line.SetLineJoin(G::LineJoinRound);
    // One filled step line per connected stretch; "not connected" samples break it.
    std::vector<G::PointF> pts;
    auto flush = [&]() {
        if (pts.size() >= 2) {
            std::vector<G::PointF> poly = pts;
            poly.push_back({pts.back().X, area.GetBottom()});
            poly.push_back({pts.front().X, area.GetBottom()});
            g.FillPolygon(&fill, poly.data(), (INT)poly.size());
            g.DrawLines(&line, pts.data(), (INT)pts.size());
        }
        pts.clear();
    };
    for (size_t i = 0; i < hist.size(); ++i) {
        const Sample& smp = hist[i];
        if (i + 1 < hist.size() && hist[i + 1].t < t0) continue;  // superseded before the window starts
        if (smp.percent < 0) {
            flush();
            continue;
        }
        float x = X(smp.t), y = Y(smp.percent);
        if (!pts.empty()) pts.push_back({x, pts.back().Y});  // step
        pts.push_back({x, y});
    }
    if (!pts.empty() && hist.back().percent >= 0) pts.push_back({area.GetRight(), pts.back().Y});
    flush();

    std::wstring l = SpanLabel(span), r = L"şimdi";
    G::RectF rm = Measure(g, r, *f.reg11);
    Text(g, l, *f.reg11, p.dim, area.X, area.GetBottom() + 2 * s);
    Text(g, r, *f.reg11, p.dim, area.GetRight() - rm.Width, area.GetBottom() + 2 * s);
}

const wchar_t* LinkText(Link l) {
    switch (l) {
    case Link::Wired: return L"Kablolu";
    case Link::Bluetooth: return L"Bluetooth";
    default: return L"Kablosuz";
    }
}

}  // namespace

void ShowCard(HWND card, const std::vector<CardRow>& rows, const Style& st, const RECT& anchor,
              const std::wstring& footer, int64_t now) {
    g_low = st.lowPercent;
    float s = st.dpi / 96.f;
    const float pad = 14 * s, rowH = 104 * s, rowGap = 12 * s, iconCol = 30 * s;
    int W = (int)(290 * s);
    size_t n = max<size_t>(rows.size(), 1);
    int H = (int)(pad * 2 + (rows.empty() ? 48 * s : n * rowH + (n - 1) * rowGap) + 30 * s);

    // Above the taskbar (or below it when the taskbar is at the top), clamped to the monitor.
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST), &mi);
    int cx = (anchor.left + anchor.right) / 2;
    POINT pos{cx - W / 2, anchor.top - H - (int)(8 * s)};
    if (pos.y < mi.rcMonitor.top) pos.y = anchor.bottom + (int)(8 * s);
    pos.x = max((int)mi.rcWork.left + 4, min(pos.x, (int)mi.rcWork.right - W - 4));

    Canvas cv(W, H);
    {
        G::Bitmap bmp(cv.w, cv.h, cv.w * 4, PixelFormat32bppPARGB, static_cast<BYTE*>(cv.bits));
        G::Graphics g(&bmp);
        Setup(g);
        g.Clear(G::Color(0, 0, 0, 0));
        Palette p = Pal(st.light);
        G::Color bg = st.light ? G::Color(250, 246, 246, 246) : G::Color(248, 32, 32, 34);
        G::Color border = st.light ? G::Color(40, 0, 0, 0) : G::Color(40, 255, 255, 255);
        G::SolidBrush bgb(bg);
        FillRound(g, bgb, {0.5f, 0.5f, W - 1.f, H - 1.f}, 8 * s);
        G::Pen bp(border, 1);
        StrokeRound(g, bp, {0.5f, 0.5f, W - 1.f, H - 1.f}, 8 * s);

        Fonts f = MakeFonts(s);
        float y = pad;
        if (rows.empty()) {
            Text(g, L"Cihaz bulunamadı", *f.semi13, p.fg, pad, y + 6 * s);
            Text(g, L"Alıcı, kablo ya da Bluetooth bağlı değil", *f.reg12, p.dim, pad, y + 26 * s);
        }
        for (const auto& row : rows) {
            const DeviceStatus& d = row.d;
            float x0 = pad + iconCol, right = W - pad;
            DrawDeviceIcon(g, d.kind, pad, y + 2 * s, 20 * s, d.r.ok && !d.stale ? p.fg : p.dim);
            std::wstring pct = PercentText(d.r);
            G::RectF pm = Measure(g, pct, *f.semi15);
            Text(g, pct, *f.semi15, !d.r.ok ? p.dim : LevelColor(d.r), right - pm.Width, y - 2 * s);
            // Name, shortened with an ellipsis if it would run into the percentage.
            std::wstring name = d.name;
            while (name.size() > 3 && Measure(g, name, *f.semi13).Width > right - x0 - pm.Width - 8 * s)
                name = name.substr(0, name.size() - 2) + L"…";
            Text(g, name, *f.semi13, p.fg, x0, y);

            std::wstring sub = !d.r.ok      ? L"Bağlı değil ya da uykuda"
                               : d.stale      ? L"Uykuda"
                               : d.r.charging ? (d.r.percent >= 100 ? L"Şarj doldu" : L"Şarj oluyor")
                               : IsLow(d.r)   ? L"Pil zayıf, şarj et"
                                              : L"Pil kullanılıyor";
            if (d.stale) {
                time_t tt = (time_t)d.lastSeen;
                tm lt{};
                localtime_s(&lt, &tt);
                wchar_t buf[48];
                swprintf_s(buf, L" · son okuma %02d:%02d", lt.tm_hour, lt.tm_min);
                sub += buf;
            } else if (d.r.ok) {
                sub += std::wstring(L" · ") + LinkText(d.link);
            }
            float subX = x0;
            if (d.r.ok && d.r.charging) {
                DrawBolt(g, x0 + 4 * s, y + 25 * s, 12 * s, s);
                subX += 11 * s;
            }
            Text(g, sub, *f.reg12, p.dim, subX, y + 18 * s);

            G::RectF track(x0, y + 38 * s, right - x0, 5 * s);
            G::SolidBrush tb(p.track);
            FillRound(g, tb, track, 2.5f * s);
            if (d.r.ok && d.r.percent > 0) {
                G::SolidBrush fb(LevelColor(d.r));
                FillRound(g, fb, {track.X, track.Y, max(5 * s, track.Width * d.r.percent / 100.f), track.Height},
                          2.5f * s);
            }
            if (d.r.ok) {
                bool known = !row.timeLeft.empty();
                Text(g, known ? row.timeLeft : L"Kalan süre hesaplanıyor…", *f.reg11, known ? p.fg : p.dim, x0,
                     y + 47 * s);
            }
            DrawHistory(g, row.history, now, {x0, y + 66 * s, right - x0, 22 * s}, s, p,
                        d.r.ok ? LevelColor(d.r) : G::Color(255, 150, 150, 150), f);
            y += rowH + rowGap;
        }
        float fy = H - pad - 14 * s;
        G::Pen sep(p.track, 1);
        g.DrawLine(&sep, pad, fy - 8 * s, W - pad, fy - 8 * s);
        Text(g, footer, *f.reg11, p.dim, pad, fy);
    }
    cv.Push(card, &pos);
    SetWindowPos(card, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

// ------------------------------------------------------------------ keyboard OLED (128x40, 1 bit)

namespace {

constexpr int OW = 128, OH = 40;

G::Font* OledFont(float px, bool numbers) {
    // Bahnschrift (Windows 10 1709+) has crisp, DIN-like digits; Segoe UI is the fallback.
    G::Font* f = numbers ? new G::Font(L"Bahnschrift SemiBold", px, G::FontStyleRegular, G::UnitPixel) : nullptr;
    if (!f || f->GetLastStatus() != G::Ok) {
        delete f;
        f = new G::Font(L"Segoe UI", px, G::FontStyleBold, G::UnitPixel);
    }
    return f;
}

int Segments(const Reading& r, int count) { return r.ok ? (r.percent * count + 99) / 100 : 0; }

bool Animates(const DeviceStatus& d, int count) { return d.r.ok && d.r.charging && !d.stale && Segments(d.r, count) < count; }

void OledBolt(G::Graphics& g, const G::Brush& b, float x, float y, float h) {
    const G::PointF unit[] = {{6.4f, 0}, {0.8f, 9.2f}, {4.8f, 9.2f}, {3.6f, 16}, {9.2f, 6.6f}, {5.2f, 6.6f}};
    float k = h / 16;
    G::PointF pts[6];
    for (int j = 0; j < 6; ++j) pts[j] = {x + unit[j].X * k, y + unit[j].Y * k};
    g.FillPolygon(&b, pts, 6);
}

// Draws text so the top of its digits/capitals lands on capTop (GDI+ positions by line top).
void TextAtCap(G::Graphics& g, const std::wstring& t, const G::Font& f, G::Color c, float x, float capTop) {
    G::FontFamily fam;
    f.GetFamily(&fam);
    int style = f.GetStyle();
    float em = (float)fam.GetEmHeight(style);
    float ascent = f.GetSize() * fam.GetCellAscent(style) / em;
    float cap = f.GetSize() * 0.70f;  // digit height of Bahnschrift / Segoe UI ~ 0.7 em
    Text(g, t, f, c, x, capTop + cap - ascent);
}

// One device in a box: icon + status mark on the left, big number on the right, segment bar below.
void OledTile(G::Graphics& g, const DeviceStatus& d, float x0, float w, bool wide, int phase) {
    const G::Color white(255, 255, 255, 255);
    G::SolidBrush wb(white);
    float numPx = wide ? 31.f : 21.f, capTop = wide ? 3.f : 3.f;
    float numH = numPx * 0.70f;
    float icon = wide ? 20.f : 14.f;
    float iy = (float)(int)(capTop + numH / 2 - icon / 2);
    DrawDeviceIcon(g, d.kind, x0 + 2, iy, icon, white);

    // Status mark next to the icon: charging bolt, "z" when asleep, "!" when low.
    float mx = x0 + 4 + icon;
    std::unique_ptr<G::Font> mark(OledFont(wide ? 15.f : 12.f, false));
    if (d.r.ok && d.r.charging && !d.stale)
        OledBolt(g, wb, mx, iy, icon);
    else if (d.stale)
        TextAtCap(g, L"z", *mark, white, mx, iy + icon - mark->GetSize() * 0.52f);
    else if (IsLow(d.r))
        TextAtCap(g, L"!", *mark, white, mx + 1, iy);

    // Number with a smaller percent sign sharing its baseline.
    std::unique_ptr<G::Font> num(OledFont(numPx, true));
    std::unique_ptr<G::Font> pct(OledFont(wide ? 14.f : 10.f, true));
    std::wstring n = d.r.ok ? std::to_wstring(d.r.percent) : L"--";
    G::RectF nm = Measure(g, n, *num), pm = Measure(g, L"%", *pct);
    float right = x0 + w - 2;
    TextAtCap(g, n, *num, white, right - pm.Width - 1 - nm.Width, capTop);
    if (d.r.ok) TextAtCap(g, L"%", *pct, white, right - pm.Width, capTop + numH - pct->GetSize() * 0.70f);

    // Segment bar: filled = level; checkered when it is the last known value of a sleeping
    // device; while charging the next segment blinks (phase 1).
    int count = wide ? 20 : 10;
    float segW = 5, gap = 1, h = wide ? 7.f : 9.f;
    float top = (float)(int)(capTop + numH + (wide ? 5 : 6));
    float bx = (float)(int)(x0 + (w - (count * (segW + gap) - gap)) / 2);
    int filled = Segments(d.r, count) + (phase == 1 && Animates(d, count) ? 1 : 0);
    for (int i = 0; i < count; ++i) {
        float sx = bx + i * (segW + gap);
        if (i < filled && !d.stale) {
            g.FillRectangle(&wb, sx, top, segW, h);
        } else if (i < filled) {
            for (int yy = 0; yy < (int)h; ++yy)
                for (int xx = (yy & 1); xx < (int)segW; xx += 2) g.FillRectangle(&wb, sx + xx, top + yy, 1.f, 1.f);
        } else {
            g.FillRectangle(&wb, sx, top + h - 1, segW, 1.f);  // empty segment: just a baseline
        }
    }
}

}  // namespace

int OledPhases(const std::vector<DeviceStatus>& devs) {
    int shown = 0;
    for (const auto& d : devs) {
        if (shown++ >= 2) break;
        if (Animates(d, devs.size() == 1 ? 20 : 10)) return 2;
    }
    return 1;
}

std::vector<uint8_t> RenderOled(const std::vector<DeviceStatus>& all, int lowPercent, int phase) {
    g_low = lowPercent;
    std::vector<uint8_t> out(OW * OH / 8, 0);
    G::Bitmap bmp(OW, OH, PixelFormat32bppARGB);
    {
        G::Graphics g(&bmp);
        g.SetSmoothingMode(G::SmoothingModeAntiAlias);  // smoother icon curves; thresholded to 1 bit below
        g.SetPixelOffsetMode(G::PixelOffsetModeHalf);
        g.SetTextRenderingHint(G::TextRenderingHintSingleBitPerPixelGridFit);
        g.Clear(G::Color(255, 0, 0, 0));

        // At most two tiles: devices that answer (or have a last value) first.
        std::vector<const DeviceStatus*> devs;
        for (const auto& d : all)
            if (d.r.ok && devs.size() < 2) devs.push_back(&d);
        for (const auto& d : all)
            if (!d.r.ok && devs.size() < 2) devs.push_back(&d);

        if (devs.empty()) {
            std::unique_ptr<G::Font> f(OledFont(14, false));
            std::wstring t = L"Cihaz yok";
            G::RectF m = Measure(g, t, *f);
            Text(g, t, *f, G::Color(255, 255, 255, 255), (OW - m.Width) / 2, (OH - m.Height) / 2);
        } else if (devs.size() == 1) {
            OledTile(g, *devs[0], 0, OW, true, phase);
        } else {
            OledTile(g, *devs[0], 0, 63, false, phase);
            OledTile(g, *devs[1], 65, 63, false, phase);
            G::SolidBrush wb(G::Color(255, 255, 255, 255));
            for (int y = 3; y < OH - 3; y += 2) g.FillRectangle(&wb, 64, y, 1, 1);  // dotted divider
        }
    }
    for (int y = 0; y < OH; ++y)
        for (int x = 0; x < OW; ++x) {
            G::Color c;
            bmp.GetPixel(x, y, &c);
            if (c.GetR() + c.GetG() + c.GetB() > 3 * 110) out[y * (OW / 8) + x / 8] |= (uint8_t)(0x80 >> (x % 8));
        }
    return out;
}

bool SaveOledPng(const std::vector<uint8_t>& bits, const wchar_t* path, int scale) {
    constexpr int W = 128, H = 40;
    G::Bitmap bmp(W * scale, H * scale, PixelFormat32bppARGB);
    {
        G::Graphics g(&bmp);
        g.Clear(G::Color(255, 10, 10, 10));
        G::SolidBrush on(G::Color(255, 235, 245, 255));
        int dot = scale > 2 ? scale - 1 : scale;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                if (bits[y * (W / 8) + x / 8] & (0x80 >> (x % 8))) g.FillRectangle(&on, x * scale, y * scale, dot, dot);
    }
    UINT num = 0, size = 0;
    G::GetImageEncodersSize(&num, &size);
    std::vector<BYTE> buf(size);
    auto* codecs = reinterpret_cast<G::ImageCodecInfo*>(buf.data());
    G::GetImageEncoders(num, size, codecs);
    for (UINT i = 0; i < num; ++i)
        if (wcscmp(codecs[i].MimeType, L"image/png") == 0) return bmp.Save(path, &codecs[i].Clsid, nullptr) == G::Ok;
    return false;
}

void PaintDeviceIcon(HDC dc, Kind k, float x, float y, float size, COLORREF color) {
    G::Graphics g(dc);
    g.SetSmoothingMode(G::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(G::PixelOffsetModeHighQuality);
    DrawDeviceIcon(g, k, x, y, size, G::Color(255, GetRValue(color), GetGValue(color), GetBValue(color)));
}