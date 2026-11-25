#include "filter.h"

#include <cstdlib>

Verdict FilterReading(Track& t, const Reading& r, int64_t now) {
    auto accept = [&]() {
        t.good = r;
        t.goodT = now;
        t.hasGood = true;
        t.hasPending = false;
        return Verdict::Accept;
    };
    if (!t.hasGood || now - t.goodT > kJumpWindow) return accept();
    if (std::abs(r.percent - t.good.percent) <= kJumpLimit) return accept();
    if (t.hasPending && std::abs(r.percent - t.pending.percent) <= kConfirmTolerance) return accept();
    t.pending = r;
    t.hasPending = true;
    return Verdict::Hold;
}
