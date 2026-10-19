#include <Arduino.h>
#include <Preferences.h>
#include "p2p.h"
#include "logger.h"

static P2pSet set;
static P2pVerdict verdict = {PRES_OFF, 0, 0, 0, -1, -1};
static SemaphoreHandle_t lk = nullptr;
static bool enabled = true;
static uint32_t cmd_ms[MESH_MAX_PROBES + 1];                // last "links on" per probe
static bool seen_flags_ok[MESH_MAX_PROBES + 1];
static uint8_t tries[MESH_MAX_PROBES + 1];                  // "links on" sent without an answer since the probe last had them on
static bool told_old[MESH_MAX_PROBES + 1];
static bool was_online[MESH_MAX_PROBES + 1];
static const char *NS = "hubp2p";
static const uint8_t STORE_VER = 1;
typedef struct { uint8_t ver; struct { bool valid; uint8_t rx, tx; float q0, thr0, r0, q, r; } n[P2P_MAX]; } P2pStore;
static float saved_q[P2P_MAX], saved_r[P2P_MAX];

static void store_save() {
    static P2pStore st; memset(&st, 0, sizeof(st)); st.ver = STORE_VER;
    for (int i = 0; i < P2P_MAX; i++) {
        const P2pLink &l = set.l[i];
        if (!l.used || !l.pl.cal) continue;
        st.n[i].valid = true; st.n[i].rx = l.rx; st.n[i].tx = l.tx; st.n[i].q0 = l.pl.q0; st.n[i].thr0 = l.pl.ratio * l.pl.q0; st.n[i].r0 = l.pl.r0; st.n[i].q = l.pl.q; st.n[i].r = l.pl.r;
        saved_q[i] = l.pl.q; saved_r[i] = l.pl.r;
    }
    Preferences p;
    if (p.begin(NS, false)) { p.putBytes("links", &st, sizeof(st)); p.putBool("en", enabled); p.end(); }
}

void p2p_init() {
    lk = xSemaphoreCreateMutex();
    p2p_reset(&set);
    Preferences p;
    if (!p.begin(NS, false)) return;
    enabled = p.getBool("en", true);
    static P2pStore st; int n = 0;
    if (p.getBytesLength("links") == sizeof(st)) {
        p.getBytes("links", &st, sizeof(st));
        if (st.ver == STORE_VER)
            for (int i = 0; i < P2P_MAX; i++) if (st.n[i].valid) {
                P2pLink *l = p2p_find(&set, st.n[i].rx, st.n[i].tx, true);
                if (l) { pres_link_restore(&l->pl, st.n[i].q0, st.n[i].thr0, st.n[i].r0, st.n[i].q, st.n[i].r); n++; }
            }
    }
    p.end();
    for (int i = 0; i < P2P_MAX; i++) { saved_q[i] = set.l[i].pl.q; saved_r[i] = set.l[i].pl.r; }
    Serial.printf("p2p: %s, %d link calibration(s) restored from flash\n", enabled ? "on" : "off", n);
}

bool p2p_enabled() { return enabled; }
void p2p_set_enabled(bool on) {
    enabled = on;
    Preferences p; if (p.begin(NS, false)) { p.putBool("en", on); p.end(); }
    if (!on) mesh_send_cmd(0, CMD_LINKS, 0);
    else for (int i = 0; i <= MESH_MAX_PROBES; i++) cmd_ms[i] = 0;      // switch them on at the next frame
}

void p2p_loop(const GridFrame *f, uint32_t now_ms) {
    if (!lk) return;
    xSemaphoreTake(lk, portMAX_DELAY);
    for (int i = 0; i < f->n_nodes; i++) {
        const NodeSample &nd = f->nodes[i];
        uint8_t id = nd.node_id;
        if (id < 1 || id > MESH_MAX_PROBES) continue;
        if (!nd.online) { was_online[id] = false; continue; }
        if (!was_online[id]) { was_online[id] = true; tries[id] = 0; told_old[id] = false; mesh_clear_diag_old(id); cmd_ms[id] = 0; }   // (re)connected: maybe updated
        PktDiag d; uint32_t age;
        bool have = mesh_get_diag(id, &d, &age);
        bool on = have && age < 4000 && (d.flags & 3) == 3;
        bool old = mesh_diag_old(id);                               // its reports are another layout: unusable; say so once, keep asking slowly
        if (old && !told_old[id]) { told_old[id] = true; char m[60]; snprintf(m, sizeof(m), "probe %u firmware too old for peer links: update it", id); logger_event(EV_SYSTEM, id, 0, m); }
        if (old) on = false;
        if (on) {
            seen_flags_ok[id] = true; tries[id] = 0; told_old[id] = false;
            for (int k = 0; k < d.npeers && k < DIAG_MAX_PEERS; k++)
                if (age < 2500 && d.peer[k].fps >= 20) p2p_feed(&set, id, d.peer[k].node, d.peer[k].tstd_x10000 / 10000.0f, d.peer[k].rssi_x10 / 10.0f, d.peer[k].win_seq, now_ms);
        } else if (enabled && (cmd_ms[id] == 0 || now_ms - cmd_ms[id] >= ((tries[id] < 6 && !old) ? 5000u : 60000u))) {   // a probe boots with the peer links off: switch them on (and again after any reboot)
            cmd_ms[id] = now_ms ? now_ms : 1;
            mesh_send_cmd(id, CMD_LINKS, 3);
            if (tries[id] < 255) tries[id]++;
            if (tries[id] == 6) { char m[72]; snprintf(m, sizeof(m), "probe %u does not answer the peer-link command (old firmware?)", id); logger_event(EV_SYSTEM, id, 0, m); }
            if (seen_flags_ok[id]) { seen_flags_ok[id] = false; char m[44]; snprintf(m, sizeof(m), "probe %u peer links re-enabled", id); logger_event(EV_SYSTEM, id, 0, m); }
        }
    }
    p2p_tick(&set, now_ms);
    verdict = p2p_verdict(&set, now_ms);
    static uint32_t last_save = 0;                                      // learned baseline: save when it moved, at most every 10 min
    if (now_ms - last_save > 600000UL) {
        last_save = now_ms;
        for (int i = 0; i < P2P_MAX; i++) { const PresLink &pl = set.l[i].pl; if (set.l[i].used && pl.cal && (fabsf(pl.q - saved_q[i]) > 0.01f * pl.q0 || fabsf(pl.r - saved_r[i]) > 0.25f)) { store_save(); break; } }
    }
    xSemaphoreGive(lk);
}

