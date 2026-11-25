// SteelBattery: shows SteelSeries wireless battery levels inside the Windows taskbar.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "battery.h"
#include "filter.h"
#include "history.h"
#include "log.h"
#include "oled.h"
#include "render.h"
#include "settings.h"
#include "stats.h"
#include "statswin.h"

namespace {

constexpr UINT WM_APP_RESULT = WM_APP + 1;
constexpr UINT WM_APP_RELAYOUT = WM_APP + 2;
constexpr UINT_PTR TIMER_POLL = 1, TIMER_LAYOUT = 2, TIMER_BALLOON = 3, TIMER_HOVER = 4, TIMER_FASTPOLL = 5,
                   TIMER_SAVE = 6;
constexpr UINT FASTPOLL_MS = 5000;               // a device just fell asleep / a value awaits confirmation
constexpr UINT SLOWPOLL_MS = 15000;              // a device has been asleep for a while
constexpr int64_t kFastPhase = 2 * 60;           // how long the 5 s phase lasts
constexpr ULONGLONG kPollWatchdogMs = 20000;     // a poll that has not reported back by then is abandoned
constexpr int64_t kStaleMaxAge = 24 * 3600;      // keep showing a sleeping device's last value this long
constexpr ULONGLONG HOVER_REFRESH_MS = 10 * 1000;  // hovering refreshes if data is older than this
constexpr UINT ID_REFRESH = 100, ID_AUTOSTART = 101, ID_EXIT = 102, ID_OLED = 103, ID_BLUETOOTH = 104,
               ID_STATS = 105, ID_LOGS = 106;
constexpr UINT ID_LOW_BASE = 200, ID_POLL_BASE = 300;
constexpr int kLowChoices[] = {10, 15, 20, 25};
constexpr int kPollChoices[] = {30, 60, 120, 300};
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"SteelBattery";

HINSTANCE g_inst;
HWND g_owner;  // hidden top-level window: timers, menu, TaskbarCreated
HWND g_band;   // our layered child window inside Shell_TrayWnd
HWND g_card;   // hover card
UINT g_msgTaskbarCreated;
HWINEVENTHOOK g_hook, g_fgHook;
std::vector<DeviceStatus> g_status;
bool g_haveData = false;
ULONGLONG g_lastPoll = 0;
SYSTEMTIME g_lastPollTime{};
bool g_polling = false;
ULONGLONG g_pollStarted = 0;
unsigned g_pollGen = 0;
bool g_relayoutPending = false;
bool g_hovering = false;
bool g_quiet = false;              // a full-screen app (game) is in front: touch no windows
int64_t g_asleepSince = 0;         // when some device started sleeping (0 = none asleep)
std::set<std::wstring> g_warned;   // device keys whose low-battery notification was shown
std::wstring g_pendingWarning;     // low-battery text held back while a game is full screen
bool g_balloonShown = false;
Style g_style;
Settings g_settings;
std::wstring g_dataDir;
std::unique_ptr<History> g_history;
std::unique_ptr<Stats> g_stats;
std::map<std::wstring, Track> g_tracks;  // per device: last accepted reading + pending suspicious one

struct PollResult {
    unsigned gen;
    bool fast;  // fast polls skip the Bluetooth scan
    std::vector<DeviceStatus> devs;
};

std::wstring DataDir() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    std::wstring dir = (n && n < MAX_PATH ? std::wstring(buf) : std::wstring(L".")) + L"\\SteelBattery";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

std::wstring Raw(const Reading& r) {
    wchar_t b[32];
    swprintf_s(b, L"%02X %02X %02X %02X %02X %02X", r.raw[0], r.raw[1], r.raw[2], r.raw[3], r.raw[4], r.raw[5]);
    return b;
}

// ------------------------------------------------------------------ full-screen (game) detection

bool FullscreenAppActive() {
    QUERY_USER_NOTIFICATION_STATE s;
    if (SUCCEEDED(SHQueryUserNotificationState(&s)) &&
        (s == QUNS_BUSY || s == QUNS_RUNNING_D3D_FULL_SCREEN || s == QUNS_PRESENTATION_MODE))
        return true;
    // Borderless "full screen" windows are not always reported above: compare with the monitor.
    HWND fg = GetForegroundWindow();
    if (!fg || fg == g_owner) return false;
    wchar_t cls[64];
    GetClassNameW(fg, cls, 64);
    if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW") || !wcscmp(cls, L"Shell_TrayWnd")) return false;
    RECT r;
    MONITORINFO mi{sizeof mi};
    if (!GetWindowRect(fg, &r) || !GetMonitorInfoW(MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST), &mi)) return false;
    return r.left <= mi.rcMonitor.left && r.top <= mi.rcMonitor.top && r.right >= mi.rcMonitor.right &&
           r.bottom >= mi.rcMonitor.bottom;
}

