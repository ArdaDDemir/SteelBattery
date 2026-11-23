# SteelSeries Battery Taskbar — Design (approved 2026-11-23)

## Intent
Show the battery % of the user's wireless SteelSeries devices directly in the Windows taskbar,
so the mouse/headset never dies mid-game. Replacement for a deleted open-source tool.

## Goals
- Text embedded in the taskbar, left of the tray: `🎧 45%   🖱 70%⚡`
- ⚡ while charging; red text at ≤15% plus a one-time Windows notification per discharge.
- Aerox 5 Wireless read over cable or 2.4 GHz dongle, whichever answers; disconnected devices hidden.
- Refresh every 60 s. Right-click menu: Şimdi yenile / Windows ile başlat / Çıkış.
- Survives Explorer restart and taskbar resize.

## Non-goals
Installer, settings window, other brands, Windows 11 testing (user is on Windows 10 22H2).

## Approaches
- **Embedded taskbar child window (chosen by user)** — matches the reference screenshot.
- Tray icons with the number drawn in — simpler but tiny digits; user declined.
- Floating topmost window over the taskbar — breaks with fullscreen games / taskbar moves.

## Protocols (verified on the user's hardware 2026-11-23, SteelSeries GG running)
All: VID 0x1038, HID interface 3 (usage page 0xFFC0), 65-byte output report `00 <cmd>`,
read 65-byte input report whose byte[1] echoes `<cmd>`. Raw Windows buffers include report ID at [0].
- Aerox 5 Wireless wired: PID 0x1854 (also 0x185E, 0x1862), cmd 0x92.
  Wireless dongle: PID 0x1852 (also 0x185C, 0x1860), cmd 0xD2.
  byte[2]: bit7 = charging, level = ((b & 0x7F) - 1) * 5. Outside 0..100 → not connected (dongle answers 0xFF).
  Source: rivalcfg. Observed: `92 8F` → 70% charging; dongle `40 FF` while mouse is on cable.
- Arctis Nova 5: PID 0x2232 (also 0x2253 Nova 5X), cmd 0xB0.
  byte[2] == 0x02 → headset offline; byte[4] = percent; byte[5] == 0x01 → charging.
  Source: HeadsetControl. Observed: `B0 03 02 2D 03` → 45%, not charging.

## Builder's calls
Single C++ file, Win32 + SetupAPI/HID, no deps, MSVC static CRT. Child of Shell_TrayWnd,
shrinks ReBarWindow32 to make room, colour-key transparency. Autostart via HKCU Run.
Device table is data-driven so more SteelSeries models can be added.
