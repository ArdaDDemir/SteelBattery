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
#include "history.h"
#include "oled.h"
#include "render.h"
#include "settings.h"

namespace {

constexpr UINT WM_APP_RESULT = WM_APP + 1;
constexpr UINT WM_APP_RELAYOUT = WM_APP + 2;
constexpr UINT_PTR TIMER_POLL = 1, TIMER_LAYOUT = 2, TIMER_BALLOON = 3, TIMER_HOVER = 4, TIMER_FASTPOLL = 5;
constexpr UINT FASTPOLL_MS = 5000;               // while a device sleeps, look for it waking up this often
constexpr int64_t kStaleMaxAge = 24 * 3600;      // keep showing a sleeping device's last value this long
constexpr ULONGLONG HOVER_REFRESH_MS = 10 * 1000;  // hovering refreshes if data is older than this
constexpr UINT ID_REFRESH = 100, ID_AUTOSTART = 101, ID_EXIT = 102, ID_OLED = 103, ID_BLUETOOTH = 104;
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
HWINEVENTHOOK g_hook;
std::vector<DeviceStatus> g_status;
bool g_haveData = false;
ULONGLONG g_lastPoll = 0;
SYSTEMTIME g_lastPollTime{};
bool g_polling = false;
bool g_hovering = false;
std::set<std::wstring> g_warned;  // device keys whose low-battery notification was shown
bool g_balloonShown = false;
Style g_style;
Settings g_settings;
std::unique_ptr<History> g_history;

struct LastGood {
    Reading r;
    int64_t t;
};
std::map<std::wstring, LastGood> g_lastGood;  // per device key

// A device that is plugged in but not answering is usually just asleep (mice power down after a
// few idle minutes): show its last known value instead of a dash.
void FillAsleep(std::vector<DeviceStatus>& devs, int64_t now) {
    for (auto& d : devs) {
        if (d.r.ok) {
            g_lastGood[d.key] = {d.r, now};
            continue;
        }
        auto it = g_lastGood.find(d.key);
        if (it == g_lastGood.end()) {  // after a restart: fall back to the saved history
            const auto& hist = g_history->Get(d.key);
            for (auto s = hist.rbegin(); s != hist.rend(); ++s)
                if (s->percent >= 0) {
                    Reading r;
                    r.ok = true;
                    r.percent = s->percent;
                    it = g_lastGood.emplace(d.key, LastGood{r, s->t}).first;
                    break;
                }
        }
        if (it == g_lastGood.end() || now - it->second.t > kStaleMaxAge) continue;
        d.r = it->second.r;
        d.r.charging = false;
        d.stale = true;
        d.lastSeen = it->second.t;
    }
}

std::wstring DataDir() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    std::wstring dir = (n && n < MAX_PATH ? std::wstring(buf) : std::wstring(L".")) + L"\\SteelBattery";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

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

void CALLBACK OnTaskbarPartMoved(HWINEVENTHOOK, DWORD, HWND hwnd, LONG idObject, LONG, DWORD, DWORD) {
    TaskbarParts t;
    if (idObject == OBJID_WINDOW && FindTaskbar(t) && (hwnd == t.rebar || hwnd == t.notify))
        PostMessageW(g_owner, WM_APP_RELAYOUT, 0, 0);
}

// Creates/re-parents the band if needed, squeezes the task-button area to make room, redraws.
void Layout() {
    TaskbarParts t;
    if (!FindTaskbar(t)) return;

    if (!IsWindow(g_band) || GetParent(g_band) != t.taskbar) {
        if (IsWindow(g_band)) DestroyWindow(g_band);
        g_band = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW, L"SteelBatteryBand", L"", WS_POPUP, 0, 0, 1, 1,
                                 nullptr, nullptr, g_inst, nullptr);
        SetWindowLongPtrW(g_band, GWL_STYLE, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS);
        SetParent(g_band, t.taskbar);
        if (g_hook) UnhookWinEvent(g_hook);
        DWORD pid = 0;
        DWORD tid = GetWindowThreadProcessId(t.taskbar, &pid);
        g_hook = SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, nullptr,
                                 OnTaskbarPartMoved, pid, tid, WINEVENT_OUTOFCONTEXT);
    }

    g_style.dpi = GetDpiForWindow(t.taskbar);
    g_style.light = LightTaskbar();
    g_style.lowPercent = g_settings.lowPercent;
    RECT tb;
    GetClientRect(t.taskbar, &tb);
    RECT bar = RectIn(t.rebar, t.taskbar), note = RectIn(t.notify, t.taskbar);
    bool horizontal = tb.right > tb.bottom;
    int extent = MeasureBand(g_status, g_style, horizontal);

    if (horizontal) {
        int want = note.left - bar.left - extent;
        if (want < 0) return;
        if (bar.right - bar.left != want) MoveWindow(t.rebar, bar.left, bar.top, want, bar.bottom - bar.top, TRUE);
        SetWindowPos(g_band, HWND_TOP, bar.left + want, 0, extent, tb.bottom, SWP_SHOWWINDOW | SWP_NOACTIVATE);
    } else {
        int want = note.top - bar.top - extent;
        if (want < 0) return;
        if (bar.bottom - bar.top != want) MoveWindow(t.rebar, bar.left, bar.top, bar.right - bar.left, want, TRUE);
        SetWindowPos(g_band, HWND_TOP, 0, bar.top + want, tb.right, extent, SWP_SHOWWINDOW | SWP_NOACTIVATE);
    }
    RenderBand(g_band, g_status, g_style);
}