void HideCard();
void ShowBalloon(const std::wstring& text);

// Updates g_quiet; returns it. While quiet we never move, show or redraw a window, because
// touching the taskbar or a topmost window over a full-screen game can make it lose focus.
bool UpdateQuiet() {
    bool q = FullscreenAppActive();
    if (q != g_quiet) {
        g_quiet = q;
        Log(q ? L"full-screen app in front: UI paused" : L"full-screen app gone: UI resumed");
        if (q) {
            g_hovering = false;
            HideCard();
        } else if (!g_pendingWarning.empty()) {
            std::wstring w = g_pendingWarning;
            g_pendingWarning.clear();
            ShowBalloon(w);
        }
    }
    return g_quiet;
}

// Logs who takes the foreground, so a game losing focus can be traced to the window that took it.
void CALLBACK OnForeground(HWINEVENTHOOK, DWORD, HWND hwnd, LONG, LONG, DWORD, DWORD) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    wchar_t exe[MAX_PATH] = L"?", cls[64] = L"";
    if (HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        DWORD n = MAX_PATH;
        if (QueryFullProcessImageNameW(p, 0, exe, &n)) {
            if (const wchar_t* slash = wcsrchr(exe, L'\\')) wmemmove(exe, slash + 1, wcslen(slash + 1) + 1);
        }
        CloseHandle(p);
    }
    GetClassNameW(hwnd, cls, 64);
    Log(L"foreground -> %s (%s)%s", exe, cls, g_quiet ? L"  [was full screen]" : L"");
}

// ------------------------------------------------------------------ OLED

void PushOled() {
    if (!g_settings.oled || !g_haveData) return;
    std::vector<OledFrame> frames;
    for (int phase = 0, n = OledPhases(g_status); phase < n; ++phase)
        frames.push_back(RenderOled(g_status, g_settings.lowPercent, phase));
    OledSet(std::move(frames));
}

bool LightTaskbar() {
    DWORD v = 0, sz = sizeof v;
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &sz);
    return v == 1;
}

// ------------------------------------------------------------------ taskbar embedding

struct TaskbarParts {
    HWND taskbar, rebar, notify;
};

bool FindTaskbar(TaskbarParts& t) {
    t.taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    t.rebar = t.taskbar ? FindWindowExW(t.taskbar, nullptr, L"ReBarWindow32", nullptr) : nullptr;
    t.notify = t.taskbar ? FindWindowExW(t.taskbar, nullptr, L"TrayNotifyWnd", nullptr) : nullptr;
    return t.rebar && t.notify;
}

RECT RectIn(HWND child, HWND parent) {
    RECT r;
    GetWindowRect(child, &r);
    MapWindowPoints(HWND_DESKTOP, parent, reinterpret_cast<POINT*>(&r), 2);
    return r;
}

// Gives the task-button area its full size back (on exit).
void RestoreRebar() {
    TaskbarParts t;
    if (!FindTaskbar(t)) return;
    RECT bar = RectIn(t.rebar, t.taskbar), note = RectIn(t.notify, t.taskbar), tb;
    GetClientRect(t.taskbar, &tb);
    if (tb.right > tb.bottom)
        MoveWindow(t.rebar, bar.left, bar.top, note.left - bar.left, bar.bottom - bar.top, TRUE);
    else
        MoveWindow(t.rebar, bar.left, bar.top, bar.right - bar.left, note.top - bar.top, TRUE);
}

