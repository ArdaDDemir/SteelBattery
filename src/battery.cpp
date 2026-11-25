#include "battery.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <devpropdef.h>
#include <winioctl.h>
#include <hidclass.h>

#include <algorithm>
#include <cwctype>
#include <string>

// ---------------------------------------------------------------- decoders

Reading DecodeAerox(const uint8_t* buf, size_t n, uint8_t cmd) {
    // Protocol from rivalcfg: bit7 = charging, level = ((b & 0x7F) - 1) * 5.
    Reading r;
    if (n < 3 || buf[1] != cmd) return r;
    int level = ((buf[2] & 0x7F) - 1) * 5;
    if (level < 0 || level > 100) return r;  // dongle answers 0xFF when the mouse isn't on it
    r.ok = true;
    r.percent = level;
    r.charging = (buf[2] & 0x80) != 0;
    return r;
}

Reading DecodeNova(const uint8_t* buf, size_t n) {
    // Protocol from HeadsetControl: [2]==0x02 offline, [4] = percent, [5]==0x01 charging.
    Reading r;
    if (n < 6 || buf[1] != 0xB0) return r;
    if (buf[2] == 0x02) return r;
    if (buf[4] > 100) return r;
    r.ok = true;
    r.percent = buf[4];
    r.charging = buf[5] == 0x01;
    return r;
}

// ---------------------------------------------------------------- device table

namespace {

constexpr uint16_t kVid = 0x1038;

struct Spec {
    Kind kind;
    uint16_t pid;
    uint8_t cmd;
    bool wired;
    const wchar_t* name;
};

// Order matters: for each kind the first spec that answers wins (mouse: cable before dongle).
constexpr Spec kSpecs[] = {
    {Kind::Headset, 0x2232, 0xB0, false, L"Arctis Nova 5"},      // base station
    {Kind::Headset, 0x2253, 0xB0, false, L"Arctis Nova 5X"},     // base station
    {Kind::Mouse, 0x1854, 0x92, true, L"Aerox 5 Wireless"},      // cable
    {Kind::Mouse, 0x185E, 0x92, true, L"Aerox 5 Wireless"},      //   Destiny 2 edition
    {Kind::Mouse, 0x1862, 0x92, true, L"Aerox 5 Wireless"},      //   Diablo IV edition
    {Kind::Mouse, 0x1852, 0xD2, false, L"Aerox 5 Wireless"},     // 2.4 GHz dongle
    {Kind::Mouse, 0x185C, 0xD2, false, L"Aerox 5 Wireless"},     //   Destiny 2 edition
    {Kind::Mouse, 0x1860, 0xD2, false, L"Aerox 5 Wireless"},     //   Diablo IV edition
};

std::vector<std::wstring> HidPaths() {
    std::vector<std::wstring> out;
    GUID guid;
    HidD_GetHidGuid(&guid);
    HDEVINFO set = SetupDiGetClassDevsW(&guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return out;
    SP_DEVICE_INTERFACE_DATA did{sizeof did};
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(set, nullptr, &guid, i, &did); ++i) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &did, nullptr, 0, &need, nullptr);
        if (!need) continue;
        std::vector<BYTE> mem(need);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(mem.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (SetupDiGetDeviceInterfaceDetailW(set, &did, detail, need, nullptr, nullptr)) {
            std::wstring p = detail->DevicePath;
            std::transform(p.begin(), p.end(), p.begin(), [](wchar_t c) { return (wchar_t)std::towlower(c); });
            out.push_back(std::move(p));
        }
    }
    SetupDiDestroyDeviceInfoList(set);
    return out;
}

// Waits for an overlapped op; cancels it on timeout. Returns bytes transferred or -1.
long Finish(HANDLE h, OVERLAPPED& ov, BOOL started, DWORD timeoutMs) {
    if (!started && GetLastError() != ERROR_IO_PENDING) return -1;
    DWORD got = 0;
    if (WaitForSingleObject(ov.hEvent, timeoutMs) != WAIT_OBJECT_0) {
        CancelIo(h);
        GetOverlappedResult(h, &ov, &got, TRUE);  // wait for cancellation before buffers go away
        return -1;
    }
    if (!GetOverlappedResult(h, &ov, &got, FALSE)) return -1;
    return (long)got;
}

