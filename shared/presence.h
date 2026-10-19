// Presence detector on top of csi_window.h (Arduino-free, host-tested). Two independent calls per 1 s window:
//   motion = temporal std > threshold                (something moves anywhere the link's reflections reach)
//   shadow = mean RSSI <= baseline RSSI - 1.5 dB     (something blocks the direct path: on the hub-probe line)
// Each is voted 2-of-3 windows; state = ON_LINE (shadow) > MOTION (motion only) > CLEAR, held PRES_HOLD_MS after the last active
// window so the display does not flicker.
//
// Calibration (30 s, quiet area): the MEDIAN of the window values is the "quiet level" q0; the threshold is q0 x (1 + g), g from the
// sensitivity setting (low 0.32 / normal 0.22 / high 0.16), but never closer than 8 robust sigmas (MAD) to q0. Median and MAD ignore a few
// disturbed windows (the old "largest window x 1.15" rule inflated 1.4x on a single bad window). A capture counts as "not still" (warning)
// when MORE than ceil(5 % of its windows) exceed 1.35 x median: 3 of 30 windows (probe links, 1/s), 4 of 60 (hub links, 2/s).
//
// Learning over time: after calibration the quiet level q and the baseline RSSI r follow the environment with a ~30 min time constant, but only
// from windows that are CLEAR (and a 30 s quiet stretch in), never while anything is detected, and limited to q in [0.7, 1.6] x q0 and
// r within +-6 dB of r0. The threshold keeps its calibrated ratio to q (thr = ratio x q). Hitting a limit raises `drift` (recalibrate).
// A baseline that is simply wrong (calibrated at another moment, restored stale after a move, or the link level stepped: AGC / temperature / battery
// steps of 1-3 dB are normal on this hardware) would keep the detector in ON_LINE forever and block that learning. The rule that tells a level
// step from a person: a person can only reach the line by MOVING. So each shadow onset is classified by whether motion accompanies it (2-of-3
// motion votes within 15 s before or 5 s after it began): no motion = level step, and once the shadow is steady for 20 s the baseline jumps to the
// new level; with motion = a person, who is kept for as long as they stay (absorbed only after 20 min perfectly still). RSSI steadily above the
// baseline for 60 s re-learns upward at once (a person can only lower the signal). Each change counts in relearn_n so it can be logged.
// Known blind spot: a level step that happens while someone is moving looks like a person arriving (resolved after 20 min still, or by recalibrating);
// a person standing still on the line when the hub boots is taken for a level step after 20 s. (Opus review + replays, 2026-10-08.)
#pragma once
#include <stdint.h>
#include <math.h>
#include <string.h>

#ifndef PRES_HOLD_MS
#define PRES_HOLD_MS    3000      // keep the state this long after the last active window (no flicker); 4000 until 2026-10-08
#endif
#define PRES_THR_FLOOR  0.015f
#define PRES_SHADOW_DB  1.5f
#define PRES_CAL_MIN    12        // windows (0.5 s apart) needed for a valid calibration
#define PRES_CAL_MAXW   128       // windows kept during a capture (30 s = 60)
#define PRES_G_LOW      0.32f     // allowed rise over the quiet level, per sensitivity
#define PRES_G_NORMAL   0.22f
#define PRES_G_HIGH     0.16f
#define PRES_MAD_K      8.0f      // threshold is at least this many robust sigmas above the quiet level
#define PRES_WARN_REL   1.35f     // a window above this x median counts as disturbed ...
#define PRES_WARN_FRAC  0.05f     // ... and more than this share of disturbed windows is a warning
#define PRES_TAU_S      1800.0f   // learning time constant
#define PRES_ADAPT_QUIET_MS 30000 // learn only after this long continuously CLEAR
#define PRES_Q_MIN      0.7f
#define PRES_Q_MAX      1.6f
#define PRES_R_LIM_DB   6.0f      // learned baseline RSSI stays within this of the calibration (was 4)
#define PRES_STEP_MS    20000     // a shadow that arrived WITHOUT motion and stays steady this long is a level step: the new baseline
#define PRES_PERSON_MS  1200000   // a shadow that arrived with motion (a person) is absorbed only after this long perfectly still (20 min)
#define PRES_ONSET_BEFORE_MS 15000 // motion within this long before the shadow started, or ...
#define PRES_ONSET_AFTER_MS  5000  // ... within this long after it started, makes it a person
#define PRES_ABSORB_DEV 0.8f      // steady = RSSI mean absolute deviation within this (dB)
#define PRES_UP_MS      60000     // RSSI steadily above the baseline this long: re-learn upward (a person can only lower it)
#define PRES_UP_DB      0.8f
#define PRES_REVERT_MS  600000    // an upward re-learn is provisional this long: ...
#define PRES_REVERT_DB  1.0f      // ... if the RSSI comes back to within this of the level it had before (for PRES_REVERT_N windows), that level is restored
#define PRES_REVERT_N   6
#define PRES_STUCK_MS   600000    // continuously active this long without a shadow: the room probably changed, suggest recalibrating