void RequestRelayout() {
    if (g_relayoutPending) return;  // coalesce: a burst of taskbar events must not flood our queue
    g_relayoutPending = PostMessageW(g_owner, WM_APP_RELAYOUT, 0, 0) != FALSE;
}

void CALLBACK OnTaskbarPartMoved(HWINEVENTHOOK, DWORD, HWND hwnd, LONG idObject, LONG, DWORD, DWORD) {
    TaskbarParts t;
    if (idObject == OBJID_WINDOW && FindTaskbar(t) && (hwnd == t.rebar || hwnd == t.notify)) RequestRelayout();
}

// What the band currently shows; used to skip redundant redraws.
std::wstring BandSignature(const RECT& pos) {
    std::wstring s = std::to_wstring(pos.left) + L"," + std::to_wstring(pos.top) + L"," + std::to_wstring(pos.right) +
                     L"," + std::to_wstring(pos.bottom) + L"|" + std::to_wstring(g_style.dpi) + (g_style.light ? L"L" : L"D") +
                     std::to_wstring(g_style.lowPercent);
    for (const auto& d : g_status)
        s += L"|" + d.key + L":" + std::to_wstring(d.r.ok) + std::to_wstring(d.r.percent) + std::to_wstring(d.r.charging) +
             std::to_wstring(d.stale);
    return s;
}

std::wstring g_bandSig;

// Creates/re-parents the band if needed, squeezes the task-button area to make room, redraws.
void Layout() {
    if (UpdateQuiet()) return;
    TaskbarParts t;
    if (!FindTaskbar(t)) return;

    if (!IsWindow(g_band) || GetParent(g_band) != t.taskbar) {
        if (IsWindow(g_band)) DestroyWindow(g_band);
        g_band = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW, L"SteelBatteryBand", L"", WS_POPUP, 0, 0, 1, 1,
                                 nullptr, nullptr, g_inst, nullptr);
        SetWindowLongPtrW(g_band, GWL_STYLE, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS);
        SetParent(g_band, t.taskbar);
        g_bandSig.clear();
        if (g_hook) UnhookWinEvent(g_hook);
        DWORD pid = 0;
        DWORD tid = GetWindowThreadProcessId(t.taskbar, &pid);
        g_hook = SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, nullptr,
                                 OnTaskbarPartMoved, pid, tid, WINEVENT_OUTOFCONTEXT);
        Log(L"band created in taskbar");
    }

    g_style.dpi = GetDpiForWindow(t.taskbar);
    g_style.light = LightTaskbar();
    g_style.lowPercent = g_settings.lowPercent;
    RECT tb;
    GetClientRect(t.taskbar, &tb);
    RECT bar = RectIn(t.rebar, t.taskbar), note = RectIn(t.notify, t.taskbar);
    bool horizontal = tb.right > tb.bottom;
    int extent = MeasureBand(g_status, g_style, horizontal);

    RECT pos;
    if (horizontal) {
        int want = note.left - bar.left - extent;
        if (want < 0) return;
        if (bar.right - bar.left != want) MoveWindow(t.rebar, bar.left, bar.top, want, bar.bottom - bar.top, TRUE);
        pos = {bar.left + want, 0, bar.left + want + extent, tb.bottom};
    } else {
        int want = note.top - bar.top - extent;
        if (want < 0) return;
        if (bar.bottom - bar.top != want) MoveWindow(t.rebar, bar.left, bar.top, bar.right - bar.left, want, TRUE);
        pos = {0, bar.top + want, tb.right, bar.top + want + extent};
    }
    std::wstring sig = BandSignature(pos);
    if (sig == g_bandSig && IsWindowVisible(g_band)) return;  // nothing changed: no window traffic at all
    g_bandSig = sig;
    SetWindowPos(g_band, HWND_TOP, pos.left, pos.top, pos.right - pos.left, pos.bottom - pos.top,
                 SWP_SHOWWINDOW | SWP_NOACTIVATE);
    RenderBand(g_band, g_status, g_style);
}

// ------------------------------------------------------------------ hover card

