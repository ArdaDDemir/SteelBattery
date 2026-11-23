// Tests for the time-left estimate.
#include "../src/history.h"
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

// The app re-records an unchanged state every 10 minutes while it runs; mimic that.
static std::vector<Sample> Heartbeats(const std::vector<Sample>& in) {
    std::vector<Sample> out;
    for (size_t i = 0; i < in.size(); ++i) {
        if (i) for (int64_t t = out.back().t + 600; t < in[i].t; t += 600) out.push_back({t, out.back().percent, out.back().charging});
        out.push_back(in[i]);
    }
    return out;
}

int main() {
    {   // Discharging 5% per hour, 65% left -> 13 h
        std::vector<Sample> s = Heartbeats({{0, 80, false}, {3600, 75, false}, {7200, 70, false}, {10800, 65, false}});
        Estimate e = EstimateTime(s, 10800);
        CHECK(e.known); CHECK(!e.charging); CHECK(e.minutes == 780);
        CHECK(FormatEstimate(e) == L"~13 sa kaldı");
    }
    {   // Time since the last step is subtracted: 30 min later -> 12.5 h
        std::vector<Sample> s = Heartbeats({{0, 80, false}, {3600, 75, false}, {7200, 70, false}, {10800, 65, false}, {12600, 65, false}});
        Estimate e = EstimateTime(s, 10800 + 1800);
        CHECK(e.known); CHECK(e.minutes == 750);
    }
    {   // Charging 5% per 10 min at 50% -> 100 min to full
        std::vector<Sample> s = {{0, 40, true}, {600, 45, true}, {1200, 50, true}};
        Estimate e = EstimateTime(s, 1200);
        CHECK(e.known); CHECK(e.charging); CHECK(e.minutes == 100);
        CHECK(FormatEstimate(e) == L"Dolmasına ~1 sa 40 dk");
    }
    {   // Only one step so far -> unknown
        std::vector<Sample> s = {{0, 80, false}, {3600, 75, false}};
        CHECK(!EstimateTime(s, 3600).known);
        CHECK(FormatEstimate(EstimateTime(s, 3600)) == L"");
    }
    {   // Only the current run counts: charging samples before unplugging are ignored
        std::vector<Sample> s = {{0, 50, true}, {600, 60, true}, {1200, 70, true}, {1800, 70, false}, {5400, 65, false}};
        CHECK(!EstimateTime(s, 5400).known);
    }
    {   // A gap over 30 min (PC asleep) breaks the run
        std::vector<Sample> s = Heartbeats({{0, 80, false}, {3600, 75, false}});
        for (const Sample& x : Heartbeats({{20000, 70, false}, {23600, 65, false}})) s.push_back(x);  // no beats in the gap
        CHECK(!EstimateTime(s, 23600).known);  // the step at 20000 happened during the gap: unreliable
        for (int64_t t = 24200; t < 27200; t += 600) s.push_back({t, 65, false});
        s.push_back({27200, 60, false});
        Estimate e = EstimateTime(s, 27200);
        CHECK(e.known); CHECK(e.minutes == 720);  // 65 -> 60 over 1 h, 60% left
    }
    {   // Disconnected last sample -> unknown
        std::vector<Sample> s = {{0, 80, false}, {3600, 75, false}, {7200, 70, false}, {7300, -1, false}};
        CHECK(!EstimateTime(s, 7300).known);
    }
    {   // Full while charging
        std::vector<Sample> s = {{0, 95, true}, {600, 100, true}};
        Estimate e = EstimateTime(s, 600);
        CHECK(e.known); CHECK(e.minutes == 0); CHECK(FormatEstimate(e) == L"Dolu");
    }
    {   // Under an hour
        Estimate e{true, false, 42};
        CHECK(FormatEstimate(e) == L"~42 dk kaldı");
    }
    std::printf(failures ? "%d FAILED\n" : "all history tests passed\n", failures);
    return failures ? 1 : 0;
}