typedef enum { PRES_OFF = 0, PRES_CLEAR = 1, PRES_MOTION = 2, PRES_ON_LINE = 3 } PresState;   // OFF = not calibrated

static inline float pres_g_for_sens(int sens) { return sens <= 0 ? PRES_G_LOW : (sens >= 2 ? PRES_G_HIGH : PRES_G_NORMAL); }

typedef struct {
    bool cal; float thr, rssi_base;                        // current (learned) threshold and baseline RSSI
    float q0, ratio, r0, q, r;                             // calibration: quiet level, thr/q ratio, RSSI; learned quiet level and RSSI
    bool drift;                                            // the learned baseline hit a limit: recalibrate
    uint8_t mh, sh;                                        // verdict history of the last 3 windows, one bit each
    bool seq_valid; uint32_t seq;
    PresState state; bool moving; uint32_t active_ms; float score;   // score 0..100 (MOTION 40-65, ON_LINE 60-85)
    uint32_t quiet_since_ms, last_learn_ms, active_since_ms;
    uint32_t sh_since_ms, up_since_ms, sh_last_ms, last_motion_ms, sh_onset_ms; float sh_mean, sh_dev, up_mean;   // stationary-shadow / upward-shift trackers
    uint8_t sh_class; bool sh_prev, sh_motion_before;       // shadow onset: 0 undecided, 1 level step (no motion around its start), 2 person
    float up_prev_r; uint32_t up_prev_until_ms; uint8_t rv_n;                        // provisional upward re-learn: the baseline before it, until when it can be reverted, windows back at that level
    uint16_t relearn_n; float relearn_from, relearn_to;                              // count + last baseline change (the caller logs an event)
} PresLink;

typedef struct { uint32_t n; float v[PRES_CAL_MAXW]; double rsum; bool seq_valid; uint32_t seq; } PresCalAcc;
typedef struct { bool valid, warn; uint32_t n; float thr, q0, tmax, tmean, rssi_base, sigma; } PresCalResult;

static inline void pres_link_reset(PresLink *p) { *p = PresLink(); p->state = PRES_OFF; }
static inline void pres_link_set_cal(PresLink *p, float q0, float thr0, float rssi0) {
    pres_link_reset(p); p->cal = true; p->q0 = q0 > 1e-6f ? q0 : thr0 / 1.2f; p->thr = thr0; p->ratio = thr0 / p->q0;
    p->r0 = rssi0; p->q = p->q0; p->r = rssi0; p->rssi_base = rssi0; p->state = PRES_CLEAR;
}
// Restore a calibration together with what it had learned (flash).
static inline void pres_link_restore(PresLink *p, float q0, float thr0, float rssi0, float q, float r) {
    pres_link_set_cal(p, q0, thr0, rssi0);
    if (q >= PRES_Q_MIN * p->q0 && q <= PRES_Q_MAX * p->q0) { p->q = q; p->thr = p->ratio * q; }
    if (fabsf(r - rssi0) <= PRES_R_LIM_DB) { p->r = r; p->rssi_base = r; }
}
static inline int pres_pop3(uint8_t v) { return (v & 1) + ((v >> 1) & 1) + ((v >> 2) & 1); }
static inline float pres_clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

// Hold expiry only (call every frame; link_update calls it too).
static inline void pres_link_tick(PresLink *p, uint32_t now_ms) {
    if (!p->cal || p->state <= PRES_CLEAR) return;
    if (now_ms - p->active_ms >= PRES_HOLD_MS) { p->state = PRES_CLEAR; p->moving = false; p->score = 0; }
}

static inline void pres_learn(PresLink *p, float tstd, float rssi_mean, uint32_t now_ms) {
    float dt = (float)(now_ms - p->last_learn_ms); p->last_learn_ms = now_ms;
    if (dt <= 0 || dt > 5000) return;
    float a = 1.0f - expf(-dt / (PRES_TAU_S * 1000.0f));
    p->q += a * (tstd - p->q); p->r += a * (rssi_mean - p->r);
    float qlo = PRES_Q_MIN * p->q0, qhi = PRES_Q_MAX * p->q0;
    if (p->q < qlo) { p->q = qlo; p->drift = true; } else if (p->q > qhi) { p->q = qhi; p->drift = true; }
    if (p->r < p->r0 - PRES_R_LIM_DB) { p->r = p->r0 - PRES_R_LIM_DB; p->drift = true; } else if (p->r > p->r0 + PRES_R_LIM_DB) { p->r = p->r0 + PRES_R_LIM_DB; p->drift = true; }
    p->thr = p->ratio * p->q; p->rssi_base = p->r;
}