std::wstring UsageEstimate(const DeviceStatus& d, int64_t now) {
    // Fallback when the current run is too short: average drain per hour of active use (14 days).
    if (!d.r.ok || d.r.charging) return L"";
    double perHour = g_stats->Summarize(d.key, 14, now).DrainPerHour();
    if (perHour <= 0) return L"";
    Estimate e{true, false, (int)(d.r.percent / perHour * 60)};
    return FormatEstimate(e) + L" kullanım";
}

void UpdateCard() {
    if (!g_hovering || !IsWindow(g_band) || g_quiet) return;
    RECT anchor;
    GetWindowRect(g_band, &anchor);
    std::wstring footer;
    if (!g_haveData) {
        footer = L"Okunuyor…";
    } else {
        wchar_t t[64];
        swprintf_s(t, L"Son güncelleme %02d:%02d  ·  Sağ tık: ayarlar", g_lastPollTime.wHour, g_lastPollTime.wMinute);
        footer = t;
    }
    int64_t now = UnixNow();
    std::vector<CardRow> rows;
    for (const auto& d : g_status) {
        const auto& hist = g_history->Get(d.key);
        std::wstring eta = d.r.ok ? FormatEstimate(EstimateTime(hist, now)) : L"";
        if (eta.empty()) eta = UsageEstimate(d, now);
        rows.push_back({d, eta, hist});
    }
    ShowCard(g_card, rows, g_style, anchor, footer, now);
}

void HideCard() {
    KillTimer(g_owner, TIMER_HOVER);
    if (IsWindowVisible(g_card)) ShowWindow(g_card, SW_HIDE);
}

// ------------------------------------------------------------------ polling, notifications, autostart

void StartPoll(bool fast = false) {
    ULONGLONG nowTick = GetTickCount64();
    if (g_polling) {
        if (nowTick - g_pollStarted < kPollWatchdogMs) return;
        Log(L"poll #%u did not report back in %llu s: abandoned, starting a new one", g_pollGen,
            (nowTick - g_pollStarted) / 1000);
    }
    g_polling = true;
    g_pollStarted = nowTick;
    unsigned gen = ++g_pollGen;
    bool bt = g_settings.bluetooth && !fast;
    HWND owner = g_owner;
    std::thread([gen, fast, bt, owner] {
        auto* res = new PollResult{gen, fast, PollAll(bt)};
        // A full message queue must not lose the result (that left polling stuck before).
        for (int i = 0; i < 50; ++i) {
            if (PostMessageW(owner, WM_APP_RESULT, 0, reinterpret_cast<LPARAM>(res))) return;
            Sleep(100);
        }
        Log(L"poll #%u result could not be delivered (error %lu)", gen, GetLastError());
        delete res;
    }).detach();
}

void ShowBalloon(const std::wstring& text) {
    if (g_quiet) {  // never pop anything over a full-screen game; show it afterwards
        g_pendingWarning = text;
        return;
    }
    NOTIFYICONDATAW nid{sizeof nid};
    nid.hWnd = g_owner;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_INFO;
    nid.hIcon = LoadIconW(nullptr, IDI_WARNING);
    wcscpy_s(nid.szTip, L"SteelBattery");
    wcscpy_s(nid.szInfoTitle, L"Pil zayıf");
    wcsncpy_s(nid.szInfo, text.c_str(), _TRUNCATE);
    nid.dwInfoFlags = NIIF_WARNING;
    Shell_NotifyIconW(g_balloonShown ? NIM_MODIFY : NIM_ADD, &nid);
    g_balloonShown = true;
    SetTimer(g_owner, TIMER_BALLOON, 15000, nullptr);  // remove the temporary tray icon afterwards
}

void RemoveBalloon() {
    if (!g_balloonShown) return;
    NOTIFYICONDATAW nid{sizeof nid};
    nid.hWnd = g_owner;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    g_balloonShown = false;
}

void CheckLow() {
    int lowAt = g_settings.lowPercent;
    for (const auto& d : g_status) {
        if (!d.r.ok || d.stale) continue;  // only warn on live readings
        bool low = !d.r.charging && d.r.percent <= lowAt;
        if (low && !g_warned.count(d.key)) {
            g_warned.insert(d.key);
            Log(L"low battery: %s %d%% raw %s", d.name.c_str(), d.r.percent, Raw(d.r).c_str());
            ShowBalloon(d.name + L": pil %" + std::to_wstring(d.r.percent) + L" kaldı.");
        } else if (d.r.charging || d.r.percent > lowAt + 5) {
            g_warned.erase(d.key);
        }
    }
}

