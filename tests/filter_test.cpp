// Tests for the garbage-reading filter and the usage statistics.
#include "../src/filter.h"
#include "../src/stats.h"
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

static Reading R(int pct, bool ch = false) {
    Reading r;
    r.ok = true;
    r.percent = pct;
    r.charging = ch;
    return r;
}

static DeviceStatus Dev(const Reading& r, bool stale = false) {
    DeviceStatus d{Kind::Mouse, L"Aerox 5 Wireless", L"mouse", true, Link::Wireless, r};
    d.stale = stale;
    return d;
}

int main() {
    // ---- filter
    {   // One-off spike is held, then dropped when the next reading is normal again
        Track t;
        CHECK(FilterReading(t, R(75), 0) == Verdict::Accept);
        CHECK(FilterReading(t, R(10), 60) == Verdict::Hold);
        CHECK(t.good.percent == 75);
        CHECK(FilterReading(t, R(75), 65) == Verdict::Accept);
        CHECK(!t.hasPending);
    }
    {   // A real big change is accepted once a second reading confirms it
        Track t;
        FilterReading(t, R(75), 0);
        CHECK(FilterReading(t, R(10), 60) == Verdict::Hold);
        CHECK(FilterReading(t, R(10), 65) == Verdict::Accept);
        CHECK(t.good.percent == 10);
    }
    {   // Normal steps pass straight through
        Track t;
        FilterReading(t, R(75), 0);
        CHECK(FilterReading(t, R(70), 600) == Verdict::Accept);
        CHECK(FilterReading(t, R(85, true), 900) == Verdict::Accept);
    }
    {   // After a long gap anything is believed (charged on another PC, etc.)
        Track t;
        FilterReading(t, R(20), 0);
        CHECK(FilterReading(t, R(100), 3 * 3600) == Verdict::Accept);
    }

    // ---- stats (2026-11-25 12:00 local is not needed: days are keyed by local date of `now`)
    {
        Stats s(L"");
        int64_t t0 = 1795600000;  // arbitrary
        s.Update({Dev(R(80))}, t0);
        s.Update({Dev(R(80))}, t0 + 60);        // 1 min active
        s.Update({Dev(R(75))}, t0 + 120);       // 1 min active, used 5%
        s.Update({Dev(R(75), true)}, t0 + 180); // stale: last minute was active
        s.Update({Dev(R(75), true)}, t0 + 240); // 1 min idle
        s.Update({Dev(R(75, true))}, t0 + 300); // charging starts (1 min idle before)
        s.Update({Dev(R(80, true))}, t0 + 360); // 1 min charging, +5%
        s.Update({Dev(R(80))}, t0 + 5000);      // gap > 10 min: not counted
        auto sum = s.Summarize(L"mouse", 1, t0 + 5000);
        CHECK(sum.activeMin == 3);
        CHECK(sum.idleMin == 2);
        CHECK(sum.chargeMin == 1);
        CHECK(sum.used == 5);
        CHECK(sum.charged == 5);
        CHECK(sum.chargeCount == 1);
    }
    {   // Drain rate: 10% over 2 active hours -> 5%/h -> full charge lasts 20 h
        Stats s(L"");
        int64_t t = 1795600000;
        int pct = 100;
        for (int i = 0; i <= 24; ++i) {  // every 5 min for 2 h, -5% per hour
            if (i == 12 || i == 24) pct -= 5;
            s.Update({Dev(R(pct))}, t + i * 300);
        }
        auto sum = s.Summarize(L"mouse", 7, t + 24 * 300);
        CHECK(sum.activeMin == 120);
        CHECK(sum.used == 10);
        CHECK(sum.DrainPerHour() > 4.99 && sum.DrainPerHour() < 5.01);
    }
    std::printf(failures ? "%d FAILED\n" : "all filter/stats tests passed\n", failures);
    return failures ? 1 : 0;
}
