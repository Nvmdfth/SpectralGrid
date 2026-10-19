// Probe-to-probe link presence (Arduino-free, host-tested). Every probe reports the CSI window numbers of the other probes' frames
// it hears (PktDiag); the hub feeds each directed link (rx hears tx) into its own presence.h detector and fuses the results.
// Why fuse: one link flickers about once per 10 min on an empty room (recorded 2026-10-15, 76 min, six links); a person raises
// several links at once (walk test: 2-7x on every probe link). A link A<->B is ONE physical path: both directions count as one "pair".
//   pair active  = either direction is non-CLEAR and fresh (a report within P2P_FRESH_MS)
//   verdict      = MOTION when >= P2P_MIN_PAIRS pairs are active (ON_LINE when one of the active links is ON_LINE), else CLEAR
// Calibration is a fixed capture (the same 30 s capture as the hub links) applied per link; a link whose capture warns (movement)
// or has too few windows keeps its previous calibration.
#pragma once
#include <stdint.h>
#include "presence.h"
#include "pathfuse.h"
#include "moved.h"

#define P2P_MAX         20         // directed links kept (5 probes all hearing each other)
#define P2P_FRESH_MS    5000       // a link with no report for this long is ignored (and its hold expires)
#define P2P_MIN_PAIRS   2          // distinct pairs that must be active for a verdict

typedef struct {
    bool used; uint8_t rx, tx;     // rx = the probe that heard, tx = the probe whose frames it heard
    PresLink pl; PresCalAcc acc; PresCalResult res;
    float tstd, rssi; uint32_t last_ms; uint16_t last_win; bool have, capturing;
    MovedPath mv;                  // how far this link sits from its calibrated RSSI (a moved board shifts all of its links)
} P2pLink;
typedef struct { P2pLink l[P2P_MAX]; bool cap; int8_t cap_target; } P2pSet;   // cap: a calibration capture is running (links that appear during it join in); cap_target: node id or -1 = all links
typedef struct { PresState state; float score; uint8_t pairs, links; int8_t rx, tx; } P2pVerdict;   // rx/tx: the strongest active link (-1 none)

static inline void p2p_reset(P2pSet *s) { memset(s, 0, sizeof(*s)); s->cap_target = -1; }
static inline bool p2p_in_cap(const P2pSet *s, uint8_t rx, uint8_t tx) { return s->cap && (s->cap_target < 0 || s->cap_target == rx || s->cap_target == tx); }
static inline P2pLink *p2p_find(P2pSet *s, uint8_t rx, uint8_t tx, bool create) {
    P2pLink *free_l = nullptr;
    for (int i = 0; i < P2P_MAX; i++) {
        if (s->l[i].used && s->l[i].rx == rx && s->l[i].tx == tx) return &s->l[i];
        if (!s->l[i].used && !free_l) free_l = &s->l[i];
    }
    if (!create || !free_l) return nullptr;
    memset(free_l, 0, sizeof(*free_l)); free_l->used = true; free_l->rx = rx; free_l->tx = tx; free_l->capturing = p2p_in_cap(s, rx, tx); pres_link_reset(&free_l->pl);
    return free_l;
}
// One report: the peer's latest window (win16 = its 16-bit window counter; the same window repeated counts once).
static inline void p2p_feed(P2pSet *s, uint8_t rx, uint8_t tx, float tstd, float rssi, uint16_t win16, uint32_t now_ms) {
    P2pLink *l = p2p_find(s, rx, tx, true);
    if (!l) return;
    if (l->have && l->last_win == win16) return;                         // the same window again: not a new report
    l->have = true; l->last_win = win16;
    l->tstd = tstd; l->rssi = rssi; l->last_ms = now_ms ? now_ms : 1;
    uint32_t seq = (uint32_t)win16 + 1;                                  // presence.h: 0 = no window yet
    if (l->capturing) pres_cal_add(&l->acc, tstd, rssi, seq);
    pres_link_update(&l->pl, tstd, rssi, seq, now_ms ? now_ms : 1);
    if (l->pl.cal) moved_feed(&l->mv, rssi, l->pl.r0, now_ms);
}
static inline void p2p_tick(P2pSet *s, uint32_t now_ms) {
    for (int i = 0; i < P2P_MAX; i++) if (s->l[i].used) pres_link_tick(&s->l[i].pl, now_ms);
}
static inline bool p2p_fresh(const P2pLink *l, uint32_t now_ms) { return l->last_ms && (now_ms - l->last_ms) < P2P_FRESH_MS; }