// Applies the filter and the sleep handling to a fresh poll; returns true if a value awaits confirmation.
bool Digest(std::vector<DeviceStatus>& devs, int64_t now) {
    bool held = false;
    for (auto& d : devs) {
        Track& t = g_tracks[d.key];
        if (!t.hasGood) {  // after a restart: last value from the saved history
            const auto& hist = g_history->Get(d.key);
            for (auto s = hist.rbegin(); s != hist.rend(); ++s)
                if (s->percent >= 0) {
                    t.hasGood = true;
                    t.good.ok = true;
                    t.good.percent = s->percent;
                    t.goodT = s->t;
                    break;
                }
        }
        if (d.r.ok) {
            if (FilterReading(t, d.r, now) == Verdict::Accept) {
                g_history->Record(d.key, now, d.r);
                continue;
            }
            Log(L"suspicious reading held: %s %d%% (last good %d%% %llds ago) raw %s", d.name.c_str(), d.r.percent,
                t.good.percent, (long long)(now - t.goodT), Raw(d.r).c_str());
            d.r = t.good;  // keep showing the last believable value until confirmed
            held = true;
            continue;
        }
        // Present but not answering: usually asleep. Show the last known value.
        if (!t.hasGood || now - t.goodT > kStaleMaxAge) continue;
        d.r = t.good;
        d.r.charging = false;
        d.stale = true;
        d.lastSeen = t.goodT;
    }
    return held;
}

std::wstring ExePath() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return buf;
}

bool AutostartEnabled() {
    wchar_t buf[MAX_PATH + 4];
    DWORD sz = sizeof buf;
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ, nullptr, buf, &sz) != ERROR_SUCCESS)
        return false;
    return std::wstring(buf) == L"\"" + ExePath() + L"\"";
}

void SetAutostart(bool on) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) return;
    if (on) {
        std::wstring v = L"\"" + ExePath() + L"\"";
        RegSetValueExW(key, kRunValue, 0, REG_SZ, reinterpret_cast<const BYTE*>(v.c_str()),
                       (DWORD)((v.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, kRunValue);
    }
    RegCloseKey(key);
}

void ShowMenu() {
    HideCard();
    HMENU low = CreatePopupMenu();
    for (int i = 0; i < 4; ++i)
        AppendMenuW(low, MF_STRING | (g_settings.lowPercent == kLowChoices[i] ? MF_CHECKED : 0), ID_LOW_BASE + i,
                    (L"%" + std::to_wstring(kLowChoices[i])).c_str());
    HMENU poll = CreatePopupMenu();
    const wchar_t* pollNames[] = {L"30 saniye", L"1 dakika", L"2 dakika", L"5 dakika"};
    for (int i = 0; i < 4; ++i)
        AppendMenuW(poll, MF_STRING | (g_settings.pollSeconds == kPollChoices[i] ? MF_CHECKED : 0), ID_POLL_BASE + i,
                    pollNames[i]);

    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, ID_REFRESH, L"Şimdi yenile");
    AppendMenuW(m, MF_STRING, ID_STATS, L"Kullanım istatistikleri…");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_POPUP, reinterpret_cast<UINT_PTR>(low), L"Düşük pil uyarısı");
    AppendMenuW(m, MF_POPUP, reinterpret_cast<UINT_PTR>(poll), L"Yenileme sıklığı");
    AppendMenuW(m, MF_STRING | (g_settings.oled ? MF_CHECKED : 0), ID_OLED, L"Klavye ekranında göster");
    AppendMenuW(m, MF_STRING | (g_settings.bluetooth ? MF_CHECKED : 0), ID_BLUETOOTH, L"Bluetooth cihazlarını göster");
    AppendMenuW(m, MF_STRING | (AutostartEnabled() ? MF_CHECKED : 0), ID_AUTOSTART, L"Windows ile başlat");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_LOGS, L"Günlük dosyasını aç");
    AppendMenuW(m, MF_STRING, ID_EXIT, L"Çıkış");
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_owner);
    UINT cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, g_owner, nullptr);
    PostMessageW(g_owner, WM_NULL, 0, 0);
    DestroyMenu(m);  // also destroys the submenus

    if (cmd == ID_REFRESH) StartPoll();
    if (cmd == ID_STATS) {
        g_stats->Save();
        OpenStatsWindow(g_inst, g_stats.get(), g_dataDir);
    }
    if (cmd == ID_LOGS) ShellExecuteW(nullptr, L"open", LogPath().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (cmd == ID_AUTOSTART) SetAutostart(!AutostartEnabled());
    if (cmd == ID_OLED) {
        g_settings.oled = !g_settings.oled;
        OledEnable(g_settings.oled);
        PushOled();
    }
    if (cmd == ID_BLUETOOTH) {
        g_settings.bluetooth = !g_settings.bluetooth;
        StartPoll();
    }
    if (cmd >= ID_LOW_BASE && cmd < ID_LOW_BASE + 4) {
        g_settings.lowPercent = kLowChoices[cmd - ID_LOW_BASE];
        g_warned.clear();
        CheckLow();
        Layout();
        PushOled();
    }
    if (cmd >= ID_POLL_BASE && cmd < ID_POLL_BASE + 4) {
        g_settings.pollSeconds = kPollChoices[cmd - ID_POLL_BASE];
        SetTimer(g_owner, TIMER_POLL, g_settings.pollSeconds * 1000, nullptr);
    }
    if (cmd == ID_OLED || cmd == ID_BLUETOOTH || cmd >= ID_LOW_BASE) SaveSettings(g_settings);
    if (cmd == ID_EXIT) DestroyWindow(g_owner);
}