// ------------------------------------------------------------------ hover card

void UpdateCard() {
    if (!g_hovering || !IsWindow(g_band)) return;
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
        rows.push_back({d, d.r.ok ? FormatEstimate(EstimateTime(hist, now)) : L"", hist});
    }
    ShowCard(g_card, rows, g_style, anchor, footer, now);
}

void HideCard() {
    KillTimer(g_owner, TIMER_HOVER);
    ShowWindow(g_card, SW_HIDE);
}

// ------------------------------------------------------------------ polling, notifications, autostart

void StartPoll() {
    if (g_polling) return;
    g_polling = true;
    bool bt = g_settings.bluetooth;
    std::thread([bt] {
        auto* res = new std::vector<DeviceStatus>(PollAll(bt));
        if (!PostMessageW(g_owner, WM_APP_RESULT, 0, reinterpret_cast<LPARAM>(res))) delete res;
    }).detach();
}

void ShowBalloon(const std::wstring& text) {
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
        if (!d.r.ok) continue;
        bool low = !d.r.charging && d.r.percent <= lowAt;
        if (low && !g_warned.count(d.key)) {
            g_warned.insert(d.key);
            ShowBalloon(d.name + L": pil %" + std::to_wstring(d.r.percent) + L" kaldı.");
        } else if (d.r.charging || d.r.percent > lowAt + 5) {
            g_warned.erase(d.key);
        }
    }
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
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_POPUP, reinterpret_cast<UINT_PTR>(low), L"Düşük pil uyarısı");
    AppendMenuW(m, MF_POPUP, reinterpret_cast<UINT_PTR>(poll), L"Yenileme sıklığı");
    AppendMenuW(m, MF_STRING | (g_settings.oled ? MF_CHECKED : 0), ID_OLED, L"Klavye ekranında göster");
    AppendMenuW(m, MF_STRING | (g_settings.bluetooth ? MF_CHECKED : 0), ID_BLUETOOTH, L"Bluetooth cihazlarını göster");
    AppendMenuW(m, MF_STRING | (AutostartEnabled() ? MF_CHECKED : 0), ID_AUTOSTART, L"Windows ile başlat");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_EXIT, L"Çıkış");
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_owner);
    UINT cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, g_owner, nullptr);
    PostMessageW(g_owner, WM_NULL, 0, 0);
    DestroyMenu(m);  // also destroys the submenus

    if (cmd == ID_REFRESH) StartPoll();
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
        if (!g_hovering) {
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

LRESULT CALLBACK OwnerProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g_msgTaskbarCreated && msg != 0) {  // Explorer restarted: our old band died with it
        g_band = nullptr;
        Layout();
        return 0;
    }
    switch (msg) {
    case WM_APP_RESULT: {
        std::unique_ptr<std::vector<DeviceStatus>> res(reinterpret_cast<std::vector<DeviceStatus>*>(lp));
        int64_t now = UnixNow();
        // Devices that vanished (unplugged / Bluetooth disconnected) get a gap in their history.
        for (const auto& old : g_status) {
            bool still = false;
            for (const auto& d : *res) still |= d.key == old.key;
            if (!still) g_history->Record(old.key, now, Reading{});
        }
        // History keeps real readings only; a sleeping device is neither a value nor a gap.
        for (const auto& d : *res)
            if (d.r.ok) g_history->Record(d.key, now, d.r);
        FillAsleep(*res, now);
        g_status = std::move(*res);
        bool anyAsleep = false;
        for (const auto& d : g_status) anyAsleep |= d.stale || !d.r.ok;
        if (anyAsleep)
            SetTimer(hwnd, TIMER_FASTPOLL, FASTPOLL_MS, nullptr);
        else
            KillTimer(hwnd, TIMER_FASTPOLL);
        g_polling = false;
        g_haveData = true;
        g_lastPoll = GetTickCount64();
        GetLocalTime(&g_lastPollTime);
        CheckLow();
        Layout();
        UpdateCard();
        PushOled();
        return 0;
    }
    case WM_APP_RELAYOUT: Layout(); return 0;
    case WM_TIMER:
        if (wp == TIMER_POLL || wp == TIMER_FASTPOLL) StartPoll();
        if (wp == TIMER_LAYOUT) Layout();
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
        PostMessageW(hwnd, WM_APP_RELAYOUT, 0, 0);
        return 0;
    case WM_DESTROY:
        if (g_hook) UnhookWinEvent(g_hook);
        g_hook = nullptr;
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
    g_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    InitGraphics();
    g_settings = LoadSettings();
    g_history = std::make_unique<History>(DataDir() + L"\\history.txt");
    g_history->Load(UnixNow());
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
    // Click-through, never activated, always above the taskbar.
    g_card = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                             L"SteelBatteryCard", L"", WS_POPUP, 0, 0, 1, 1, g_owner, nullptr, inst, nullptr);
    Layout();
    StartPoll();
    SetTimer(g_owner, TIMER_POLL, g_settings.pollSeconds * 1000, nullptr);
    SetTimer(g_owner, TIMER_LAYOUT, 2000, nullptr);  // fallback in case a location event is missed

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    ShutdownGraphics();
    if (mutex) CloseHandle(mutex);
    return 0;
}
