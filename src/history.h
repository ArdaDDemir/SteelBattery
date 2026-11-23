#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "battery.h"

// One recorded state. percent < 0 means "not connected" (a gap in the graph).
struct Sample {
    int64_t t;  // unix seconds
    int percent;
    bool charging;
};

struct Estimate {
    bool known = false;
    bool charging = false;
    int minutes = 0;  // time to empty (discharging) or to full (charging)
};

// Estimates from the current charge/discharge run: the rate between the first and last
// percentage *changes* in that run (devices report in steps, e.g. 5%). Needs >= 2% and >= 10 min.
Estimate EstimateTime(const std::vector<Sample>& samples, int64_t now);
// "~7 sa 20 dk kaldı", "Dolmasına ~40 dk", "Dolu", or "" when unknown.
std::wstring FormatEstimate(const Estimate& e);

// Battery history per device, persisted as tab-separated lines (key, t, percent, charging).
class History {
public:
    explicit History(std::wstring file) : file_(std::move(file)) {}
    void Load(int64_t now);  // keeps the last 48 hours
    // Appends when the state changes, or every 10 minutes so the graph and estimate keep up.
    void Record(const std::wstring& key, int64_t t, const Reading& r);
    const std::vector<Sample>& Get(const std::wstring& key) const;

private:
    std::wstring file_;
    std::map<std::wstring, std::vector<Sample>> data_;
};

int64_t UnixNow();
