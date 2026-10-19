// Composite anomaly engine (SpectralGrid.md §8). Keep Arduino-free.
#include <stdio.h>
#include <string.h>
#include "score.h"

void score_config_default(ScoreConfig *c) {
    c->w_csi = 0.40f; c->w_emf = 0.45f; c->w_t = 0.15f;
    c->wd_csi = 0.30f; c->wd_emf = 0.45f; c->wd_t = 0.25f;
    c->alpha = 0.02f;
    c->k_sigma = 2.5f;
    c->emf_scale = 50.0f;     // TODO(open item #5): calibrate on device
    c->csi_scale = 20.0f;     // TODO(open item #5)
    // Die temp quantises in ~1 degC steps and self-heats (spec §12 #12): a plain 1 degC step is sensor noise, so the
    // gate sits one full step up. Drops of 2 degC in 3 s (the next quantum) score 100.
    c->temp_gate_c = 1.0f; c->temp_gate_tol = 0.05f; c->temp_scale = 1.0f;
    c->warmup_n = 200;        // 10 s at 20 Hz
    c->lo = 20.0f; c->hi = 40.0f;
    c->hysteresis_ms = 3000;
}

void score_init(ScoreState *s) {
    score_config_default(&s->cfg);
    for (int i = 0; i < SCORE_N; i++) {
        baseline_init(&s->emf[i], s->cfg.alpha);
        baseline_init(&s->csi[i], s->cfg.alpha);
        s->emf_hat[i] = s->temp_hat[i] = s->csi_hat[i] = 0;
        pres_link_reset(&s->pres[i]);
        s->emf_scale_n[i] = s->cfg.emf_scale; s->csi_scale_n[i] = s->cfg.csi_scale;
    }
    s->mode = s->pending_mode = SCORE_MODE_DEGRADED;
    s->pending_set = false; s->pending_since_ms = 0;
    s->control_id = -1; s->n_ext = 0; s->n_ext_cal = 0; s->fused = fuse_paths(nullptr, 0, 0);
}

static void set_vec(GridFrame *f, const char *name, int id) {
    snprintf(f->vector, sizeof(f->vector), id >= 0 ? "%s_%d" : "%s", name, id);
    f->vec_id = (int8_t)id;
}

// drop = positive when the node cooled over the last 3 s. Only drops beyond gate+tol count (tol absorbs float error on
// quantised readings, e.g. 31.1 - 30.1 = 0.99999).
static float thermal_score(const ScoreConfig *c, float drop) {
    if (drop <= c->temp_gate_c + c->temp_gate_tol) return 0;
    float v = 100.0f * (drop - c->temp_gate_c) / c->temp_scale;
    return v > 100.0f ? 100.0f : v;
}

