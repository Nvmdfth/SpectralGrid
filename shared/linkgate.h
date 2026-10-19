// Link-stability gate for probe OTA (Arduino-free, host-tested). A probe update is only sent when the link has been good for a
// configured time: online, RSSI above the gate, loss low, and the RSSI not wandering (spread of 10 s means over the window).
// Fed at any rate; advances in 1 s steps. 2026-10-08: default gate -50 dBm, 5 min, spread 4 dB, loss 2 %.
#pragma once
#include <stdint.h>
#include <math.h>

#define LG_HIST 300                       // seconds of RSSI history kept
typedef struct { float gate_dbm, spread_db, loss_max, stable_s; } LinkGateCfg;
static inline LinkGateCfg lg_default_cfg() { LinkGateCfg c = { -50.0f, 4.0f, 2.0f, 300.0f }; return c; }

typedef struct {
    int8_t rssi[LG_HIST]; uint16_t head;              // ring, newest sample at head-1
    uint32_t last_ms, ok_s; bool started;             // ok_s: seconds the criteria have held without a break
    float spread, rssi_now, loss_now; bool online;    // last evaluated values (for status display)
} LinkGate;

static inline void lg_reset(LinkGate *g) { *g = LinkGate(); }

// Spread of the 10 s means over the last `secs` seconds (needs at least two full 10 s blocks, else 0).
static inline float lg_spread(const LinkGate *g, uint32_t secs) {
    if (secs > LG_HIST) secs = LG_HIST;
    uint32_t blocks = secs / 10; if (blocks < 2) return 0;
    float lo = 1e9f, hi = -1e9f;
    for (uint32_t b = 0; b < blocks; b++) {
        float s = 0;
        for (int i = 0; i < 10; i++) s += g->rssi[(g->head + LG_HIST - 1 - b * 10 - i) % LG_HIST];
        s /= 10; if (s < lo) lo = s; if (s > hi) hi = s;
    }
    return hi - lo;
}

static inline void lg_update(LinkGate *g, const LinkGateCfg *c, bool online, float rssi, float loss, uint32_t now_ms) {
    if (!g->started) { g->started = true; g->last_ms = now_ms; return; }
    g->online = online; g->rssi_now = rssi; g->loss_now = loss;
    while (now_ms - g->last_ms >= 1000) {              // one step per elapsed second (a stall is replayed with the same sample)
        g->last_ms += 1000;
        g->rssi[g->head] = (int8_t)(rssi < -127 ? -127 : (rssi > 0 ? 0 : rssi)); g->head = (uint16_t)((g->head + 1) % LG_HIST);
        bool ok = online && rssi >= c->gate_dbm && loss <= c->loss_max;
        if (!ok) { g->ok_s = 0; g->spread = 0; continue; }
        g->ok_s++;
        g->spread = lg_spread(g, g->ok_s);
        if (g->spread > c->spread_db) { g->ok_s = 0; g->spread = 0; }   // wandering link: start the clock again
    }
}

static inline bool lg_ready(const LinkGate *g, const LinkGateCfg *c) { return g->ok_s >= (uint32_t)c->stable_s; }
