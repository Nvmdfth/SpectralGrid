#include <Arduino.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <math.h>
#include "calibration.h"
#include "calib.h"
#include "logger.h"
#include "presence.h"
#include "p2p.h"

static CalState cal;
static ScoreState *sc = nullptr;
static SemaphoreHandle_t lock = nullptr;
static const char *NS = "hubcal";
static const uint8_t STORE_VER = 2;     // v2: CalChan gained raw_mean (antenna-change signal); older stores are discarded

// requests from other tasks (consumed in calibration_feed on Core 1)
static volatile int req_start_node = -2;     // -2 none, -1 all, else id
static volatile uint8_t req_sens = CAL_SENS_NORMAL;
static volatile bool req_accept = false, req_discard = false;
static volatile int req_reset_node = -2;

typedef struct { uint8_t ver; NodeCal applied[SCORE_N]; } CalStore;

// CSI presence calibration (presence.h) rides on the same capture/accept/discard/reset flow as the EMF/CSI calibration above;
// it is stored separately so the older store layout stays untouched.
static const char *PNS = "hubpres";
static const uint8_t PSTORE_VER = 2;     // v2: median-based calibration + learned baseline (older stores are discarded: recalibrate)
typedef struct { uint8_t ver; struct { bool valid; float q0, thr0, r0, q, r; } n[SCORE_N]; } PresStore;
static PresCalAcc pacc[SCORE_N];
static PresCalResult ppend[SCORE_N];
static float saved_q[SCORE_N], saved_r[SCORE_N];            // what the flash holds, to write the learned baseline only when it moved
static bool drift_reported[SCORE_N];
static uint16_t relearn_seen[SCORE_N];
static uint32_t verify_until = 0, last_learn_save = 0; static float verify_active_ms = 0; static bool verifying = false;

static void pstore_save() {
    static PresStore st;
    st.ver = PSTORE_VER;
    for (int i = 0; i < SCORE_N; i++) {
        const PresLink &pl = sc->pres[i];
        st.n[i].valid = pl.cal; st.n[i].q0 = pl.q0; st.n[i].thr0 = pl.ratio * pl.q0; st.n[i].r0 = pl.r0; st.n[i].q = pl.q; st.n[i].r = pl.r;
        saved_q[i] = pl.q; saved_r[i] = pl.r;
    }
    Preferences p;
    if (p.begin(PNS, false)) { p.putBytes("pres", &st, sizeof(st)); p.end(); }
}
static void pstore_load(ScoreState *s) {
    static PresStore st;
    Preferences p;
    if (!p.begin(PNS, false)) return;
    int n = 0;
    if (p.getBytesLength("pres") == sizeof(st)) {
        p.getBytes("pres", &st, sizeof(st));
        if (st.ver == PSTORE_VER)
            for (int i = 0; i < SCORE_N; i++) if (st.n[i].valid) {
                pres_link_restore(&s->pres[i], st.n[i].q0, st.n[i].thr0, st.n[i].r0, st.n[i].q, st.n[i].r);
                saved_q[i] = s->pres[i].q; saved_r[i] = s->pres[i].r; n++;
            }
    }
    p.end();
    Serial.printf("presence: %d link(s) restored from flash\n", n);
}

static void store_save() {
    static CalStore st;                      // CalStore is ~1.5 KB: keep it off the stack
    st.ver = STORE_VER;
    memcpy(st.applied, cal.applied, sizeof(st.applied));
    Preferences p;
    if (p.begin(NS, false)) { p.putBytes("cal", &st, sizeof(st)); p.end(); }
}

// Thermal sensitivity: the required 3 s cooling (degC) for a node to score. Smaller = more sensitive. Saved in NVS.
#define TNS "hubset"
float calibration_thermal_gate() { return sc ? sc->cfg.temp_gate_c : 1.0f; }
bool calibration_set_thermal_gate(float g) {
    if (!sc || !(g >= THERMAL_GATE_MIN && g <= THERMAL_GATE_MAX)) return false;
    sc->cfg.temp_gate_c = g;
    Preferences p;
    if (p.begin(TNS, false)) { p.putFloat("tgate", g); p.end(); }
    return true;
}

