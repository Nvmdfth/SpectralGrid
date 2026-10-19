// "A board was moved or bumped" (Arduino-free, host-tested). 2026-10-16: node 3 was bumped; every path through it shifted ~3 dB at once
// (the paths to the other probes -3 dB, the hub path +3 dB). presence.h reads that as a person standing still (absorbed only after 20 min), so
// the alarm stayed on in an empty room until node 3 was recalibrated. A person shadows one or two paths; only a moved board shifts ALL of its paths.
//   path off  = its RSSI (smoothed, MOVED_TAU_MS) is MOVED_DB or more away from its CALIBRATED level (r0, either sign) for MOVED_HOLD_MS
//   node moved = it has >= 2 paths and every one of them is off
// A person standing next to a node for minutes does the same to its paths, so the hint says "if nobody is there".
#pragma once
#include <stdint.h>
#include <math.h>
#include "pathfuse.h"

#define MOVED_DB        2.0f
#define MOVED_CLEAR_DB  1.0f
#define MOVED_HOLD_MS   180000u
#define MOVED_TAU_MS    15000.0f
#define MOVED_GAP_MS    10000u      // no sample for this long: start the average again

typedef struct { float ema, r0; bool init, off; uint32_t last_ms, since_ms; } MovedPath;   // one per directed link / hub link

static inline void moved_feed(MovedPath *m, float rssi, float r0, uint32_t now_ms) {
    if (!now_ms) now_ms = 1;
    if (!m->init || fabsf(m->r0 - r0) > 0.01f || now_ms - m->last_ms > MOVED_GAP_MS) {   // first sample, a new calibration, or a gap
        bool new_cal = !m->init || fabsf(m->r0 - r0) > 0.01f;
        m->ema = rssi; m->init = true; m->last_ms = now_ms; m->r0 = r0;
        if (new_cal) { m->off = false; m->since_ms = 0; }
        return;
    }
    float a = 1.0f - expf(-(float)(now_ms - m->last_ms) / MOVED_TAU_MS);
    m->ema += a * (rssi - m->ema); m->last_ms = now_ms;
    float dev = fabsf(m->ema - r0);
    if (m->off) { if (dev < MOVED_CLEAR_DB) { m->off = false; m->since_ms = 0; } }
    else if (dev >= MOVED_DB) { if (!m->since_ms) m->since_ms = now_ms; else if (now_ms - m->since_ms >= MOVED_HOLD_MS) m->off = true; }
    else m->since_ms = 0;
}

// Bit n set = node n (0 = hub) has >= 2 paths and every one is off. p[i].off comes from the path's directions (any direction off).
static inline uint32_t moved_nodes(const PathAct *p, int n, int max_node) {
    uint32_t mask = 0;
    for (int id = 0; id < max_node && id < 32; id++) {
        int cnt = 0, off = 0;
        for (int i = 0; i < n; i++) if (p[i].a == id || p[i].b == id) { cnt++; if (p[i].off) off++; }
        if (cnt >= 2 && off == cnt) mask |= 1u << id;
    }
    return mask;
}