P2pVerdict p2p_current() { return verdict; }
int p2p_get_paths(PathAct *out, int max, int *n_cal, uint32_t now_ms) {
    *n_cal = 0; if (!lk) return 0;
    xSemaphoreTake(lk, portMAX_DELAY); int n = p2p_paths(&set, now_ms, out, max, n_cal); xSemaphoreGive(lk);
    return n;
}

void p2p_cal_start(int target) { if (!lk) return; xSemaphoreTake(lk, portMAX_DELAY); p2p_cal_begin(&set, target); xSemaphoreGive(lk); }
void p2p_cal_done(float g) {
    if (!lk) return; xSemaphoreTake(lk, portMAX_DELAY); p2p_cal_finish(&set, g);
    int n = 0; for (int i = 0; i < P2P_MAX; i++) if (set.l[i].used && set.l[i].res.n) n++;
    Serial.printf("p2p: capture complete, %d link(s) have results\n", n);
    xSemaphoreGive(lk);
}
int p2p_cal_accept_all() {
    if (!lk) return 0; xSemaphoreTake(lk, portMAX_DELAY);
    int sk = 0, ok = p2p_cal_accept(&set, &sk);
    if (ok > 0) store_save();
    Serial.printf("p2p: %d link(s) calibrated, %d skipped (moving or too few windows)\n", ok, sk);
    if (sk) { char m[56]; snprintf(m, sizeof(m), "%d probe link(s) not calibrated (movement)", sk); logger_event(EV_CAL, 255, 0, m); }
    xSemaphoreGive(lk);
    return ok;
}
void p2p_cal_discard_all() { if (!lk) return; xSemaphoreTake(lk, portMAX_DELAY); p2p_cal_discard(&set); xSemaphoreGive(lk); }
void p2p_cal_reset_all() {
    if (!lk) return; xSemaphoreTake(lk, portMAX_DELAY);
    for (int i = 0; i < P2P_MAX; i++) if (set.l[i].used) pres_link_reset(&set.l[i].pl);
    store_save(); xSemaphoreGive(lk);
}
void p2p_cal_reset_node(int node) {
    if (!lk) return; xSemaphoreTake(lk, portMAX_DELAY);
    for (int i = 0; i < P2P_MAX; i++) if (set.l[i].used && (node < 0 || set.l[i].rx == node || set.l[i].tx == node)) pres_link_reset(&set.l[i].pl);
    store_save(); xSemaphoreGive(lk);
}
bool p2p_cal_pending_info(bool *warn) { *warn = false; if (!lk) return false; xSemaphoreTake(lk, portMAX_DELAY); bool a = p2p_cal_pending(&set, warn); xSemaphoreGive(lk); return a; }

String p2p_json() {
    String o = "{\"enabled\":"; o += enabled ? "true" : "false";
    if (!lk) return o + ",\"links\":[]}";
    xSemaphoreTake(lk, portMAX_DELAY);
    uint32_t now = millis();
    char b[360];
    snprintf(b, sizeof(b), ",\"state\":%d,\"score\":%.0f,\"pairs\":%u,\"links\":[", (int)verdict.state, verdict.score, verdict.pairs); o += b;
    bool first = true;
    for (int i = 0; i < P2P_MAX; i++) {
        const P2pLink &l = set.l[i];
        if (!l.used) continue;
        snprintf(b, sizeof(b), "%s{\"rx\":%u,\"tx\":%u,\"cal\":%s,\"fresh\":%s,\"state\":%d,\"score\":%.0f,\"tstd\":%.4f,\"rssi\":%.1f,\"thr\":%.4f,\"q0\":%.4f,\"q\":%.4f,\"r0\":%.1f,\"r\":%.2f,\"tr\":%.2f,\"dr\":%.1f,\"drift\":%s}", first ? "" : ",", l.rx, l.tx,
                 l.pl.cal ? "true" : "false", p2p_fresh(&l, now) ? "true" : "false", (int)l.pl.state, l.pl.score, l.tstd, l.rssi, l.pl.thr, l.pl.q0, l.pl.q, l.pl.r0, l.pl.r, l.pl.thr > 0 ? l.tstd / l.pl.thr : 0.0f, l.pl.rssi_base - l.rssi, l.pl.drift ? "true" : "false");
        o += b; first = false;
    }
    xSemaphoreGive(lk);
    return o + "]}";
}
