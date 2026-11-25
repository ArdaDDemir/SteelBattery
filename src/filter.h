#pragma once
#include <cstdint>

#include "battery.h"

// Per-device memory of the last accepted reading, used to reject one-off garbage values
// (e.g. a receiver answering "10%" for a mouse that has 75%).
struct Track {
    bool hasGood = false;
    Reading good;       // last accepted real reading
    int64_t goodT = 0;  // when it was accepted
    bool hasPending = false;
    Reading pending;    // suspicious value waiting for a second, matching reading
};

enum class Verdict { Accept, Hold };

constexpr int kJumpLimit = 15;           // a change bigger than this needs confirmation...
constexpr int64_t kJumpWindow = 20 * 60; // ...if the last good value is younger than this
constexpr int kConfirmTolerance = 5;     // the confirming reading must be this close to the pending one

// Decides whether a fresh reading (r.ok must be true) is believable. Accept updates t.good.
Verdict FilterReading(Track& t, const Reading& r, int64_t now);
