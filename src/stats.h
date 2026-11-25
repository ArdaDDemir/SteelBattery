#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "battery.h"

// Daily usage per device, kept forever in a small CSV (stats.csv, ';'-separated, opens in Excel).
struct DayStat {
    std::wstring name;
    double activeMin = 0;  // awake and on battery
    double idleMin = 0;    // asleep (receiver plugged in, device not answering)
    double chargeMin = 0;
    int used = 0;          // battery percent consumed
    int charged = 0;       // battery percent gained
    int chargeCount = 0;   // times charging started
};

struct StatSummary {
    double activeMin = 0, idleMin = 0, chargeMin = 0;
    int used = 0, charged = 0, chargeCount = 0;
    double DrainPerHour() const { return activeMin >= 30 && used > 0 ? used / (activeMin / 60.0) : 0; }
    // Hours a full charge lasts in active use, 0 if unknown.
    double FullChargeHours() const { return DrainPerHour() > 0 ? 100.0 / DrainPerHour() : 0; }
};

class Stats {
public:
    explicit Stats(std::wstring file) : file_(std::move(file)) {}
    void Load();
    void Save() const;
    // Attributes the time since the previous update to the state each device was in then, and
    // the percent change between real readings to used/charged. Gaps over 10 min (PC off) are skipped.
    void Update(const std::vector<DeviceStatus>& devs, int64_t now);
    StatSummary Summarize(const std::wstring& key, int days, int64_t now) const;
    // Active minutes per day for the last `days` days, oldest first.
    std::vector<double> DailyActive(const std::wstring& key, int days, int64_t now) const;
    std::vector<std::wstring> Keys() const;  // devices with any data
    std::wstring NameOf(const std::wstring& key) const;
    const std::wstring& File() const { return file_; }

private:
    struct Prev {
        int64_t t = 0;
        int state = 0;  // 0 idle, 1 active, 2 charging
        bool charging = false;
    };
    std::wstring file_;
    std::map<std::pair<int, std::wstring>, DayStat> days_;  // (yyyymmdd, key)
    std::map<std::wstring, Prev> prev_;
    std::map<std::wstring, int> lastReal_;  // last real percent per key
};

int LocalDay(int64_t unixTime);  // yyyymmdd in local time
