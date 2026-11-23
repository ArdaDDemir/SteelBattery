#pragma once

// User preferences, stored under HKCU\Software\SteelBattery.
struct Settings {
    int lowPercent = 15;   // warn (red + notification) at or below this
    int pollSeconds = 60;  // refresh interval
    bool oled = true;      // mirror on the keyboard's OLED (Apex 7 / Apex Pro) via SteelSeries GG
    bool bluetooth = true; // also show connected Bluetooth devices that report a battery level
};

Settings LoadSettings();
void SaveSettings(const Settings& s);
