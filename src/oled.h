#pragma once
#include <cstdint>
#include <vector>

using OledFrame = std::vector<uint8_t>;  // 640 bytes from RenderOled

// Mirrors images on the keyboard OLED (Apex 7 / Apex Pro, 128x40) through SteelSeries GG's
// GameSense API. Runs on its own thread. One frame is re-sent every 10 s so GG keeps showing it;
// several frames are cycled as an animation (0.7 s each).
void OledStart();
void OledSet(std::vector<OledFrame> frames);
void OledEnable(bool on);  // off: GG gets the screen back (game removed)
void OledStop();