void calibration_init(ScoreState *s) {
    sc = s;
    { Preferences t; if (t.begin(TNS, false)) { float g = t.getFloat("tgate", s->cfg.temp_gate_c);
        if (g >= THERMAL_GATE_MIN && g <= THERMAL_GATE_MAX) s->cfg.temp_gate_c = g; t.end(); } }
    lock = xSemaphoreCreateMutex();
    cal_init(&cal);
    Preferences p;
    if (!p.begin(NS, false)) return;      // read-write: creates the namespace on first boot
    static CalStore st;
    int applied = 0;
    if (p.getBytesLength("cal") == sizeof(st)) {
        p.getBytes("cal", &st, sizeof(st));
        if (st.ver == STORE_VER) {
            memcpy(cal.applied, st.applied, sizeof(cal.applied));
            for (int id = 0; id < SCORE_N; id++) {
                if (cal.applied[id].emf.valid || cal.applied[id].csi.valid) { cal_apply_one(sc, id, &cal.applied[id]); applied++; }
            }
        }
    }
    p.end();
    Serial.printf("calibration: %d node(s) restored from flash\n", applied);
    pstore_load(sc);
}

void calibration_feed(const GridFrame *f) {
    if (!sc) return;
    uint32_t now = millis();
    xSemaphoreTake(lock, portMAX_DELAY);
    int n = req_start_node;
    if (n != -2) {
        req_start_node = -2;
        if (cal_start(&cal, n, req_sens, now)) {
            for (int i = 0; i < SCORE_N; i++) { pres_cal_reset(&pacc[i]); ppend[i] = PresCalResult(); }
            p2p_cal_start(n);
            Serial.printf("calibration: capturing %s for %d s, keep the area still\n", n < 0 ? "all nodes" : "one node", CAL_CAPTURE_MS / 1000);
        }
    }
    if (req_discard) { req_discard = false; cal_discard(&cal); for (int i = 0; i < SCORE_N; i++) ppend[i] = PresCalResult(); p2p_cal_discard_all(); Serial.println("calibration: discarded"); logger_event(EV_CAL, 255, f->score, "calibration discarded"); }
    if (req_accept) {
        req_accept = false;
        char who[40] = "";                                   // which nodes this accept covers, for the event text (and the field log)
        for (int i = 0; i < SCORE_N; i++) if (cal.pending[i].emf.valid || cal.pending[i].csi.valid || ppend[i].valid) {
            char b[8]; snprintf(b, sizeof(b), "%s%d", who[0] ? "," : "", i); strncat(who, b, sizeof(who) - strlen(who) - 1);
        }
        int k = cal_accept(&cal, sc);
        int kp = 0;
        bool probe_link_cal = false;
        for (int i = 1; i < SCORE_N; i++) if (ppend[i].valid) probe_link_cal = true;
        for (int i = 0; i < SCORE_N; i++) if (ppend[i].valid) {
            pres_link_set_cal(&sc->pres[i], ppend[i].q0, ppend[i].thr, ppend[i].rssi_base); ppend[i] = PresCalResult(); drift_reported[i] = false; relearn_seen[i] = 0; kp++;
        }
        int kq = p2p_cal_accept_all(); kp += kq; if (kq > 0) probe_link_cal = true;
        if (kp > 0) { pstore_save(); Serial.printf("presence: %d link(s) calibrated\n", kp); verifying = true; verify_until = millis() + 30000; verify_active_ms = 0; }
        if (k > 0 || kp > 0) { store_save(); logger_drift_reset_all(); }
        Serial.printf("calibration: accepted, %d node(s) applied\n", k);
        char m[96]; snprintf(m, sizeof(m), "calibration accepted: nodes %s%s", who[0] ? who : "none",
                             (mesh_paired_count() > 0 && !probe_link_cal) ? " (probe link detector NOT recalibrated)" : "");
        logger_event(EV_CAL, 255, f->score, m);
    }
    int r = req_reset_node;
    if (r != -2) { req_reset_node = -2; cal_reset(&cal, sc, r); store_save(); for (int i = 0; i < SCORE_N; i++) if (r < 0 || r == i) { pres_link_reset(&sc->pres[i]); drift_reported[i] = false; relearn_seen[i] = 0; } p2p_cal_reset_node(r); pstore_save(); logger_drift_reset_all(); Serial.printf("calibration: reset %s to defaults\n", r < 0 ? "all" : "one node"); logger_event(EV_CAL, 255, f->score, r < 0 ? "calibration reset (all)" : "calibration reset (one node)"); }
    CalPhase before = cal.phase;
    if (cal.phase == CAL_CAPTURING)
        for (int i = 0; i < f->n_links; i++) {
            const LinkSample *l = &f->links[i];
            if (l->node_id < SCORE_N && (cal.target < 0 || cal.target == l->node_id)) pres_cal_add(&pacc[l->node_id], l->tstd, l->rssi_mean, l->win_seq);
        }
    cal_feed(&cal, f, sc, now);
    if (before == CAL_CAPTURING && cal.phase == CAL_PENDING)
        { for (int i = 0; i < SCORE_N; i++) ppend[i] = pres_cal_result(&pacc[i], pres_g_for_sens(cal.sens)); p2p_cal_done(pres_g_for_sens(cal.sens)); }
    if (before == CAL_CAPTURING && cal.phase == CAL_PENDING) Serial.println("calibration: capture complete, awaiting accept/discard");
    // learned baseline: report a drift past its limits once, save it to flash at most every 10 min and only when it moved
    for (int i = 0; i < SCORE_N; i++) {
        const PresLink &pl = sc->pres[i];
        if (pl.cal && pl.relearn_n != relearn_seen[i]) {                  // the RSSI baseline was re-learned: say so, keep it
            relearn_seen[i] = pl.relearn_n; char m[44]; snprintf(m, sizeof(m), "link %d base re-learned %.1f to %.1f dB", i, pl.relearn_from, pl.relearn_to);
            logger_event(EV_CAL, (uint8_t)i, 0, m); pstore_save();
        }
        if (pl.cal && pl.drift && !drift_reported[i]) {
            drift_reported[i] = true; char m[60]; snprintf(m, sizeof(m), "link %d baseline moved past its limits: recalibrate", i); logger_event(EV_DRIFT, (uint8_t)i, 0, m);
        }
    }
    if (now - last_learn_save > 600000UL) {
        last_learn_save = now;
        for (int i = 0; i < SCORE_N; i++) { const PresLink &pl = sc->pres[i]; if (pl.cal && (fabsf(pl.q - saved_q[i]) > 0.01f * pl.q0 || fabsf(pl.r - saved_r[i]) > 0.25f)) { pstore_save(); break; } }
    }
    if (verifying) {                                                       // 30 s after accepting: were there alerts? (you may be walking back)
        for (int i = 0; i < SCORE_N; i++) if (sc->pres[i].cal && sc->pres[i].state > PRES_CLEAR) { verify_active_ms += 50.0f; break; }
        if ((int32_t)(now - verify_until) >= 0) {
            verifying = false; char m[72];
            if (verify_active_ms < 1500.0f) snprintf(m, sizeof(m), "calibration check: quiet for 30 s after accepting");
            else snprintf(m, sizeof(m), "calibration check: %.0f s of alerts in the first 30 s (ignore if you were moving)", verify_active_ms / 1000.0f);
            logger_event(EV_CAL, 255, 0, m);
        }
    }
    xSemaphoreGive(lock);
}

