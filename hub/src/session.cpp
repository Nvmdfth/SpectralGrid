#include <stdio.h>
#include <string.h>
#include <time.h>
#include "session.h"

static const char *VEC[] = {"QUIET", "LOCAL_NODE", "BARRIER_BREACH", "MULTI_LINK_BREACH", "GRID_ANOMALY", "THERMAL",
                            "RF_DISTURBANCE", "COMMON_MODE", "UNCLASSIFIED"};
#define NVEC (sizeof(VEC) / sizeof(VEC[0]))

const char *sess_vec_name(uint8_t c) { return c < NVEC ? VEC[c] : "UNKNOWN"; }
uint8_t sess_vec_code(const char *n) { for (uint8_t i = 0; i < NVEC; i++) if (!strcmp(n, VEC[i])) return i; return 255; }

static void w16(uint8_t *p, uint16_t v) { p[0] = v & 0xFF; p[1] = v >> 8; }
static void w32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (v >> (8 * i)) & 0xFF; }
static uint16_t r16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t r32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static int16_t clamp16(float v) { return v > 32767 ? 32767 : (v < -32768 ? -32768 : (int16_t)(v < 0 ? v - 0.5f : v + 0.5f)); }

uint32_t sess_time(const uint8_t *rec) { return r32(rec); }

uint16_t sess_encode(const GridFrame *f, uint32_t t_ms, uint8_t *out, uint16_t max) {
    int nn = f->n_nodes > SESS_MAX_NODES ? SESS_MAX_NODES : f->n_nodes, nl = f->n_links > SESS_MAX_LINKS ? SESS_MAX_LINKS : f->n_links;
    uint16_t need = 9 + nn * 14 + nl * 3;
    if (need > max) return 0;
    w32(out, t_ms); out[4] = f->score; out[5] = sess_vec_code(f->vector); out[6] = (uint8_t)f->vec_id; out[7] = (uint8_t)nn; out[8] = (uint8_t)nl;
    uint8_t *p = out + 9;
    for (int i = 0; i < nn; i++, p += 14) {
        const NodeSample *n = &f->nodes[i];
        p[0] = n->node_id; p[1] = n->online ? 1 : 0; w16(p + 2, n->emf_raw); w16(p + 4, n->emf_p2p);
        w16(p + 6, (uint16_t)clamp16(n->temp_c * 100.0f)); w16(p + 8, (uint16_t)clamp16(n->d_temp * 100.0f));
        p[10] = (uint8_t)n->rssi;
        float lp = n->loss_pct < 0 ? 0 : (n->loss_pct > 100 ? 100 : n->loss_pct); p[11] = (uint8_t)(lp + 0.5f);
        w16(p + 12, n->vbat_mv);
    }
    for (int i = 0; i < nl; i++, p += 3) {
        float c = f->links[i].csi_var * 100.0f; if (c < 0) c = 0; if (c > 65535) c = 65535;
        p[0] = f->links[i].node_id; w16(p + 1, (uint16_t)(c + 0.5f));
    }
    return need;
}

bool sess_decode(const uint8_t *rec, uint16_t len, SessFrame *o) {
    if (len < 9) return false;
    o->t_ms = r32(rec); o->score = rec[4]; o->vec = rec[5]; o->vec_id = (int8_t)rec[6]; o->n_nodes = rec[7]; o->n_links = rec[8];
    if (o->n_nodes > SESS_MAX_NODES || o->n_links > SESS_MAX_LINKS || len < 9 + o->n_nodes * 14 + o->n_links * 3) return false;
    const uint8_t *p = rec + 9;
    for (int i = 0; i < o->n_nodes; i++, p += 14) {
        SessNode *n = &o->nodes[i];
        n->id = p[0]; n->online = p[1]; n->emf_raw = r16(p + 2); n->p2p = r16(p + 4);
        n->temp_c100 = (int16_t)r16(p + 6); n->d_temp_c100 = (int16_t)r16(p + 8); n->rssi = (int8_t)p[10]; n->loss_pct = p[11]; n->vbat_mv = r16(p + 12);
    }
    for (int i = 0; i < o->n_links; i++, p += 3) { o->links[i].id = p[0]; o->links[i].csi100 = r16(p + 1); }
    return true;
}

