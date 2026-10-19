// EMF-wire touch / near-hand detector (Arduino-free, host-tested). Watches the slow level of a node's raw touch reading against a
// self-adapting baseline; a hand about 10 cm from the wire or a touch shifts it by 100 to tens of thousands of counts, a hand at
// 30 cm does not (measured 2026-10-08, EMF_TEST.md). Deliberately separate from the presence score: it only raises an event.
// Time-based (not per-sample), so it behaves the same at 2 Hz and 20 Hz.
#pragma once
#include <stdint.h>
#include <math.h>

#define TOUCH_K          4.0f      // shift must exceed K sigma ... (5 until 2026-10-08; with the 500 ms persistence 4 is enough)
#define TOUCH_MIN        50.0f     // ... and this many counts
#define TOUCH_TAU_MS     8000.0f   // baseline / noise adaptation time constant (only while quiet)
#define TOUCH_FIRE_MS    500       // shift must persist this long to count (a hand or touch stays; a Wi-Fi burst from a nearby Pi does not; was 150)
#define TOUCH_CLEAR_MS   1000      // back within half the threshold this long ends the event
#define TOUCH_LOCKOUT_MS 1000      // no new event for this long after one ends
#define TOUCH_MAX_MS     10000     // a shift that never goes away (wire moved) is re-baselined after this
#define TOUCH_WARMUP_MS  20000     // after boot: learn the baseline and noise fast, raise no events

typedef struct {
    bool init, active;
    float min_counts, base, dev, peak;                  // min_counts: smallest shift that counts (hub 80: its raw level is noisier; probes 50). dev = running mean absolute deviation; sigma ~ 1.25 x dev
    uint32_t start_ms, last_ms, over_ms, quiet_ms, lockout_until, active_since;
} TouchDet;

static inline void touch_reset(TouchDet *t, float min_counts = TOUCH_MIN) { *t = TouchDet(); t->min_counts = min_counts; }

// Returns true exactly once when an event starts. t->peak holds the largest |shift| of the current event.
static inline bool touch_update(TouchDet *t, float raw, uint32_t now) {
    if (!t->init) { t->init = true; t->base = raw; t->dev = 4.0f; t->start_ms = t->last_ms = now; return false; }
    uint32_t dt = now - t->last_ms; if (dt > 1000) dt = 1000; t->last_ms = now;
    float d = raw - t->base, ad = fabsf(d);
    if (now - t->start_ms < TOUCH_WARMUP_MS) {                                    // warm-up: fast learning, no events
        float aw = 1.0f - expf(-(float)dt / 2000.0f); t->base += aw * d; t->dev += aw * (ad - t->dev); return false;
    }
    float sig = t->dev * 1.25f; if (sig < 3.0f) sig = 3.0f;
    float thr = TOUCH_K * sig; if (thr < t->min_counts) thr = t->min_counts;
    float a = 1.0f - expf(-(float)dt / TOUCH_TAU_MS);
    bool fired = false;
    if (!t->active) {
        if (ad >= thr) {
            t->over_ms += dt;
            if (t->over_ms >= TOUCH_FIRE_MS && (int32_t)(now - t->lockout_until) >= 0) { t->active = true; t->peak = ad; t->quiet_ms = 0; t->active_since = now; fired = true; }
        } else {
            t->over_ms = t->over_ms > 2 * dt ? t->over_ms - 2 * dt : 0;               // leaky: noise around the threshold must not reset a real shift
            if (ad < 2.5f * sig) { t->base += a * d; t->dev += a * (ad - t->dev); }     // learn only while quiet
        }
    } else {
        if (ad > t->peak) t->peak = ad;
        if (ad < 0.5f * thr) {
            t->quiet_ms += dt;
            if (t->quiet_ms >= TOUCH_CLEAR_MS) { t->active = false; t->over_ms = 0; t->lockout_until = now + TOUCH_LOCKOUT_MS; }
        } else t->quiet_ms = 0;
        if (t->active && now - t->active_since >= TOUCH_MAX_MS) { t->active = false; t->base = raw; t->over_ms = 0; t->lockout_until = now + TOUCH_LOCKOUT_MS; }
    }
    return fired;
}