bool calibration_request_start(int node, uint8_t sens) {
    if (node < -1 || node >= SCORE_N || cal.phase == CAL_CAPTURING || req_start_node != -2) return false;
    req_sens = sens; req_start_node = node;
    return true;
}
bool calibration_request_accept() { if (cal.phase != CAL_PENDING) return false; req_accept = true; return true; }
bool calibration_request_discard() { if (cal.phase != CAL_PENDING) return false; req_discard = true; return true; }
void calibration_request_reset(int node) { req_reset_node = node; }
const CalState *calibration_state() { return &cal; }
int calibration_phase() { return (int)cal.phase; }
void calibration_debug_shift_baseline(float db) {
    if (!sc) return;
    for (int i = 0; i < SCORE_N; i++) if (sc->pres[i].cal) { sc->pres[i].r += db; sc->pres[i].rssi_base = sc->pres[i].r; }
}
bool calibration_pending_quiet(const char **why) {
    bool any = false;
    for (int id = 0; id < SCORE_N; id++) {
        if (cal.pending[id].emf.valid || cal.pending[id].csi.valid || ppend[id].valid) any = true;
        // id 0 is the hub's own EMF wire only: its spikes are supply noise and it cannot see people (EMF_TEST.md), so it never vetoes.
        if ((id != 0 && cal.pending[id].warn) || ppend[id].warn) { *why = "movement during the capture"; return false; }
    }
    { bool pw; if (p2p_cal_pending_info(&pw)) { any = true; if (pw) { *why = "movement during the capture"; return false; } } }
    if (!any) { *why = "no node had enough data"; return false; }
    return true;
}
float calibration_left_s() { return cal_left_s(&cal, millis()); }

