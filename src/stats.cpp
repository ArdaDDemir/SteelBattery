#include "stats.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <ctime>
#include <set>
#include <sstream>

namespace {
constexpr int64_t kMaxGap = 10 * 60;

int64_t DayStart(int day) {
    tm t{};
    t.tm_year = day / 10000 - 1900;
    t.tm_mon = day / 100 % 100 - 1;
    t.tm_mday = day % 100;
    t.tm_hour = 12;  // noon: safe against DST shifts
    t.tm_isdst = -1;
    return (int64_t)mktime(&t);
}

std::string ToUtf8(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring FromUtf8(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}
}  // namespace

int LocalDay(int64_t unixTime) {
    time_t tt = (time_t)unixTime;
    tm lt{};
    localtime_s(&lt, &tt);
    return (lt.tm_year + 1900) * 10000 + (lt.tm_mon + 1) * 100 + lt.tm_mday;
}

void Stats::Update(const std::vector<DeviceStatus>& devs, int64_t now) {
    int day = LocalDay(now);
    for (const auto& d : devs) {
        bool real = d.r.ok && !d.stale;
        int state = !real ? 0 : d.r.charging ? 2 : 1;
        DayStat& ds = days_[{day, d.key}];
        if (!d.name.empty()) ds.name = d.name;

        auto p = prev_.find(d.key);
        if (p != prev_.end()) {
            int64_t dt = now - p->second.t;
            if (dt > 0 && dt <= kMaxGap) {
                double min = dt / 60.0;
                (p->second.state == 1 ? ds.activeMin : p->second.state == 2 ? ds.chargeMin : ds.idleMin) += min;
            }
            if (state == 2 && !p->second.charging) ++ds.chargeCount;
        }
        if (real) {
            auto lr = lastReal_.find(d.key);
            if (lr != lastReal_.end()) {
                int delta = d.r.percent - lr->second;
                if (delta < 0 && !d.r.charging) ds.used += -delta;
                if (delta > 0) ds.charged += delta;
            }
            lastReal_[d.key] = d.r.percent;
        }
        prev_[d.key] = {now, state, state == 2};
    }
}

StatSummary Stats::Summarize(const std::wstring& key, int days, int64_t now) const {
    StatSummary s;
    int64_t from = DayStart(LocalDay(now)) - (int64_t)(days - 1) * 86400;
    int firstDay = LocalDay(from);
    for (const auto& [k, ds] : days_) {
        if (k.second != key || k.first < firstDay) continue;
        s.activeMin += ds.activeMin;
        s.idleMin += ds.idleMin;
        s.chargeMin += ds.chargeMin;
        s.used += ds.used;
        s.charged += ds.charged;
        s.chargeCount += ds.chargeCount;
    }
    return s;
}

std::vector<double> Stats::DailyActive(const std::wstring& key, int days, int64_t now) const {
    std::vector<double> out;
    int64_t noon = DayStart(LocalDay(now));
    for (int i = days - 1; i >= 0; --i) {
        auto it = days_.find({LocalDay(noon - (int64_t)i * 86400), key});
        out.push_back(it == days_.end() ? 0 : it->second.activeMin);
    }
    return out;
}

std::vector<std::wstring> Stats::Keys() const {
    std::set<std::wstring> keys;
    for (const auto& [k, ds] : days_) keys.insert(k.second);
    std::vector<std::wstring> out(keys.begin(), keys.end());
    // SteelSeries devices first (headset, mouse), Bluetooth after.
    std::stable_partition(out.begin(), out.end(), [](const std::wstring& k) { return k.rfind(L"bt:", 0) != 0; });
    return out;
}

std::wstring Stats::NameOf(const std::wstring& key) const {
    for (auto it = days_.rbegin(); it != days_.rend(); ++it)
        if (it->first.second == key && !it->second.name.empty()) return it->second.name;
    return key;
}

void Stats::Load() {
    days_.clear();
    HANDLE f = CreateFileW(file_.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    std::string data;
    char buf[4096];
    DWORD got;
    while (ReadFile(f, buf, sizeof buf, &got, nullptr) && got) data.append(buf, got);
    CloseHandle(f);
    if (data.rfind("\xEF\xBB\xBF", 0) == 0) data.erase(0, 3);
    std::istringstream in(data);
    std::string line;
    std::getline(in, line);  // header
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::vector<std::string> c;
        std::stringstream ls(line);
        for (std::string cell; std::getline(ls, cell, ';');) c.push_back(cell);
        if (c.size() < 9) continue;
        // day;key;name;active_min;idle_min;charge_min;used_pct;charged_pct;charge_count
        int day = atoi(c[0].substr(0, 4).c_str()) * 10000 + atoi(c[0].substr(5, 2).c_str()) * 100 + atoi(c[0].substr(8, 2).c_str());
        auto num = [](std::string v) {
            std::replace(v.begin(), v.end(), ',', '.');
            return atof(v.c_str());
        };
        DayStat ds;
        ds.name = FromUtf8(c[2]);
        ds.activeMin = num(c[3]);
        ds.idleMin = num(c[4]);
        ds.chargeMin = num(c[5]);
        ds.used = atoi(c[6].c_str());
        ds.charged = atoi(c[7].c_str());
        ds.chargeCount = atoi(c[8].c_str());
        days_[{day, FromUtf8(c[1])}] = ds;
    }
}

void Stats::Save() const {
    if (file_.empty()) return;
    std::string out = "\xEF\xBB\xBFgun;cihaz_id;cihaz;aktif_dk;uykuda_dk;sarjda_dk;harcanan_yuzde;sarj_edilen_yuzde;sarj_sayisi\r\n";
    char row[512];
    for (const auto& [k, ds] : days_) {
        // Decimal comma: Excel with Turkish settings reads "12,5" as a number ("12.5" would become a date).
        auto num = [](double v) {
            char b[32];
            snprintf(b, sizeof b, "%.1f", v);
            for (char* c = b; *c; ++c)
                if (*c == '.') *c = ',';
            return std::string(b);
        };
        snprintf(row, sizeof row, "%04d-%02d-%02d;%s;%s;%s;%s;%s;%d;%d;%d\r\n", k.first / 10000, k.first / 100 % 100,
                 k.first % 100, ToUtf8(k.second).c_str(), ToUtf8(ds.name).c_str(), num(ds.activeMin).c_str(),
                 num(ds.idleMin).c_str(), num(ds.chargeMin).c_str(), ds.used, ds.charged, ds.chargeCount);
        out += row;
    }
    // Write to a temp file and swap, so a crash never leaves a half-written CSV.
    std::wstring tmp = file_ + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD w;
    BOOL ok = WriteFile(f, out.data(), (DWORD)out.size(), &w, nullptr);
    CloseHandle(f);
    if (ok) MoveFileExW(tmp.c_str(), file_.c_str(), MOVEFILE_REPLACE_EXISTING);
}