void score_compute(ScoreState *s, GridFrame *f, uint32_t now_ms) {
    const ScoreConfig *c = &s->cfg;
    bool online[SCORE_N] = {false};
    memset(s->emf_hat, 0, sizeof(s->emf_hat));
    memset(s->temp_hat, 0, sizeof(s->temp_hat));
    memset(s->csi_hat, 0, sizeof(s->csi_hat));

    // 1. per-channel scores against the *previous* baseline, then update baselines
    for (int i = 0; i < f->n_nodes; i++) {
        const NodeSample *n = &f->nodes[i];
        if (n->node_id >= SCORE_N || !n->online) continue;
        online[n->node_id] = true;
    }
    for (int i = 0; i < f->n_nodes; i++) {
        const NodeSample *n = &f->nodes[i];
        if (n->node_id >= SCORE_N || !n->online) continue;
        int id = n->node_id;
        s->emf_hat[id]  = baseline_score(&s->emf[id],  (float)n->emf_p2p, c->k_sigma, s->emf_scale_n[id], c->warmup_n);
        s->temp_hat[id] = thermal_score(c, -n->d_temp);
        baseline_update(&s->emf[id],  (float)n->emf_p2p);
    }
    bool probe_link = false, link_seen[SCORE_N] = {false};
    for (int i = 0; i < f->n_links; i++) {
        LinkSample *l = &f->links[i];
        if (l->node_id >= SCORE_N || !online[l->node_id]) continue;
        PresLink *pl = &s->pres[l->node_id];
        if (pl->cal) {   // calibrated: the presence detector owns this link's CSI channel
            pres_link_update(pl, l->tstd, l->rssi_mean, l->win_seq, now_ms);
            s->csi_hat[l->node_id] = pl->score;
            l->pres = (uint8_t)pl->state; l->pscore = (uint8_t)(pl->score + 0.5f); l->moving = pl->moving ? 1 : 0;
            l->tratio = pl->thr > 0 ? l->tstd / pl->thr : 0; l->drop = pl->rssi_base - l->rssi_mean;
            moved_feed(&s->mv[l->node_id], l->rssi_mean, pl->r0, now_ms);
            link_seen[l->node_id] = true;
        } else {
            s->csi_hat[l->node_id] = baseline_score(&s->csi[l->node_id], l->csi_var, c->k_sigma, s->csi_scale_n[l->node_id], c->warmup_n);
            baseline_update(&s->csi[l->node_id], l->csi_var);
        }
        if (l->node_id != NODE_ID_HUB) probe_link = true;
    }
    for (int i = 0; i < SCORE_N; i++) if (!link_seen[i]) pres_link_tick(&s->pres[i], now_ms);   // a link that went quiet still lets the hold expire

    // 1b. one alarm rule over every path (pathfuse.h): hub<->probe links + probe<->probe pairs. A single weak link alone is not a
    //     detection once there are >= 3 calibrated paths, so its channel score is dropped too (else it would still raise a vector).
    PathAct paths[FUSE_MAX_PATHS]; int np = 0, n_cal = 0;
    for (int i = 0; i < SCORE_N && np < FUSE_MAX_PATHS; i++) {
        if (i == s->control_id || !s->pres[i].cal || (i != NODE_ID_HUB && !online[i])) continue;
        n_cal++; paths[np].a = 0; paths[np].b = (uint8_t)i; paths[np].state = (uint8_t)s->pres[i].state; paths[np].score = s->pres[i].score; paths[np].off = s->mv[i].off ? 1 : 0; np++;
    }
    for (int k = 0; k < s->n_ext && np < FUSE_MAX_PATHS; k++) paths[np++] = s->ext[k];
    n_cal += s->n_ext_cal;
    FuseVerdict fv = fuse_paths(paths, np, n_cal); s->fused = fv;
    s->moved_mask = moved_nodes(paths, np, SCORE_N);
    bool pres_any = fv.state > PRES_CLEAR;
    if (!pres_any) for (int i = 0; i < SCORE_N; i++) if (s->pres[i].cal) s->csi_hat[i] = 0;

    // 2. mode with hysteresis (§8.3)
    ScoreMode want = probe_link ? SCORE_MODE_GRID : SCORE_MODE_DEGRADED;
    if (want == s->mode) s->pending_set = false;
    else if (!s->pending_set || s->pending_mode != want) { s->pending_mode = want; s->pending_set = true; s->pending_since_ms = now_ms; }
    else if (now_ms - s->pending_since_ms >= c->hysteresis_ms) { s->mode = want; s->pending_set = false; }

    // 3. aggregate (§8.2 / §8.3)
    float mc = 0, me = 0, mt = 0;
    for (int i = 0; i < SCORE_N; i++) {
        if (i == s->control_id) continue;                  // the control probe never contributes to the score
        if (s->csi_hat[i] > mc) mc = s->csi_hat[i];
        if (s->emf_hat[i] > me) me = s->emf_hat[i];
        if (s->temp_hat[i] > mt) mt = s->temp_hat[i];
    }
    bool grid = s->mode == SCORE_MODE_GRID;
    float sc = (grid ? c->w_csi : c->wd_csi) * mc + (grid ? c->w_emf : c->wd_emf) * me + (grid ? c->w_t : c->wd_t) * mt;
    float ps = pres_any ? fv.score : 0;                    // presence floors the headline: a detected person must read as STIRRING or more
    if (ps > sc) sc = ps;
    f->score = (uint8_t)(sc > 100.0f ? 100.0f : sc);
    f->control_id = (int8_t)s->control_id;

    // 4. vector classification (§8.4)
    int n_emf_hi = 0, n_csi_hi = 0, n_t_hi = 0, emf_id = -1, csi_id = -1, t_id = -1;
    int pres_id = (pres_any && fv.a == 0) ? fv.b : -1;     // strongest path is a hub link: name its probe; a probe pair: no single id
    bool all_emf_lo = true, any_lo = false;
    bool ctrl_hi = s->control_id >= 0 && (s->emf_hat[s->control_id] >= c->hi || s->csi_hat[s->control_id] >= c->hi);
    for (int i = 0; i < SCORE_N; i++) {
        if (i == s->control_id) continue;
        if (s->emf_hat[i] >= c->hi) { n_emf_hi++; emf_id = i; }
        if (s->csi_hat[i] >= c->hi) { n_csi_hi++; csi_id = i; }
        if (s->temp_hat[i] >= c->hi) { n_t_hi++; t_id = i; }
        if (s->emf_hat[i] >= c->lo) all_emf_lo = false;
        if (s->emf_hat[i] >= c->lo || s->csi_hat[i] >= c->lo || s->temp_hat[i] >= c->lo) any_lo = true;
    }
    if (pres_any && fv.score >= c->lo) any_lo = true;
    // QUIET is per-channel, not score-based: single-channel events cap at 40/45/15 (§8.2 weights), below `quiet`.
    if (!any_lo)                                                    set_vec(f, "QUIET", -1);
    else if (ctrl_hi && (n_emf_hi + n_csi_hi + n_t_hi) > 0)         set_vec(f, "COMMON_MODE", -1);   // control disturbed too: likely environmental
    else if (n_emf_hi >= 2 && n_csi_hi >= 1)                        set_vec(f, "GRID_ANOMALY", -1);
    else if (n_emf_hi == 1 && n_csi_hi == 0)                        set_vec(f, "LOCAL_NODE", emf_id);
    else if (pres_any && n_emf_hi == 0 && n_t_hi == 0 && s->emf_hat[NODE_ID_HUB] < c->lo)
                                                                    set_vec(f, fv.state == PRES_ON_LINE ? "LINE_BREAK" : "ROOM_MOTION", pres_id);   // CSI presence (calibrated links only)
    else if (n_csi_hi == 1 && n_emf_hi == 0 && s->emf_hat[csi_id] < c->lo && s->emf_hat[NODE_ID_HUB] < c->lo)
                                                                    { if (csi_id == NODE_ID_HUB) set_vec(f, "RF_DISTURBANCE", -1); else set_vec(f, "BARRIER_BREACH", csi_id); }
    else if (n_csi_hi >= 2 && all_emf_lo)                           set_vec(f, "MULTI_LINK_BREACH", -1);
    else if (n_t_hi == 1 && n_emf_hi == 0 && n_csi_hi == 0)         set_vec(f, "THERMAL", t_id);
    else                                                            set_vec(f, "UNCLASSIFIED", -1);

    // 5. strongest single channel, so the dashboard can say what is behind a vector (esp. UNCLASSIFIED)
    f->top_ch = 0; f->top_val = 0; f->top_id = -1;
    float best = 0;
    for (int i = 0; i < SCORE_N; i++) {
        float v[4] = {s->emf_hat[i], s->csi_hat[i], s->temp_hat[i], (pres_any && s->pres[i].cal && s->pres[i].state > PRES_CLEAR) ? (float)s->pres[i].score : 0.0f};
        for (int k = 0; k < 4; k++) if (v[k] > best) { best = v[k]; f->top_ch = (uint8_t)(k + 1); f->top_id = (int8_t)i; }
    }
    if (pres_any && fv.a > 0 && fv.score > best) { best = fv.score; f->top_ch = 4; f->top_id = fv.a; }   // a probe pair: name one end
    f->top_val = (uint8_t)(best > 100.0f ? 100.0f : best);
}

