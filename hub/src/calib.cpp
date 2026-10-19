#include <math.h>
#include <string.h>
#include "calib.h"

static const float MULT[3] = {20.0f, 12.0f, 6.0f};   // scale = max(floor, MULT[sens] * sigma): lower sens => bigger scale

static void stat_add(CalStat *s, float x) {
    s->n++;
    float d = x - s->mean;
    s->mean += d / s->n;
    s->m2 += d * (x - s->mean);
    if (s->n == 1 || x > s->max) s->max = x;
}

void cal_init(CalState *c) { memset(c, 0, sizeof(*c)); c->target = -1; c->sens = CAL_SENS_NORMAL; }

bool cal_start(CalState *c, int target, uint8_t sens, uint32_t now_ms) {
    if (c->phase == CAL_CAPTURING) return false;
    if (target < -1 || target >= SCORE_N) return false;
    memset(c->emf, 0, sizeof(c->emf)); memset(c->csi, 0, sizeof(c->csi)); memset(c->raw, 0, sizeof(c->raw)); memset(c->pending, 0, sizeof(c->pending));
    c->target = target; c->sens = sens > CAL_SENS_HIGH ? CAL_SENS_NORMAL : sens; c->start_ms = now_ms; c->phase = CAL_CAPTURING;
    return true;
}

static bool wanted(const CalState *c, int id) { return c->target < 0 || c->target == id; }

static void finish_chan(CalChan *ch, const CalStat *st, float floor_scale, uint8_t sens, const Baseline *live, uint32_t live_ready_n, uint8_t *warn) {
    memset(ch, 0, sizeof(*ch));
    ch->n = st->n;
    if (st->n < CAL_MIN_SAMPLES) return;                      // not enough data: leave invalid
    float sd = st->n > 1 ? sqrtf(st->m2 / (st->n - 1)) : 0.0f;
    ch->valid = true; ch->mean = st->mean; ch->sd = sd; ch->max = st->max;
    float sc = MULT[sens] * sd;
    ch->scale = sc < floor_scale ? floor_scale : sc;
    if (live->n >= live_ready_n) { ch->prev_mean = live->mean; ch->prev_sd = sqrtf(live->var); }
    float excursion = st->max - st->mean;
    if (excursion > CAL_SPIKE_SIGMA * sd && excursion > floor_scale) *warn = CAL_WARN_SPIKE;
}

void cal_feed(CalState *c, const GridFrame *f, const ScoreState *s, uint32_t now_ms) {
    if (c->phase != CAL_CAPTURING) return;
    bool online[SCORE_N] = {false};
    for (int i = 0; i < f->n_nodes; i++) {
        const NodeSample *n = &f->nodes[i];
        if (n->node_id >= SCORE_N || !n->online || !wanted(c, n->node_id)) continue;
        online[n->node_id] = true;
        stat_add(&c->emf[n->node_id], (float)n->emf_p2p);
        stat_add(&c->raw[n->node_id], (float)n->emf_raw);
    }
    for (int i = 0; i < f->n_links; i++) {
        const LinkSample *l = &f->links[i];
        if (l->node_id < SCORE_N && online[l->node_id]) stat_add(&c->csi[l->node_id], l->csi_var);
    }
    if (now_ms - c->start_ms < CAL_CAPTURE_MS) return;
    for (int id = 0; id < SCORE_N; id++) {
        NodeCal *p = &c->pending[id];
        memset(p, 0, sizeof(*p));
        p->sens = c->sens;
        if (!wanted(c, id)) continue;
        finish_chan(&p->emf, &c->emf[id], CAL_EMF_FLOOR, c->sens, &s->emf[id], s->cfg.warmup_n, &p->warn);
        if (p->emf.valid) p->emf.raw_mean = c->raw[id].mean;
        finish_chan(&p->csi, &c->csi[id], CAL_CSI_FLOOR, c->sens, &s->csi[id], s->cfg.warmup_n, &p->warn);
    }
    c->phase = CAL_PENDING;
}

void cal_apply_one(ScoreState *s, int id, const NodeCal *cal) {
    if (cal->emf.valid) score_seed_emf(s, id, cal->emf.mean, cal->emf.sd, cal->emf.scale);
    if (cal->csi.valid) score_seed_csi(s, id, cal->csi.mean, cal->csi.sd, cal->csi.scale);
}

int cal_accept(CalState *c, ScoreState *s) {
    if (c->phase != CAL_PENDING) return 0;
    int applied = 0;
    for (int id = 0; id < SCORE_N; id++) {
        const NodeCal *p = &c->pending[id];
        if (!p->emf.valid && !p->csi.valid) continue;
        cal_apply_one(s, id, p);
        NodeCal *a = &c->applied[id];
        if (p->emf.valid) a->emf = p->emf;                  // a channel the capture could not measure keeps its old calibration
        if (p->csi.valid) a->csi = p->csi;
        a->sens = p->sens; a->warn = p->warn;
        applied++;
    }
    memset(c->pending, 0, sizeof(c->pending));
    c->phase = CAL_IDLE;
    return applied;
}

void cal_discard(CalState *c) { memset(c->pending, 0, sizeof(c->pending)); c->phase = CAL_IDLE; }

void cal_reset(CalState *c, ScoreState *s, int target) {
    for (int id = 0; id < SCORE_N; id++) {
        if (target >= 0 && target != id) continue;
        score_reset_node(s, id);
        memset(&c->applied[id], 0, sizeof(c->applied[id]));
    }
}

float cal_left_s(const CalState *c, uint32_t now_ms) {
    if (c->phase != CAL_CAPTURING) return 0;
    uint32_t el = now_ms - c->start_ms;
    return el >= CAL_CAPTURE_MS ? 0 : (CAL_CAPTURE_MS - el) / 1000.0f;
}
