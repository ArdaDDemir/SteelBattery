# SteelSeries Battery Taskbar Implementation Plan

**Goal:** Tiny Win32 C++ app that embeds `🎧 45%   🖱 70%⚡` into the Windows 10 taskbar.
**Architecture:** `battery.*` = device table + HID query + pure decoders; `main.cpp` = taskbar child window,
polling timer, menu, autostart, low-battery notification. Decoders unit-tested with bytes captured on the real hardware.
**Tech Stack:** C++20, Win32, SetupAPI, hid.dll, GDI+ not needed (GDI + DirectWrite-free). MSVC BuildTools 2022, static CRT.
**Spec:** docs/design.md

## Global Constraints
- VID 0x1038, interface `mi_03`, output report `00 <cmd>` padded to OutputReportByteLength, response byte[1] == cmd.
- Refresh 60 s; low threshold 15%; UI strings Turkish: "Şimdi yenile", "Windows ile başlat", "Çıkış".
- Run key: HKCU\Software\Microsoft\Windows\CurrentVersion\Run, value "SteelBattery".

## Review Focus
1. Mouse switches cable↔dongle while running → shows whichever answers, never stale value (reset each poll).
2. Explorer restarts → window re-embeds on `TaskbarCreated`.
3. Device unplugged mid-read → read times out (≤500 ms), device hidden, no hang of UI thread (poll on worker thread).
4. Dongle answers `FF` level byte → treated as disconnected, not 630%.
5. Taskbar on non-bottom edge / auto-hide → layout check on timer; at least don't crash (vertical taskbars: stack lines).

### Task 1: Decoders + HID query (`src/battery.h`, `src/battery.cpp`, `tests/decode_test.cpp`)
- Produces: `struct Reading { bool ok; int percent; bool charging; };`
  `Reading DecodeAerox(const uint8_t* buf, size_t n, uint8_t cmd);` `Reading DecodeNova(const uint8_t* buf, size_t n);`
  `struct DeviceStatus { const wchar_t* icon; Reading r; };` `std::vector<DeviceStatus> PollAll();`
- Tests: `00 92 8F` → {ok,70,charging}; `00 40 FF` with cmd D2 → !ok; `00 B0 03 02 2D 03` → {ok,45,false};
  `00 B0 02 ...` → !ok; byte[5]==01 → charging; level byte 0x15 → 100.
### Task 2: Taskbar window + app shell (`src/main.cpp`, `build.bat`)
- Child of Shell_TrayWnd, shrink ReBarWindow32 right edge by our width, color-key transparency, Segoe UI 9pt white,
  red ≤15%, right-click menu, autostart toggle, balloon via temporary notify icon once per discharge.
### Task 3: Verify on hardware: build, run, screenshot taskbar, kill Explorer → re-embed.