bool sess_seek(const RingLog *r, RingIter *it, uint32_t from_ms) {
    RingIter probe; ringlog_iter(r, &probe);
    uint8_t rec[4]; uint16_t n;       // only the timestamp is read: the caller holds the logger lock for the whole scan (all records on a full ring)
    for (;;) {
        RingIter before = probe;
        if (!ringlog_next(r, &probe, rec, sizeof(rec), &n)) return false;
        if (n >= 4 && sess_time(rec) >= from_ms) { *it = before; return true; }
    }
}

// ---- CSV ---------------------------------------------------------------------------------------------------------
void csv_begin(CsvStream *c, const RingLog *ring, bool epoch_known, int64_t epoch_off_ms) {
    memset(c, 0, sizeof(*c));
    c->ring = ring; c->epoch_known = epoch_known; c->epoch_off_ms = epoch_off_ms;
}

static void wall_iso(const CsvStream *c, uint32_t t_ms, char *out, size_t n) {
    if (!c->epoch_known) { out[0] = 0; return; }
    int64_t ms = (int64_t)t_ms + c->epoch_off_ms;
    time_t s = (time_t)(ms / 1000);
    struct tm tmv; gmtime_r(&s, &tmv);
    snprintf(out, n, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec, (int)(ms % 1000));
}

// Loads the next frame from the ring (re-seeking by timestamp if the ring dropped records since the last call).
static bool next_frame(CsvStream *c) {
    uint8_t rec[SESS_MAX_REC]; uint16_t n;
    for (int attempts = 0; attempts < 3; attempts++) {
        if (!c->iter_ok || c->it.dropped_at_start != c->ring->dropped) {
            if (c->have_last) {
                if (!sess_seek(c->ring, &c->it, c->last_t + 1)) return false;
            } else ringlog_iter(c->ring, &c->it);
            c->iter_ok = true;
        }
        if (!ringlog_next(c->ring, &c->it, rec, sizeof(rec), &n)) { if (c->it.dropped_at_start != c->ring->dropped) { c->iter_ok = false; continue; } return false; }
        if (!sess_decode(rec, n, &c->f)) continue;
        c->last_t = c->f.t_ms; c->have_last = true; c->node_idx = 0; c->have_frame = true;
        return true;
    }
    return false;
}

static void build_row(CsvStream *c) {
    const SessFrame *f = &c->f; const SessNode *n = &f->nodes[c->node_idx];
    char wall[40]; wall_iso(c, f->t_ms, wall, sizeof(wall));
    float csi = -1; for (int i = 0; i < f->n_links; i++) if (f->links[i].id == n->id) csi = f->links[i].csi100 / 100.0f;
    char csis[16] = ""; if (csi >= 0) snprintf(csis, sizeof(csis), "%.2f", csi);
    c->row_len = (uint16_t)snprintf(c->row, sizeof(c->row), "%u,%s,%u,%s,%d,%u,%u,%u,%u,%.2f,%.2f,%d,%u,%s,%.2f\n",
        (unsigned)f->t_ms, wall, f->score, sess_vec_name(f->vec), f->vec_id, n->id, n->online, n->emf_raw, n->p2p,
        n->temp_c100 / 100.0f, n->d_temp_c100 / 100.0f, n->rssi, n->loss_pct, csis, n->vbat_mv / 1000.0f);
    c->row_off = 0;
}

size_t csv_fill(CsvStream *c, uint8_t *out, size_t max) {
    size_t w = 0;
    if (c->done) return 0;
    if (!c->header_done) {
        const char *h = "t_ms,wall_utc,score,vector,vec_id,node,online,emf_raw,p2p,temp_c,d_temp_c,rssi,loss_pct,csi_var,vbat_v\n";
        c->row_len = (uint16_t)snprintf(c->row, sizeof(c->row), "%s", h); c->row_off = 0; c->header_done = true;
    }
    while (w < max) {
        if (c->row_off >= c->row_len) {
            if (!c->have_frame || c->node_idx >= c->f.n_nodes) {
                if (!next_frame(c)) { c->done = true; break; }
            }
            if (c->f.n_nodes == 0) { c->have_frame = false; continue; }
            build_row(c);
            c->node_idx++;
            if (c->node_idx >= c->f.n_nodes) c->have_frame = false;   // finished this frame after this row
        }
        size_t take = c->row_len - c->row_off; if (take > max - w) take = max - w;
        memcpy(out + w, c->row + c->row_off, take); w += take; c->row_off += (uint16_t)take;
    }
    return w;
}
