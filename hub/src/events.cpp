#include <stdio.h>
#include <string.h>
#include <time.h>
#include "events.h"

static const char *NAMES[] = {"system", "vector", "alert", "node_up", "node_down", "pairing", "calibration", "tag", "drift", "battery", "control", "touch"};
const char *ev_type_name(uint8_t t) { return t < sizeof(NAMES) / sizeof(NAMES[0]) ? NAMES[t] : "?"; }

void evlog_init(EventLog *l) { memset(l, 0, sizeof(*l)); l->next_seq = 1; }

const Event *evlog_push(EventLog *l, uint32_t t_ms, uint32_t epoch_s, uint8_t type, uint8_t id, uint8_t score, const char *text) {
    Event *e = &l->ev[l->head];
    memset(e, 0, sizeof(*e));
    e->seq = l->next_seq++; e->t_ms = t_ms; e->epoch_s = epoch_s; e->type = type; e->id = id; e->score = score;
    if (text) { strncpy(e->text, text, sizeof(e->text) - 1); }
    l->head = (l->head + 1) % EV_LOG_N;
    if (l->count < EV_LOG_N) l->count++;
    return e;
}

int evlog_since(const EventLog *l, uint32_t since_seq, Event *out, int max) {
    int n = 0;
    uint32_t start = (l->head + EV_LOG_N - l->count) % EV_LOG_N;
    for (uint32_t i = 0; i < l->count && n < max; i++) {
        const Event *e = &l->ev[(start + i) % EV_LOG_N];
        if (e->seq > since_seq) out[n++] = *e;
    }
    return n;
}

int evlog_tail(const EventLog *l, uint32_t since_seq, Event *out, int max) {
    uint32_t start = (l->head + EV_LOG_N - l->count) % EV_LOG_N;
    int total = 0;
    for (uint32_t i = 0; i < l->count; i++) if (l->ev[(start + i) % EV_LOG_N].seq > since_seq) total++;
    int skip = total > max ? total - max : 0, n = 0, seen = 0;
    for (uint32_t i = 0; i < l->count && n < max; i++) {
        const Event *e = &l->ev[(start + i) % EV_LOG_N];
        if (e->seq <= since_seq) continue;
        if (seen++ < skip) continue;
        out[n++] = *e;
    }
    return n;
}

void evdet_init(EventDetector *d, EventSink sink, void *ctx) { memset(d, 0, sizeof(*d)); d->sink = sink; d->ctx = ctx; d->vec_id = -1; d->cand_id = -1; }
void evdet_radio_activity(EventDetector *d, uint32_t now_ms) { d->radio_ms = now_ms; d->radio_valid = true; }

void evdet_emit(EventDetector *d, uint8_t type, uint8_t id, uint8_t score, const char *text) {
    if (!d->sink) return;
    Event e; memset(&e, 0, sizeof(e));
    e.type = type; e.id = id; e.score = score;
    strncpy(e.text, text, sizeof(e.text) - 1);
    d->sink(&e, d->ctx);
}

#define VEC_STABLE_FRAMES 3     // 150 ms at 20 Hz: ignore single-frame flicker
#define ALERT_ON  71
#define ALERT_OFF 55
#define BATT_LOW_MV 3300
#define BATT_OK_MV  3400
#define RADIO_WINDOW_MS 5000