static void put_chan(JsonObject o, const CalChan &c, bool with_prev) {
    o["n"] = c.n; o["mean"] = roundf(c.mean * 100) / 100; o["sd"] = roundf(c.sd * 100) / 100;
    o["max"] = roundf(c.max * 100) / 100; o["scale"] = roundf(c.scale * 10) / 10;
    if (with_prev) { o["prev_mean"] = roundf(c.prev_mean * 100) / 100; o["prev_sd"] = roundf(c.prev_sd * 100) / 100; }
}

String calibration_json() {
    JsonDocument doc;
    static CalState snap;                    // ~3 KB: static so the async task stack stays small; guarded by `lock`
    static PresCalResult sp[SCORE_N];
    xSemaphoreTake(lock, portMAX_DELAY);
    snap = cal;
    memcpy(sp, ppend, sizeof(sp));
    xSemaphoreGive(lock);
    const char *ph[] = {"idle", "capturing", "pending"};
    doc["state"] = ph[snap.phase];
    doc["target"] = snap.target;
    doc["sens"] = snap.sens;
    doc["left"] = snap.phase == CAL_CAPTURING ? roundf(cal_left_s(&snap, millis()) * 10) / 10 : 0;
    JsonArray pa = doc["pending"].to<JsonArray>();
    if (snap.phase == CAL_PENDING)
        for (int id = 0; id < SCORE_N; id++) {
            const NodeCal &p = snap.pending[id];
            if (!p.emf.n && !p.csi.n && !sp[id].n) continue;
            JsonObject o = pa.add<JsonObject>();
            o["id"] = id; o["warn"] = p.warn || sp[id].warn; o["sens"] = p.sens;
            if (sp[id].n) { JsonObject q = o["pres"].to<JsonObject>(); q["ok"] = sp[id].valid; q["n"] = sp[id].n; q["thr"] = roundf(sp[id].thr * 10000) / 10000; q["max"] = roundf(sp[id].tmax * 10000) / 10000; q["mean"] = roundf(sp[id].tmean * 10000) / 10000; q["med"] = roundf(sp[id].q0 * 10000) / 10000; q["rssi"] = roundf(sp[id].rssi_base * 10) / 10; q["warn"] = sp[id].warn; q["sens"] = p.sens; }
            o["emf_ok"] = p.emf.valid; put_chan(o["emf"].to<JsonObject>(), p.emf, true);
            if (p.csi.n) { o["csi_ok"] = p.csi.valid; put_chan(o["csi"].to<JsonObject>(), p.csi, true); }
        }
    JsonArray aa = doc["applied"].to<JsonArray>();
    for (int id = 0; id < SCORE_N; id++) {
        const NodeCal &a = snap.applied[id];
        if (!a.emf.valid && !a.csi.valid && !sc->pres[id].cal) continue;
        JsonObject o = aa.add<JsonObject>();
        o["id"] = id; o["sens"] = a.sens;
        if (sc->pres[id].cal) { const PresLink &pl = sc->pres[id]; JsonObject q = o["pres"].to<JsonObject>(); q["thr"] = roundf(pl.thr * 10000) / 10000; q["rssi"] = roundf(pl.rssi_base * 10) / 10; q["q0"] = roundf(pl.q0 * 10000) / 10000; q["q"] = roundf(pl.q * 10000) / 10000; q["r0"] = roundf(pl.r0 * 10) / 10; q["drift"] = pl.drift; }
        if (a.emf.valid) { put_chan(o["emf"].to<JsonObject>(), a.emf, false); o["emf"]["raw"] = (int)a.emf.raw_mean; }
        if (a.csi.valid) put_chan(o["csi"].to<JsonObject>(), a.csi, false);
    }
    String out;
    serializeJson(doc, out);
    return out;
}