Reading Query(const std::wstring& path, const Spec& s) {
    Reading r;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (h == INVALID_HANDLE_VALUE) return r;

    HIDP_CAPS caps{};
    PHIDP_PREPARSED_DATA pp = nullptr;
    if (HidD_GetPreparsedData(h, &pp)) {
        HidP_GetCaps(pp, &caps);
        HidD_FreePreparsedData(pp);
    }
    if (caps.OutputReportByteLength < 2 || caps.InputReportByteLength < 6) {
        CloseHandle(h);
        return r;
    }

    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HidD_FlushQueue(h);

    std::vector<uint8_t> out(caps.OutputReportByteLength, 0);
    out[1] = s.cmd;
    // SET_REPORT over the control pipe. The Aerox dongle ignores interrupt writes while SteelSeries GG
    // streams lighting to it; fall back to WriteFile for anything that rejects SET_REPORT.
    // Same request HidD_SetOutputReport makes, but overlapped so a receiver that never answers
    // cannot hang the poll (HidD_SetOutputReport has no timeout).
    DWORD unused = 0;
    bool sent = Finish(h, ov,
                       DeviceIoControl(h, IOCTL_HID_SET_OUTPUT_REPORT, out.data(), (DWORD)out.size(), nullptr, 0, &unused, &ov),
                       500) >= 0;
    ResetEvent(ov.hEvent);
    if (!sent) sent = Finish(h, ov, WriteFile(h, out.data(), (DWORD)out.size(), nullptr, &ov), 500) >= 0;
    if (sent) {
        std::vector<uint8_t> in(caps.InputReportByteLength);
        ULONGLONG deadline = GetTickCount64() + 1000;
        // Devices push unrelated input reports (e.g. GG lighting acks); skip until our command's echo.
        for (ULONGLONG now; (now = GetTickCount64()) < deadline;) {
            ResetEvent(ov.hEvent);
            std::fill(in.begin(), in.end(), uint8_t{0});
            BOOL ok = ReadFile(h, in.data(), (DWORD)in.size(), nullptr, &ov);
            long n = Finish(h, ov, ok, (DWORD)(deadline - now));
            if (n < 0) break;
            if (n >= 2 && in[1] == s.cmd) {
                r = s.kind == Kind::Headset ? DecodeNova(in.data(), (size_t)n) : DecodeAerox(in.data(), (size_t)n, s.cmd);
                for (size_t i = 0; i < 6 && i < (size_t)n; ++i) r.raw[i] = in[i];
                break;
            }
        }
    }
    CloseHandle(ov.hEvent);
    CloseHandle(h);
    return r;
}

}  // namespace

// ---------------------------------------------------------------- Bluetooth (Windows battery property)