static inline void pres_set_r(PresLink *p, float nr) {
    float lo = p->r0 - PRES_R_LIM_DB, hi = p->r0 + PRES_R_LIM_DB;
    if (nr < lo || nr > hi) { p->drift = true; nr = nr < lo ? lo : hi; }
    p->relearn_from = p->rssi_base; p->r = nr; p->rssi_base = nr; p->relearn_to = nr; p->relearn_n++;
    p->state = PRES_CLEAR; p->moving = false; p->score = 0; p->mh = p->sh = 0; p->sh_since_ms = p->up_since_ms = 0; p->active_since_ms = 0; p->sh_class = 0; p->sh_prev = false;
    p->up_prev_until_ms = 0; p->rv_n = 0;
}

// win_seq 0 = no window yet. The same window may be passed repeatedly: it counts once.
static inline void pres_link_update(PresLink *p, float tstd, float rssi_mean, uint32_t win_seq, uint32_t now_ms) {
    if (!p->cal) { p->state = PRES_OFF; p->score = 0; return; }
    if (win_seq && (!p->seq_valid || win_seq != p->seq)) {
        p->seq_valid = true; p->seq = win_seq;
        bool m1 = tstd > p->thr, s1 = rssi_mean <= p->rssi_base - PRES_SHADOW_DB;
        p->mh = (uint8_t)(((p->mh << 1) | (m1 ? 1 : 0)) & 7);
        p->sh = (uint8_t)(((p->sh << 1) | (s1 ? 1 : 0)) & 7);
        bool mv = pres_pop3(p->mh) >= 2, sh = pres_pop3(p->sh) >= 2;
        // A person next to a node can RAISE the link's RSSI (2026-10-16: +2.7 dB after 80 s at node 3, learned as the new normal), and
        // the baseline learned up then reads their departure as a shadow (a person's, so absorbed only after 20 min). The upward re-learn
        // is therefore provisional: when the RSSI returns to the level it had before, that level wins at once.
        if (p->up_prev_until_ms) {
            if (now_ms >= p->up_prev_until_ms) p->up_prev_until_ms = 0;
            else {
                p->rv_n = fabsf(rssi_mean - p->up_prev_r) <= PRES_REVERT_DB ? (uint8_t)(p->rv_n < 255 ? p->rv_n + 1 : 255) : 0;
                if (p->rv_n >= PRES_REVERT_N && p->rssi_base > p->up_prev_r + PRES_REVERT_DB) { float to = p->up_prev_r; pres_set_r(p, to); return; }
            }
        }
        // classify the shadow onset: motion around its start = a person, none = a level step
        if (mv) p->last_motion_ms = now_ms ? now_ms : 1;
        bool onset = sh && !p->sh_prev; p->sh_prev = sh;
        if (onset) { p->sh_onset_ms = now_ms; p->sh_class = 0; p->sh_motion_before = p->last_motion_ms && (now_ms - p->last_motion_ms <= PRES_ONSET_BEFORE_MS); }
        if (!sh) p->sh_class = 0;
        if (sh && p->sh_class == 0) {
            if (p->sh_motion_before || mv) p->sh_class = 2;
            else if (now_ms - p->sh_onset_ms >= PRES_ONSET_AFTER_MS) p->sh_class = 1;
        }
        if (sh && p->sh_class == 1 && mv) p->sh_class = 2;                      // someone moves there later: it is a person after all
        // steady, motionless shadow: track it and absorb it once it has lasted long enough for its class
        if (sh && !mv) {
            if (!p->sh_since_ms) { p->sh_since_ms = now_ms; p->sh_last_ms = now_ms; p->sh_mean = rssi_mean; p->sh_dev = 0; }
            else { float dt = (float)(now_ms - p->sh_last_ms); p->sh_last_ms = now_ms; float a = 1.0f - expf(-dt / 10000.0f);
                   p->sh_dev += a * (fabsf(rssi_mean - p->sh_mean) - p->sh_dev); p->sh_mean += a * (rssi_mean - p->sh_mean); }
            uint32_t need = p->sh_class == 1 ? PRES_STEP_MS : (p->sh_class == 2 ? PRES_PERSON_MS : 0xFFFFFFFFu);
            if (need != 0xFFFFFFFFu && now_ms - p->sh_since_ms >= need && p->sh_dev <= PRES_ABSORB_DEV) { pres_set_r(p, p->sh_mean); return; }
        } else p->sh_since_ms = 0;
        if (sh || mv) {
            p->active_ms = now_ms; p->moving = mv; p->state = sh ? PRES_ON_LINE : PRES_MOTION;
            if (sh) p->score = 60.0f + 25.0f * pres_clamp01((p->rssi_base - rssi_mean) / 4.0f);
            else    p->score = 40.0f + 25.0f * pres_clamp01(tstd / p->thr - 1.0f);
            p->quiet_since_ms = 0;
            if (!p->active_since_ms) p->active_since_ms = now_ms;
            else if (now_ms - p->active_since_ms > PRES_STUCK_MS) p->drift = true;
        } else if (p->state <= PRES_CLEAR || now_ms - p->active_ms >= PRES_HOLD_MS) {
            p->active_since_ms = 0;
            // RSSI steadily above the baseline: only the environment can do that, re-learn upward quickly
            if (rssi_mean >= p->rssi_base + PRES_UP_DB) {
                if (!p->up_since_ms) { p->up_since_ms = now_ms; p->up_mean = rssi_mean; }
                else p->up_mean += 0.1f * (rssi_mean - p->up_mean);
                if (now_ms - p->up_since_ms >= PRES_UP_MS && p->up_mean >= p->rssi_base + PRES_UP_DB) { float old = p->rssi_base; pres_set_r(p, p->up_mean); p->up_prev_r = old; p->up_prev_until_ms = now_ms + PRES_REVERT_MS; return; }
            } else p->up_since_ms = 0;
            // quiet: learn the environment slowly, but only well after the last activity and from windows that are clearly quiet
            if (!p->quiet_since_ms) { p->quiet_since_ms = now_ms; p->last_learn_ms = now_ms; }
            else if (now_ms - p->quiet_since_ms >= PRES_ADAPT_QUIET_MS && !m1 && !s1) pres_learn(p, tstd, rssi_mean, now_ms);
            else if (now_ms - p->quiet_since_ms < PRES_ADAPT_QUIET_MS) p->last_learn_ms = now_ms;
        } else p->quiet_since_ms = 0;
    }
    pres_link_tick(p, now_ms);
}

