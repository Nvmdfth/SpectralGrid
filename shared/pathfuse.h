// One alarm rule for every RF path (Arduino-free, host-tested). A path is one physical line between two boards: hub<->probe
// (the hub's own detector) or probe<->probe (both directions fused into one pair by p2pfuse.h). Rule ("B", chosen 2026-10-15
// by replaying the recordings: 76 min empty room, walk test, 18-cell grid test):
//   fewer than FUSE_PAIRING_MIN calibrated paths  -> any single active path is a detection (one-probe setups keep working)
//   otherwise                                     -> >= 2 distinct active paths, or ONE path that is strong:
//       ON_LINE (its RSSI shadowed: someone on that line, which only that line sees), or MOTION scoring >= FUSE_STRONG
//       (presence.h: MOTION = 40 + 25 x (tstd/thr - 1), so 65 = temporal std at 2x its threshold).
// Note ON_LINE scores 60 + 25 x drop/4 dB, i.e. already ~69 at the 1.5 dB shadow threshold: it is strong by rule, not by score.
// Cost: an RSSI level step without motion (1-3 dB, normal on this hardware) also alarms on its own until presence.h absorbs it (20 s).
// Replay result vs the previous rule (a hub link alone, or >= 2 probe pairs): still person 60 % vs 39 % of seconds, false alarms
// 1.5 % vs 1.8 % over 76 min (0.2 % vs 0.3 % in its quiet middle), walking 100 % for both.
#pragma once
#include <stdint.h>
#include "presence.h"

#define FUSE_STRONG       65.0f
#define FUSE_PAIRING_MIN  3
#define FUSE_MAX_PATHS    32

typedef struct { uint8_t a, b; uint8_t state; float score; uint8_t off; } PathAct;   // a < b; a = 0 is the hub; state = PresState; off = a moved-board hint input (moved.h), not used by fuse_paths
typedef struct { PresState state; float score; int8_t a, b; uint8_t active, calibrated; bool strong; } FuseVerdict;

static inline FuseVerdict fuse_paths(const PathAct *p, int n, int n_cal) {
    FuseVerdict v; v.state = n_cal > 0 ? PRES_CLEAR : PRES_OFF; v.score = 0; v.a = v.b = -1; v.active = 0; v.calibrated = (uint8_t)n_cal; v.strong = false;
    uint8_t ka[FUSE_MAX_PATHS], kb[FUSE_MAX_PATHS]; int nk = 0; bool on_line = false; float best = 0;
    for (int i = 0; i < n; i++) {
        if (p[i].state <= PRES_CLEAR) continue;
        bool dup = false; for (int k = 0; k < nk; k++) if (ka[k] == p[i].a && kb[k] == p[i].b) dup = true;
        if (!dup && nk < FUSE_MAX_PATHS) { ka[nk] = p[i].a; kb[nk] = p[i].b; nk++; }
        if (p[i].state == PRES_ON_LINE) on_line = true;
        if (p[i].state == PRES_ON_LINE || p[i].score >= FUSE_STRONG) v.strong = true;
        if (p[i].score > best) { best = p[i].score; v.a = (int8_t)p[i].a; v.b = (int8_t)p[i].b; }
    }
    v.active = (uint8_t)nk;
    bool det = n_cal < FUSE_PAIRING_MIN ? nk >= 1 : (nk >= 2 || v.strong);
    if (det) { v.state = on_line ? PRES_ON_LINE : PRES_MOTION; v.score = best; } else { v.a = v.b = -1; }
    return v;
}