// ------------------------------------------------------------------ window procs

LRESULT CALLBACK BandProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_MOUSEMOVE:
        if (!g_hovering && !g_quiet) {
            g_hovering = true;
            TRACKMOUSEEVENT tme{sizeof tme, TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tme);
            SetTimer(g_owner, TIMER_HOVER, 300, nullptr);
            if (GetTickCount64() - g_lastPoll > HOVER_REFRESH_MS) StartPoll();
        }
        return 0;
    case WM_MOUSELEAVE:
        g_hovering = false;
        HideCard();
        return 0;
    case WM_RBUTTONUP: PostMessageW(g_owner, WM_CONTEXTMENU, 0, 0); return 0;
    case WM_LBUTTONUP: StartPoll(); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void OnPollResult(HWND hwnd, PollResult* raw) {
    std::unique_ptr<PollResult> res(raw);
    if (res->gen != g_pollGen) {
        Log(L"late result of abandoned poll #%u ignored", res->gen);
        return;
    }
    g_polling = false;
    int64_t now = UnixNow();
    auto& devs = res->devs;
    if (res->fast)  // fast polls skip the Bluetooth scan: keep the Bluetooth devices we already know
        for (const auto& old : g_status)
            if (old.link == Link::Bluetooth) devs.push_back(old);
    // Devices that vanished (unplugged / Bluetooth disconnected) get a gap in their history.
    for (const auto& old : g_status) {
        bool still = false;
        for (const auto& d : devs) still |= d.key == old.key;
        if (!still) g_history->Record(old.key, now, Reading{});
    }
    bool held = Digest(devs, now);
    g_status = std::move(devs);
    g_stats->Update(g_status, now);

    // Poll faster while something sleeps (to catch it waking) or a value awaits confirmation.
    bool anyAsleep = false;
    for (const auto& d : g_status) anyAsleep |= d.stale || !d.r.ok;
    if (!anyAsleep) g_asleepSince = 0;
    if (anyAsleep && !g_asleepSince) g_asleepSince = now;
    if (held || (anyAsleep && now - g_asleepSince < kFastPhase))
        SetTimer(hwnd, TIMER_FASTPOLL, FASTPOLL_MS, nullptr);
    else if (anyAsleep)
        SetTimer(hwnd, TIMER_FASTPOLL, SLOWPOLL_MS, nullptr);
    else
        KillTimer(hwnd, TIMER_FASTPOLL);

    g_haveData = true;
    g_lastPoll = GetTickCount64();
    GetLocalTime(&g_lastPollTime);
    CheckLow();
    Layout();
    UpdateCard();
    PushOled();
}

