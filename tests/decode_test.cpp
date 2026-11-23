// Decoder tests using bytes captured from the real devices (2026-11-23).
#include "../src/battery.h"
#include <cstdio>
#include <cstdint>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

int main() {
    {   // Aerox 5 on cable: 0x8F -> charging, (0x0F-1)*5 = 70
        uint8_t b[65] = {0x00, 0x92, 0x8F};
        Reading r = DecodeAerox(b, sizeof b, 0x92);
        CHECK(r.ok); CHECK(r.percent == 70); CHECK(r.charging);
    }
    {   // Dongle while mouse is on cable: level byte 0xFF -> not connected
        uint8_t b[65] = {0x00, 0x40, 0xFF};
        CHECK(!DecodeAerox(b, sizeof b, 0xD2).ok);
    }
    {   // Echo mismatch -> not our response
        uint8_t b[65] = {0x00, 0x40, 0x15};
        CHECK(!DecodeAerox(b, sizeof b, 0xD2).ok);
    }
    {   // Full battery over dongle, not charging
        uint8_t b[65] = {0x00, 0xD2, 0x15};
        Reading r = DecodeAerox(b, sizeof b, 0xD2);
        CHECK(r.ok); CHECK(r.percent == 100); CHECK(!r.charging);
    }
    {   // Dongle reply captured while GG streamed lighting: 0x11 -> 80%
        uint8_t b[65] = {0x00, 0xD2, 0x11, 0x31, 0x31};
        Reading r = DecodeAerox(b, sizeof b, 0xD2);
        CHECK(r.ok); CHECK(r.percent == 80); CHECK(!r.charging);
    }
    {   // GG lighting ack is not our reply
        uint8_t b[65] = {0x00, 0x61, 0x00};
        CHECK(!DecodeAerox(b, sizeof b, 0xD2).ok);
    }
    {   // Headset charging, captured: B0 03 02 2B 01 -> 43% charging
        uint8_t b[65] = {0x00, 0xB0, 0x03, 0x02, 0x2B, 0x01};
        Reading r = DecodeNova(b, sizeof b);
        CHECK(r.ok); CHECK(r.percent == 43); CHECK(r.charging);
    }
    {   // Raw 0 -> (0-1)*5 = -5 -> invalid
        uint8_t b[65] = {0x00, 0xD2, 0x00};
        CHECK(!DecodeAerox(b, sizeof b, 0xD2).ok);
    }
    {   // Arctis Nova 5 observed: 45%, not charging
        uint8_t b[65] = {0x00, 0xB0, 0x03, 0x02, 0x2D, 0x03};
        Reading r = DecodeNova(b, sizeof b);
        CHECK(r.ok); CHECK(r.percent == 45); CHECK(!r.charging);
    }
    {   // Headset offline
        uint8_t b[65] = {0x00, 0xB0, 0x02, 0x02, 0x2D, 0x03};
        CHECK(!DecodeNova(b, sizeof b).ok);
    }
    {   // Charging flag
        uint8_t b[65] = {0x00, 0xB0, 0x03, 0x02, 0x50, 0x01};
        Reading r = DecodeNova(b, sizeof b);
        CHECK(r.ok); CHECK(r.percent == 80); CHECK(r.charging);
    }
    {   // Out-of-range percent / short buffer
        uint8_t b[65] = {0x00, 0xB0, 0x03, 0x02, 0xC8, 0x03};
        CHECK(!DecodeNova(b, sizeof b).ok);
        CHECK(!DecodeNova(b, 3).ok);
    }
    std::printf(failures ? "%d FAILED\n" : "all tests passed\n", failures);
    return failures ? 1 : 0;
}