void evdet_observe(EventDetector *d, const GridFrame *f, bool pairing, int cal_phase, uint32_t now_ms) {
    char t[48];

    // vector changes (debounced)
    int8_t vid = f->vec_id;
    if (!d->have_vec) { strncpy(d->vec, f->vector, sizeof(d->vec) - 1); d->vec_id = vid; d->have_vec = true; }
    else if (strcmp(f->vector, d->vec) != 0 || vid != d->vec_id) {
        if (strcmp(f->vector, d->cand) == 0 && vid == d->cand_id) d->cand_n++;
        else { strncpy(d->cand, f->vector, sizeof(d->cand) - 1); d->cand[sizeof(d->cand) - 1] = 0; d->cand_id = vid; d->cand_n = 1; }
        if (d->cand_n >= VEC_STABLE_FRAMES) {
            strncpy(d->vec, d->cand, sizeof(d->vec) - 1); d->vec_id = d->cand_id; d->cand_n = 0;
            if (vid >= 0) snprintf(t, sizeof(t), "%s node %d", d->vec, vid); else snprintf(t, sizeof(t), "%s", d->vec);
            bool radio = d->radio_valid && (now_ms - d->radio_ms) < RADIO_WINDOW_MS && (vid == 0 || (vid < 0 && strcmp(d->vec, "LOCAL_NODE") == 0));
            if (radio && strlen(t) < 22) strncat(t, " (hub radio?)", sizeof(t) - strlen(t) - 1);
            evdet_emit(d, EV_VECTOR, (uint8_t)vid, f->score, t);
        }
    } else d->cand_n = 0;

    // alert threshold with hysteresis
    if (!d->alerting && f->score >= ALERT_ON) { d->alerting = true; snprintf(t, sizeof(t), "alert: score %u", f->score); evdet_emit(d, EV_ALERT, 255, f->score, t); }
    else if (d->alerting && f->score < ALERT_OFF) { d->alerting = false; evdet_emit(d, EV_ALERT, 255, f->score, "alert cleared"); }

    // nodes joining, dropping, low battery
    for (int i = 0; i < f->n_nodes; i++) {
        const NodeSample *n = &f->nodes[i];
        if (n->node_id >= EVDET_N) continue;
        int id = n->node_id;
        if (!d->seen[id]) {
            d->seen[id] = true; d->online[id] = n->online;
            if (id == NODE_ID_HUB && !n->online) d->hub_startup = true;
            if (id != NODE_ID_HUB && n->online) { snprintf(t, sizeof(t), "node %d online", id); evdet_emit(d, EV_NODE_UP, (uint8_t)id, f->score, t); }
        } else if (d->online[id] != n->online) {
            d->online[id] = n->online;
            if (id == NODE_ID_HUB && n->online && d->hub_startup) { d->hub_startup = false; }
            else {
            if (id == NODE_ID_HUB) snprintf(t, sizeof(t), n->online ? "hub sensor recovered" : "hub sensor fault");
            else snprintf(t, sizeof(t), n->online ? "node %d online" : "node %d lost signal", id);
            evdet_emit(d, n->online ? EV_NODE_UP : EV_NODE_DOWN, (uint8_t)id, f->score, t);
            }
        }
        if (n->online && n->vbat_mv) {
            if (!d->low_batt[id] && n->vbat_mv < BATT_LOW_MV) { d->low_batt[id] = true; snprintf(t, sizeof(t), "node %d battery %.2f V", id, n->vbat_mv / 1000.0f); evdet_emit(d, EV_BATT, (uint8_t)id, f->score, t); }
            else if (d->low_batt[id] && n->vbat_mv > BATT_OK_MV) d->low_batt[id] = false;
        }
    }

    // pairing window and calibration phase
    if (!d->have_pairing) { d->pairing = pairing; d->have_pairing = true; }
    else if (d->pairing != pairing) { d->pairing = pairing; evdet_emit(d, EV_PAIRING, 255, f->score, pairing ? "pairing window open" : "pairing window closed"); }
    if (!d->have_cal) { d->cal_phase = cal_phase; d->have_cal = true; }
    else if (d->cal_phase != cal_phase) {
        if (cal_phase == 1) evdet_emit(d, EV_CAL, 255, f->score, "calibration capture started");
        else if (cal_phase == 2) evdet_emit(d, EV_CAL, 255, f->score, "calibration ready for review");
        d->cal_phase = cal_phase;
    }
}

int evlog_snapshot(const EventLog *l, Event *out, int max) { return evlog_since(l, 0, out, max); }

void evcsv_begin(EvCsv *c, const Event *snap, uint32_t n, bool epoch_known, int64_t epoch_off_ms) {
    memset(c, 0, sizeof(*c)); c->snap = snap; c->n = n; c->epoch_known = epoch_known; c->epoch_off_ms = epoch_off_ms;
}

static void csv_text(char *dst, size_t n, const char *src) {          // RFC 4180 quoting
    size_t w = 0; if (n < 3) return; dst[w++] = '"';
    for (; *src && w + 3 < n; src++) { if (*src == '"') dst[w++] = '"'; dst[w++] = (*src == '\n' || *src == '\r') ? ' ' : *src; }
    dst[w++] = '"'; dst[w] = 0;
}

size_t evcsv_fill(EvCsv *c, uint8_t *out, size_t max) {
    size_t w = 0;
    if (c->done) return 0;
    if (!c->header_done) { c->row_len = (uint16_t)snprintf(c->row, sizeof(c->row), "seq,t_ms,wall_utc,type,node,score,text\n"); c->row_off = 0; c->header_done = true; }
    while (w < max) {
        if (c->row_off >= c->row_len) {
            if (c->idx >= c->n) { c->done = true; break; }
            const Event *e = &c->snap[c->idx++];
            char wall[80] = "", node[8] = "";
            int64_t ms = -1;
            if (e->epoch_s) ms = (int64_t)e->epoch_s * 1000; else if (c->epoch_known) ms = (int64_t)e->t_ms + c->epoch_off_ms;
            if (ms >= 0) { time_t ss = (time_t)(ms / 1000); struct tm tmv; gmtime_r(&ss, &tmv);
                snprintf(wall, sizeof(wall), "%04d-%02d-%02dT%02d:%02d:%02dZ", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec); }
            char q[100]; csv_text(q, sizeof(q), e->text);
            if (e->id != 255) snprintf(node, sizeof(node), "%u", e->id);
            c->row_len = (uint16_t)snprintf(c->row, sizeof(c->row), "%u,%u,%s,%s,%s,%u,%s\n", (unsigned)e->seq, (unsigned)e->t_ms, wall, ev_type_name(e->type), node, e->score, q);
            c->row_off = 0;
        }
        size_t take = c->row_len - c->row_off; if (take > max - w) take = max - w;
        memcpy(out + w, c->row + c->row_off, take); w += take; c->row_off += (uint16_t)take;
    }
    return w;
}