static inline void pres_cal_reset(PresCalAcc *a) { *a = PresCalAcc(); }
static inline void pres_cal_add(PresCalAcc *a, float tstd, float rssi_mean, uint32_t win_seq) {
    if (!win_seq || (a->seq_valid && a->seq == win_seq)) return;
    a->seq_valid = true; a->seq = win_seq;
    if (a->n < PRES_CAL_MAXW) a->v[a->n] = tstd;
    a->n++; a->rsum += rssi_mean;
}
static inline float pres_median_of(float *x, uint32_t n) {    // in-place insertion sort (n <= 128)
    for (uint32_t i = 1; i < n; i++) { float k = x[i]; int j = (int)i - 1; while (j >= 0 && x[j] > k) { x[j + 1] = x[j]; j--; } x[j + 1] = k; }
    return (n & 1) ? x[n / 2] : 0.5f * (x[n / 2 - 1] + x[n / 2]);
}
static inline PresCalResult pres_cal_result(const PresCalAcc *a, float g) {
    PresCalResult r = PresCalResult();
    r.n = a->n;
    if (a->n < PRES_CAL_MIN) return r;
    uint32_t n = a->n < PRES_CAL_MAXW ? a->n : PRES_CAL_MAXW;
    float w[PRES_CAL_MAXW], dev[PRES_CAL_MAXW]; memcpy(w, a->v, n * sizeof(float));
    float mx = 0, sum = 0; for (uint32_t i = 0; i < n; i++) { if (w[i] > mx) mx = w[i]; sum += w[i]; }
    float med = pres_median_of(w, n);
    for (uint32_t i = 0; i < n; i++) dev[i] = fabsf(a->v[i] - med);
    float sig = 1.4826f * pres_median_of(dev, n);
    float rel = 1.0f + g, guard = med > 1e-6f ? 1.0f + PRES_MAD_K * sig / med : rel;
    float thr = med * (rel > guard ? rel : guard);
    if (thr < PRES_THR_FLOOR) thr = PRES_THR_FLOOR;
    uint32_t hi = 0; for (uint32_t i = 0; i < n; i++) if (a->v[i] > PRES_WARN_REL * med) hi++;
    r.q0 = med; r.thr = thr; r.tmax = mx; r.tmean = sum / n; r.sigma = sig; r.rssi_base = (float)(a->rsum / a->n);
    r.warn = hi > (uint32_t)ceilf(PRES_WARN_FRAC * n); r.valid = true;
    return r;
}
