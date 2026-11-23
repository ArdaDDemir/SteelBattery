#include "settings.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace {
constexpr wchar_t kKey[] = L"Software\\SteelBattery";

int ReadInt(const wchar_t* name, int def, int lo, int hi) {
    DWORD v = 0, sz = sizeof v;
    if (RegGetValueW(HKEY_CURRENT_USER, kKey, name, RRF_RT_REG_DWORD, nullptr, &v, &sz) != ERROR_SUCCESS) return def;
    int i = (int)v;
    return i < lo || i > hi ? def : i;
}
}  // namespace

Settings LoadSettings() {
    Settings s;
    s.lowPercent = ReadInt(L"LowPercent", s.lowPercent, 5, 50);
    s.pollSeconds = ReadInt(L"PollSeconds", s.pollSeconds, 10, 3600);
    s.oled = ReadInt(L"Oled", s.oled, 0, 1) != 0;
    s.bluetooth = ReadInt(L"Bluetooth", s.bluetooth, 0, 1) != 0;
    return s;
}

void SaveSettings(const Settings& s) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS)
        return;
    auto put = [&](const wchar_t* name, DWORD v) {
        RegSetValueExW(k, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&v), sizeof v);
    };
    put(L"LowPercent", (DWORD)s.lowPercent);
    put(L"PollSeconds", (DWORD)s.pollSeconds);
    put(L"Oled", s.oled ? 1 : 0);
    put(L"Bluetooth", s.bluetooth ? 1 : 0);
    RegCloseKey(k);
}
