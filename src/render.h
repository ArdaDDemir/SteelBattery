#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

#include "battery.h"
#include "history.h"

struct Style {
    UINT dpi = 96;
    bool light = false;  // light taskbar theme
    int lowPercent = 15;
};

// Band = the strip inside the taskbar. Horizontal: returns the width it needs; vertical: the height.
int MeasureBand(const std::vector<DeviceStatus>& devs, const Style& st, bool horizontal);
// Draws with per-pixel alpha and pushes it to the layered child window (UpdateLayeredWindow).
void RenderBand(HWND band, const std::vector<DeviceStatus>& devs, const Style& st);

struct CardRow {
    DeviceStatus d;
    std::wstring timeLeft;         // "" when unknown
    std::vector<Sample> history;   // last 24 h
};

// Hover card shown above the band. anchor = band rectangle in screen coordinates.
void ShowCard(HWND card, const std::vector<CardRow>& rows, const Style& st, const RECT& anchor,
              const std::wstring& footer, int64_t now);

// 128x40 one-bit image for the keyboard OLED: 640 bytes, rows top to bottom, MSB = leftmost pixel.
// phase 1 = charging animation frame (next segment lit); OledPhases says how many frames there are.
std::vector<uint8_t> RenderOled(const std::vector<DeviceStatus>& devs, int lowPercent, int phase = 0);
int OledPhases(const std::vector<DeviceStatus>& devs);
// Debug helper: saves an OLED image scaled up as PNG.
bool SaveOledPng(const std::vector<uint8_t>& bits, const wchar_t* path, int scale);

// Draws one of the device icons onto any DC (used by the statistics window).
void PaintDeviceIcon(HDC dc, Kind k, float x, float y, float size, COLORREF color);

void InitGraphics();
void ShutdownGraphics();
