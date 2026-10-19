#include <math.h>
#include <stdio.h>
#include <string.h>
#include "drift.h"

void drift_init(DriftState *d, DriftSink sink, void *ctx) { memset(d, 0, sizeof(*d)); d->sink = sink; d->ctx = ctx; }
DriftKind drift_kind(const DriftState *d, int id) { return (id >= 0 && id < SCORE_N) ? (DriftKind)d->n[id].kind : DRIFT_NONE; }
void drift_reset_node(DriftState *d, int id) { if (id >= 0 && id < SCORE_N) { DriftNode *n = &d->n[id]; n->kind = 0; n->ant_on = n->noise_on = false; n->clear_since = 0; n->first_ms = n->first_set ? n->first_ms : 0; } }
const char *drift_kind_name(DriftKind k) { return k == DRIFT_UNCAL ? "uncalibrated" : k == DRIFT_ANTENNA ? "antenna" : k == DRIFT_NOISE ? "noise" : k == DRIFT_MOVED ? "moved" : "none"; }
void drift_set_moved(DriftState *d, int id, bool moved) { if (id >= 0 && id < SCORE_N) d->n[id].moved = moved; }

static void raise(DriftState *d, int id, DriftKind k, uint32_t now) {
    DriftNode *n = &d->n[id];
    if (n->kind == k) return;
    n->kind = (uint8_t)k; n->clear_since = 0; (void)now;
    char m[44];
    if (k == DRIFT_ANTENNA) snprintf(m, sizeof(m), "node %d: antenna change suspected", id);
    else if (k == DRIFT_NOISE) snprintf(m, sizeof(m), "node %d: noise floor shifted", id);
    else if (k == DRIFT_MOVED) snprintf(m, sizeof(m), "node %d: may have been moved or bumped", id);
    else snprintf(m, sizeof(m), "node %d: not calibrated", id);
    if (d->sink) d->sink(id, k, m, d->ctx);
}

void drift_update(DriftState *d, const GridFrame *f, const CalState *c, const ScoreState *s, uint32_t now) {
    bool capturing = c->phase == CAL_CAPTURING;
    for (int i = 0; i < f->n_nodes; i++) {
        const NodeSample *ns = &f->nodes[i];
        int id = ns->node_id;
        if (id >= SCORE_N || id == s->control_id) continue;     // the control node is not scored, so it is not calibrated either
        DriftNode *n = &d->n[id];
        if (!ns->online) { n->first_set = false; n->raw_init = false; n->ant_on = n->noise_on = false; continue; }
        if (!n->first_set) { n->first_set = true; n->first_ms = now; }
        if (!n->raw_init) { n->raw_ema = ns->emf_raw; n->raw_init = true; } else n->raw_ema += 0.05f * (ns->emf_raw - n->raw_ema);
        if (capturing) continue;                                 // hold all conditions while a calibration capture runs

        const NodeCal *a = &c->applied[id];
        bool cal = a->emf.valid;
        bool ant = false, noise = false;
        if (cal && a->emf.raw_mean > 0) {
            float thr = fmaxf(0.05f * a->emf.raw_mean, 600.0f);
            ant = fabsf(n->raw_ema - a->emf.raw_mean) > thr;
        }
        if (cal && s->emf[id].n >= s->cfg.warmup_n) {
            float thr = fmaxf(5.0f * a->emf.sd, 5.0f);
            noise = (sqrtf(s->emf[id].var) < 1e9f) && (s->emf[id].mean > a->emf.mean + thr);
        }
        if (ant) { if (!n->ant_on) { n->ant_on = true; n->ant_since = now; } } else n->ant_on = false;
        if (noise) { if (!n->noise_on) { n->noise_on = true; n->noise_since = now; } } else n->noise_on = false;

        DriftKind want = DRIFT_NONE;
        if (n->ant_on && now - n->ant_since >= DRIFT_ANTENNA_HOLD_MS) want = DRIFT_ANTENNA;
        else if (n->noise_on && now - n->noise_since >= DRIFT_NOISE_HOLD_MS) want = DRIFT_NOISE;
        else if (n->moved) want = DRIFT_MOVED;
        else if (!cal && now - n->first_ms >= DRIFT_UNCAL_AFTER_MS) want = DRIFT_UNCAL;

        if (want != DRIFT_NONE) raise(d, id, want, now);
        else if (n->kind != DRIFT_NONE) {
            if (!n->clear_since) n->clear_since = now;
            else if (now - n->clear_since >= DRIFT_CLEAR_MS) { n->kind = 0; n->clear_since = 0; }
        } else n->clear_since = 0;
    }
}