static inline P2pVerdict p2p_verdict(const P2pSet *s, uint32_t now_ms) {
    P2pVerdict v; v.state = PRES_CLEAR; v.score = 0; v.pairs = 0; v.links = 0; v.rx = v.tx = -1;
    uint8_t seen_lo[P2P_MAX], seen_hi[P2P_MAX]; int np = 0; bool on_line = false; float best = 0;
    bool any_cal = false;
    for (int i = 0; i < P2P_MAX; i++) {
        const P2pLink *l = &s->l[i];
        if (!l->used || !l->pl.cal) continue;
        any_cal = true;
        if (!p2p_fresh(l, now_ms) || l->pl.state <= PRES_CLEAR) continue;
        v.links++;
        if (l->pl.state == PRES_ON_LINE) on_line = true;
        if (l->pl.score > best) { best = l->pl.score; v.rx = (int8_t)l->rx; v.tx = (int8_t)l->tx; }
        uint8_t lo = l->rx < l->tx ? l->rx : l->tx, hi = l->rx < l->tx ? l->tx : l->rx;
        bool dup = false; for (int k = 0; k < np; k++) if (seen_lo[k] == lo && seen_hi[k] == hi) dup = true;
        if (!dup && np < P2P_MAX) { seen_lo[np] = lo; seen_hi[np] = hi; np++; }
    }
    if (!any_cal) { v.state = PRES_OFF; return v; }
    v.pairs = (uint8_t)np;
    if (np >= P2P_MIN_PAIRS) { v.state = on_line ? PRES_ON_LINE : PRES_MOTION; v.score = best; }
    return v;
}

// ---- calibration -------------------------------------------------------------------------------------------------------
static inline void p2p_cal_begin(P2pSet *s, int target) {   // target = node id (links with that probe at either end) or -1 = every link
    s->cap = true; s->cap_target = (int8_t)target;
    for (int i = 0; i < P2P_MAX; i++) if (s->l[i].used) { s->l[i].res = PresCalResult(); s->l[i].capturing = p2p_in_cap(s, s->l[i].rx, s->l[i].tx); if (s->l[i].capturing) pres_cal_reset(&s->l[i].acc); }
}
static inline void p2p_cal_finish(P2pSet *s, float g) {              // capture over: compute the pending results
    s->cap = false;
    for (int i = 0; i < P2P_MAX; i++) if (s->l[i].used && s->l[i].capturing) { s->l[i].capturing = false; s->l[i].res = pres_cal_result(&s->l[i].acc, g); }
}
static inline void p2p_cal_discard(P2pSet *s) {
    s->cap = false;
    for (int i = 0; i < P2P_MAX; i++) if (s->l[i].used) { s->l[i].capturing = false; s->l[i].res = PresCalResult(); }
}
// Apply the pending results; returns the number of links calibrated. A warned (moving) or short capture is left out.
static inline int p2p_cal_accept(P2pSet *s, int *skipped) {
    int ok = 0, sk = 0;
    for (int i = 0; i < P2P_MAX; i++) {
        P2pLink *l = &s->l[i];
        if (!l->used || !l->res.n) continue;
        if (l->res.valid && !l->res.warn) { pres_link_set_cal(&l->pl, l->res.q0, l->res.thr, l->res.rssi_base); ok++; } else sk++;
        l->res = PresCalResult();
    }
    if (skipped) *skipped = sk;
    return ok;
}
static inline bool p2p_cal_pending(const P2pSet *s, bool *warn) {   // anything to accept, and did any capture see movement
    bool any = false; *warn = false;
    for (int i = 0; i < P2P_MAX; i++) if (s->l[i].used && s->l[i].res.n) { if (s->l[i].res.valid) any = true; if (s->l[i].res.warn) *warn = true; }
    return any;
}

// The probe links as physical paths for pathfuse.h: one PathAct per probe pair (the stronger direction), calibrated and fresh only.
// Returns the number written; *n_cal = calibrated pairs (fresh or not, so a quiet-but-alive set still counts toward FUSE_PAIRING_MIN).
static inline int p2p_paths(const P2pSet *s, uint32_t now_ms, PathAct *out, int max, int *n_cal) {
    int n = 0, nc = 0; uint8_t ca[P2P_MAX], cb[P2P_MAX];
    for (int i = 0; i < P2P_MAX; i++) {
        const P2pLink *l = &s->l[i];
        if (!l->used || !l->pl.cal) continue;
        uint8_t a = l->rx < l->tx ? l->rx : l->tx, b = l->rx < l->tx ? l->tx : l->rx;
        bool seen = false; for (int k = 0; k < nc; k++) if (ca[k] == a && cb[k] == b) seen = true;
        if (!seen && nc < P2P_MAX) { ca[nc] = a; cb[nc] = b; nc++; }
        if (!p2p_fresh(l, now_ms)) continue;
        int j = 0; while (j < n && !(out[j].a == a && out[j].b == b)) j++;
        if (j == n) { if (n >= max) continue; out[n].a = a; out[n].b = b; out[n].state = PRES_CLEAR; out[n].score = 0; out[n].off = 0; n++; }
        if (l->mv.off) out[j].off = 1;
        if (l->pl.state > out[j].state) out[j].state = (uint8_t)l->pl.state;
        if (l->pl.state > PRES_CLEAR && l->pl.score > out[j].score) out[j].score = l->pl.score;
    }
    if (n_cal) *n_cal = nc;
    return n;
}