namespace {

// Battery level Windows reads from Bluetooth devices (what Settings > Bluetooth shows).
const DEVPROPKEY kBtBattery = {{0x104EA319, 0x6EE2, 0x4701, {0xBD, 0x47, 0x8D, 0xDB, 0xF4, 0x25, 0xBB, 0xE5}}, 2};
// True while the device is actually connected (paired-but-off devices keep a stale battery value).
const DEVPROPKEY kBtConnected = {{0x83DA6326, 0x97A6, 0x4088, {0x94, 0x53, 0xA1, 0x92, 0x3F, 0x57, 0x3B, 0x29}}, 15};
const DEVPROPKEY kFriendlyName = {{0xA45C254E, 0xDF1C, 0x4EFD, {0x80, 0x20, 0x67, 0xD1, 0x46, 0xA8, 0x50, 0xE0}}, 14};

bool Contains(const std::wstring& hay, const std::wstring& needle) {
    std::wstring a = hay, b = needle;
    for (auto* s : {&a, &b}) std::transform(s->begin(), s->end(), s->begin(), [](wchar_t c) { return (wchar_t)std::towlower(c); });
    return !b.empty() && a.find(b) != std::wstring::npos;
}

void AddBluetooth(std::vector<DeviceStatus>& out) {
    size_t steelCount = out.size();
    for (const wchar_t* enumerator : {L"BTHLE", L"BTHENUM"}) {
        HDEVINFO set = SetupDiGetClassDevsW(nullptr, enumerator, nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
        if (set == INVALID_HANDLE_VALUE) continue;
        SP_DEVINFO_DATA dev{sizeof dev};
        for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &dev); ++i) {
            DEVPROPTYPE type;
            BYTE level = 0;
            if (!SetupDiGetDevicePropertyW(set, &dev, &kBtBattery, &type, &level, sizeof level, nullptr, 0) ||
                type != DEVPROP_TYPE_BYTE || level > 100)
                continue;
            DEVPROP_BOOLEAN connected = DEVPROP_FALSE;
            if (!SetupDiGetDevicePropertyW(set, &dev, &kBtConnected, &type, reinterpret_cast<BYTE*>(&connected),
                                           sizeof connected, nullptr, 0) ||
                connected != DEVPROP_TRUE)
                continue;
            wchar_t name[256] = L"";
            SetupDiGetDevicePropertyW(set, &dev, &kFriendlyName, &type, reinterpret_cast<BYTE*>(name), sizeof name,
                                      nullptr, 0);
            wchar_t id[512] = L"";
            SetupDiGetDeviceInstanceIdW(set, &dev, id, 512, nullptr);
            bool dup = false;  // same device seen via another node, or a SteelSeries device already read over USB
            for (size_t k = 0; k < out.size(); ++k)
                if (out[k].name == name || (k < steelCount && (Contains(name, out[k].name) || Contains(out[k].name, name))))
                    dup = true;
            if (dup || !name[0]) continue;
            Reading r;
            r.ok = true;
            r.percent = level;
            out.push_back({KindFromName(name), name, std::wstring(L"bt:") + id, true, Link::Bluetooth, r});
        }
        SetupDiDestroyDeviceInfoList(set);
    }
}

}  // namespace

Kind KindFromName(const std::wstring& name) {
    for (const wchar_t* w : {L"controller", L"gamepad", L"xbox", L"dualsense", L"dualshock", L"joy-con", L"pro controller"})
        if (Contains(name, w)) return Kind::Gamepad;
    for (const wchar_t* w : {L"head", L"bud", L"airpods", L"arctis", L"earphone", L"wh-", L"wf-", L"audio", L"hands-free"})
        if (Contains(name, w)) return Kind::Headset;
    for (const wchar_t* w : {L"mouse", L"aerox", L"mx ", L"pebble"})
        if (Contains(name, w)) return Kind::Mouse;
    return Kind::Other;
}

std::vector<DeviceStatus> PollAll(bool bluetooth) {
    std::vector<DeviceStatus> result;
    auto paths = HidPaths();
    for (Kind kind : {Kind::Headset, Kind::Mouse}) {
        DeviceStatus st{kind, L"", kind == Kind::Headset ? L"headset" : L"mouse", false, Link::Wireless, {}};
        for (const Spec& s : kSpecs) {
            if (s.kind != kind) continue;
            wchar_t key[40];
            swprintf_s(key, L"vid_%04x&pid_%04x&mi_03", kVid, s.pid);
            for (const auto& p : paths) {
                if (p.find(key) == std::wstring::npos) continue;
                if (!st.present) st.name = s.name;
                st.present = true;
                Reading r = Query(p, s);
                if (r.ok) {
                    st.name = s.name;
                    st.link = s.wired ? Link::Wired : Link::Wireless;
                    st.r = r;
                    break;
                }
            }
            if (st.r.ok) break;
        }
        if (st.present) result.push_back(st);
    }
    if (bluetooth) AddBluetooth(result);
    return result;
}
