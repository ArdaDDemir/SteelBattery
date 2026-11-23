#include "history.h"

#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>

namespace {
constexpr int64_t kGapSeconds = 30 * 60;       // longer silence = PC asleep / app closed: run ends
constexpr int64_t kRunWindow = 12 * 3600;      // only look this far back for the rate
constexpr int64_t kHeartbeat = 10 * 60;        // re-record an unchanged state this often
constexpr int64_t kKeep = 48 * 3600;
const std::vector<Sample> kEmpty;
}  // namespace

int64_t UnixNow() { return (int64_t)std::time(nullptr); }

Estimate EstimateTime(const std::vector<Sample>& s, int64_t now) {
    Estimate e;
    if (s.empty() || s.back().percent < 0) return e;
    const Sample& last = s.back();
    e.charging = last.charging;
    if (last.charging && last.percent >= 100) {
        e.known = true;
        return e;
    }
    // Walk back over the current run.
    size_t begin = s.size() - 1;
    while (begin > 0) {
        const Sample& prev = s[begin - 1];
        const Sample& cur = s[begin];
        if (prev.percent < 0 || prev.charging != last.charging || cur.t - prev.t > kGapSeconds ||
            last.t - prev.t > kRunWindow)
            break;
        --begin;
    }
    // Change points: samples whose percentage differs from the previous sample inside the run.
    const Sample* first = nullptr;
    const Sample* lastChange = nullptr;
    for (size_t i = begin + 1; i < s.size(); ++i) {
        if (s[i].percent == s[i - 1].percent) continue;
        if (!first) first = &s[i];
        lastChange = &s[i];
    }
    if (!first || first == lastChange) return e;
    int dp = last.charging ? lastChange->percent - first->percent : first->percent - lastChange->percent;
    int64_t dt = lastChange->t - first->t;
    if (dp < 2 || dt < 600) return e;
    double perSecond = (double)dp / (double)dt;
    double need = last.charging ? 100 - last.percent : last.percent;
    double seconds = need / perSecond - (double)(now - lastChange->t);
    e.known = true;
    e.minutes = seconds < 60 ? 1 : (int)(seconds / 60 + 0.5);
    return e;
}

std::wstring FormatEstimate(const Estimate& e) {
    if (!e.known) return L"";
    if (e.charging && e.minutes == 0) return L"Dolu";
    wchar_t buf[64];
    int h = e.minutes / 60, m = e.minutes % 60;
    if (h >= 10 || (h > 0 && m == 0))
        swprintf_s(buf, L"~%d sa", h);
    else if (h > 0)
        swprintf_s(buf, L"~%d sa %d dk", h, m);
    else
        swprintf_s(buf, L"~%d dk", m);
    return e.charging ? std::wstring(L"Dolmasına ") + buf : std::wstring(buf) + L" kaldı";
}

void History::Load(int64_t now) {
    data_.clear();
    std::wifstream in(file_);
    std::wstring line;
    while (std::getline(in, line)) {
        std::wistringstream ls(line);
        std::wstring key;
        Sample smp{};
        int ch = 0;
        if (!std::getline(ls, key, L'\t')) continue;
        if (!(ls >> smp.t >> smp.percent >> ch)) continue;
        smp.charging = ch != 0;
        if (now - smp.t <= kKeep) data_[key].push_back(smp);
    }
    in.close();
    // Rewrite without the pruned lines.
    std::wofstream out(file_, std::ios::trunc);
    for (const auto& [key, v] : data_)
        for (const auto& smp : v) out << key << L'\t' << smp.t << L' ' << smp.percent << L' ' << (smp.charging ? 1 : 0) << L'\n';
}

void History::Record(const std::wstring& key, int64_t t, const Reading& r) {
    Sample smp{t, r.ok ? r.percent : -1, r.ok && r.charging};
    auto& v = data_[key];
    if (!v.empty()) {
        const Sample& b = v.back();
        bool same = b.percent == smp.percent && b.charging == smp.charging;
        if (same && t - b.t < kHeartbeat) return;
        if (same && smp.percent < 0) return;  // no need to keep logging "not connected"
    }
    v.push_back(smp);
    while (!v.empty() && t - v.front().t > kKeep) v.erase(v.begin());
    std::wofstream out(file_, std::ios::app);
    out << key << L'\t' << smp.t << L' ' << smp.percent << L' ' << (smp.charging ? 1 : 0) << L'\n';
}

const std::vector<Sample>& History::Get(const std::wstring& key) const {
    auto it = data_.find(key);
    return it == data_.end() ? kEmpty : it->second;
}