static void seed(Baseline *b, float alpha, float mean, float sd, uint32_t warmup_n) {
    baseline_init(b, alpha);
    b->mean = mean; b->var = sd * sd; b->n = warmup_n;
}
void score_seed_emf(ScoreState *s, int id, float mean, float sd, float scale) {
    if (id < 0 || id >= SCORE_N) return;
    seed(&s->emf[id], s->cfg.alpha, mean, sd, s->cfg.warmup_n); s->emf_scale_n[id] = scale;
}
void score_seed_csi(ScoreState *s, int id, float mean, float sd, float scale) {
    if (id < 0 || id >= SCORE_N) return;
    seed(&s->csi[id], s->cfg.alpha, mean, sd, s->cfg.warmup_n); s->csi_scale_n[id] = scale;
}
void score_reset_node(ScoreState *s, int id) {
    if (id < 0 || id >= SCORE_N) return;
    baseline_init(&s->emf[id], s->cfg.alpha); baseline_init(&s->csi[id], s->cfg.alpha);
    pres_link_reset(&s->pres[id]);
    s->emf_scale_n[id] = s->cfg.emf_scale; s->csi_scale_n[id] = s->cfg.csi_scale;
}

void score_set_control(ScoreState *s, int id) { s->control_id = (id >= 1 && id < SCORE_N) ? id : -1; }
