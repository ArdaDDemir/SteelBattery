#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct Reading {
    bool ok = false;
    int percent = 0;
    bool charging = false;
};

// buf is a raw Windows HID input report: buf[0] = report ID, buf[1] = echoed command.
Reading DecodeAerox(const uint8_t* buf, size_t n, uint8_t cmd);
Reading DecodeNova(const uint8_t* buf, size_t n);

enum class Kind { Headset, Mouse, Gamepad, Other };

enum class Link { Wireless, Wired, Bluetooth };

struct DeviceStatus {
    Kind kind;
    std::wstring name;
    std::wstring key;  // stable id for history ("headset", "mouse", "bt:<device id>")
    bool present;      // receiver/cable plugged in, or Bluetooth device connected
    Link link;
    Reading r;         // r.ok == false: present but not answering (off, asleep, out of range)
    bool stale = false;     // asleep: r holds the last good reading (filled in by the app, not PollAll)
    int64_t lastSeen = 0;   // unix time of that last good reading
};

// Guesses an icon for a Bluetooth device from its name.
Kind KindFromName(const std::wstring& name);

// SteelSeries devices whose receiver/cable is plugged in (headset first, then mouse), followed by
// connected Bluetooth devices that report a battery level when `bluetooth` is set.
// Blocking (up to ~1 s per device); call off the UI thread.
std::vector<DeviceStatus> PollAll(bool bluetooth);