LRESULT CALLBACK OwnerProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g_msgTaskbarCreated && msg != 0) {  // Explorer restarted: our old band died with it
        Log(L"Explorer restarted: re-embedding");
        g_band = nullptr;
        Layout();
        return 0;
    }
    switch (msg) {
    case WM_APP_RESULT: OnPollResult(hwnd, reinterpret_cast<PollResult*>(lp)); return 0;
    case WM_APP_RELAYOUT:
        g_relayoutPending = false;
        Layout();
        return 0;
    case WM_TIMER:
        if (wp == TIMER_POLL) StartPoll();
        if (wp == TIMER_FASTPOLL) StartPoll(true);
        if (wp == TIMER_LAYOUT) Layout();
        if (wp == TIMER_SAVE) g_stats->Save();
        if (wp == TIMER_HOVER) {
            KillTimer(hwnd, TIMER_HOVER);
            UpdateCard();
        }
        if (wp == TIMER_BALLOON) {
            KillTimer(hwnd, TIMER_BALLOON);
            RemoveBalloon();
        }
        return 0;
    case WM_CONTEXTMENU: ShowMenu(); return 0;
    case WM_SETTINGCHANGE:  // light/dark theme or DPI changed
        g_bandSig.clear();
        RequestRelayout();
        return 0;
    case WM_QUERYENDSESSION: g_stats->Save(); return TRUE;
    case WM_DESTROY:
        Log(L"exit");
        g_stats->Save();
        if (g_hook) UnhookWinEvent(g_hook);
        if (g_fgHook) UnhookWinEvent(g_fgHook);
        g_hook = g_fgHook = nullptr;
        RemoveBalloon();
        OledStop();  // hands the keyboard screen back to GG
        if (IsWindow(g_band)) DestroyWindow(g_band);
        RestoreRebar();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\SteelBatterySingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;
    g_inst = inst;
    g_dataDir = DataDir();
    LogInit(g_dataDir);
    Log(L"start");
    g_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    InitGraphics();
    g_settings = LoadSettings();
    g_history = std::make_unique<History>(g_dataDir + L"\\history.txt");
    g_history->Load(UnixNow());
    g_stats = std::make_unique<Stats>(g_dataDir + L"\\stats.csv");
    g_stats->Load();
    OledEnable(g_settings.oled);
    OledStart();

    WNDCLASSW wc{};
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpfnWndProc = OwnerProc;
    wc.lpszClassName = L"SteelBatteryOwner";
    RegisterClassW(&wc);
    wc.lpfnWndProc = BandProc;
    wc.lpszClassName = L"SteelBatteryBand";
    RegisterClassW(&wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.lpszClassName = L"SteelBatteryCard";
    RegisterClassW(&wc);

    // Hidden top-level (not message-only) so it receives the TaskbarCreated broadcast.
    g_owner = CreateWindowExW(WS_EX_TOOLWINDOW, L"SteelBatteryOwner", L"SteelBattery", WS_POPUP, 0, 0, 0, 0, nullptr,
                              nullptr, inst, nullptr);
    if (!g_owner) Log(L"owner window could not be created (error %lu)", GetLastError());
    // Click-through, never activated, always above the taskbar.
    g_card = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                             L"SteelBatteryCard", L"", WS_POPUP, 0, 0, 1, 1, g_owner, nullptr, inst, nullptr);
    g_fgHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, OnForeground, 0, 0,
                               WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    Layout();
    StartPoll();
    SetTimer(g_owner, TIMER_POLL, g_settings.pollSeconds * 1000, nullptr);
    SetTimer(g_owner, TIMER_LAYOUT, 2000, nullptr);       // fallback in case a location event is missed
    SetTimer(g_owner, TIMER_SAVE, 5 * 60 * 1000, nullptr);  // stats to disk

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    ShutdownGraphics();
    if (mutex) CloseHandle(mutex);
    return 0;
}
