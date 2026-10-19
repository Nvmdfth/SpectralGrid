#include <Arduino.h>
#include <LittleFS.h>
#include "fieldlog.h"
#include <Preferences.h>
#include <ArduinoJson.h>
#include <string.h>
#include "logger.h"
#include "config.h"
#include "calibration.h"
#include "tags.h"
#include "drift.h"
#include "ringlog.h"

#define SESSION_BYTES   (2u * 1024 * 1024)
#define SESSION_DIV     10            // record every 10th frame: 2 Hz at the 20 Hz aggregation rate
#define STORE_PATH      "/events.tsv"
#define STORE_MAX_BYTES (48 * 1024)
#define STORE_LOAD_N    200

static SemaphoreHandle_t lk = nullptr;
static EventLog elog;
static RingLog sess;
static uint8_t *sess_buf = nullptr;
static EventDetector det;
static TagSet tags;
static DriftState drift;
static ScoreState *sc = nullptr;
static QueueHandle_t evq = nullptr;
static int64_t epoch_off_ms = 0;
static bool epoch_known = false;
static uint32_t frame_n = 0, store_bytes = 0;

// ---- persistence -----------------------------------------------------------------------------------------------
static void flat(char *dst, size_t n, const char *src) {
    size_t i = 0; for (; src[i] && i + 1 < n; i++) dst[i] = (src[i] == '\t' || src[i] == '\n' || src[i] == '\r') ? ' ' : src[i]; dst[i] = 0;
}
static void store_append(const Event &e) {
    File f = LittleFS.open(STORE_PATH, FILE_APPEND);
    if (!f) return;
    char t[sizeof(e.text)]; flat(t, sizeof(t), e.text);
    char line[100]; int n = snprintf(line, sizeof(line), "%u\t%u\t%u\t%u\t%u\t%s\n", (unsigned)e.epoch_s, (unsigned)e.t_ms, e.type, e.id, e.score, t);
    f.write((const uint8_t *)line, n);
    store_bytes += n;
    f.close();
    if (store_bytes > STORE_MAX_BYTES) {                      // keep the newest half
        File r = LittleFS.open(STORE_PATH, FILE_READ);
        if (!r) return;
        r.seek(r.size() / 2);
        r.readStringUntil('\n');                              // realign to a line start
        String keep = r.readString(); r.close();
        File w = LittleFS.open(STORE_PATH, FILE_WRITE);
        if (w) { w.print(keep); w.close(); store_bytes = keep.length(); }
    }
}
static void store_load() {
    File f = LittleFS.open(STORE_PATH, FILE_READ);
    if (!f) return;
    store_bytes = f.size();
    Event *ring = (Event *)malloc(sizeof(Event) * STORE_LOAD_N); int cnt = 0, head = 0;
    if (!ring) { f.close(); return; }
    while (f.available()) {
        String l = f.readStringUntil('\n'); if (l.length() < 8) continue;
        Event e; memset(&e, 0, sizeof(e)); unsigned ep, t, ty, id, sc2; char tx[48] = "";
        if (sscanf(l.c_str(), "%u\t%u\t%u\t%u\t%u\t%47[^\n]", &ep, &t, &ty, &id, &sc2, tx) < 5) continue;
        e.epoch_s = ep; e.t_ms = 0; e.type = (uint8_t)ty; e.id = (uint8_t)id; e.score = (uint8_t)sc2; strncpy(e.text, tx, sizeof(e.text) - 1);
        ring[head] = e; head = (head + 1) % STORE_LOAD_N; if (cnt < STORE_LOAD_N) cnt++;
    }
    f.close();
    int start = (head + STORE_LOAD_N - cnt) % STORE_LOAD_N;
    for (int i = 0; i < cnt; i++) { const Event &e = ring[(start + i) % STORE_LOAD_N]; evlog_push(&elog, 0, e.epoch_s, e.type, e.id, e.score, e.text); }
    free(ring);
}

// ---- events ----------------------------------------------------------------------------------------------------
static uint32_t epoch_s_now() { return epoch_known ? (uint32_t)(((int64_t)millis() + epoch_off_ms) / 1000) : 0; }

static void sink(const Event *e, void *) {
    xSemaphoreTake(lk, portMAX_DELAY);
    const Event *s = evlog_push(&elog, millis(), epoch_s_now(), e->type, e->id, e->score, e->text);
    Event copy = *s;
    xSemaphoreGive(lk);
    store_append(copy);
    fieldlog_event(copy.type, copy.id, copy.text);
    xQueueSend(evq, &copy, 0);                                // broadcast happens from the Core 1 loop (web is not thread-safe)
    Serial.printf("event #%u [%s] %s\n", (unsigned)copy.seq, ev_type_name(copy.type), copy.text);
}
void logger_event(uint8_t type, uint8_t id, uint8_t score, const char *text) { evdet_emit(&det, type, id, score, text); }
bool logger_pop_event(Event *out) { return evq && xQueueReceive(evq, out, 0) == pdTRUE; }
void logger_radio_activity() { evdet_radio_activity(&det, millis()); }

void logger_set_epoch_ms(int64_t ms) {
    xSemaphoreTake(lk, portMAX_DELAY);
    bool first = !epoch_known;
    epoch_off_ms = ms - (int64_t)millis(); epoch_known = true;
    xSemaphoreGive(lk);
    fieldlog_clock(ms);
    if (first) logger_event(EV_SYSTEM, 255, 0, "clock set from phone");
}
bool logger_epoch_known() { return epoch_known; }

// ---- tags / control / drift ------------------------------------------------------------------------------------
bool logger_tag(const char *kind, const char *note, uint32_t dur_s) {
    if (!tag_kind_valid(kind)) return false;
    char t[48];
    xSemaphoreTake(lk, portMAX_DELAY);
    bool ok = tags_add(&tags, kind, note, dur_s, millis());
    xSemaphoreGive(lk);
    if (!ok) return false;
    bool has_note = note && note[0];
    if (dur_s) snprintf(t, sizeof(t), "%s %um%s%.22s", kind, (unsigned)((dur_s + 59) / 60), has_note ? ": " : "", has_note ? note : "");   // "hvac 10m: furnace"
    else snprintf(t, sizeof(t), "%s%s%.30s", kind, has_note ? ": " : "", has_note ? note : "");                                              // "note: smelled smoke"
    logger_event(EV_TAG, 255, 0, t);
    return true;
}
bool logger_tag_clear(const char *kind) {
    xSemaphoreTake(lk, portMAX_DELAY); bool ok = tags_clear(&tags, kind); xSemaphoreGive(lk);
    if (ok) { char t[32]; snprintf(t, sizeof(t), "tag %s cleared", kind); logger_event(EV_TAG, 255, 0, t); }
    return ok;
}
String logger_tags_json() {
    Tag t[TAG_MAX]; uint32_t left[TAG_MAX];
    xSemaphoreTake(lk, portMAX_DELAY); int n = tags_active(&tags, millis(), t, left, TAG_MAX); xSemaphoreGive(lk);
    JsonDocument d; JsonArray a = d.to<JsonArray>();
    for (int i = 0; i < n; i++) { JsonObject o = a.add<JsonObject>(); o["k"] = t[i].kind; o["note"] = t[i].note; o["left"] = left[i]; }
    String out; serializeJson(d, out); return out;
}
void logger_set_control(int id) {
    score_set_control(sc, id);
    Preferences p; if (p.begin("hubcfg", false)) { p.putChar("ctl", (int8_t)sc->control_id); p.end(); }
    char t[40]; if (sc->control_id >= 0) snprintf(t, sizeof(t), "node %d set as control probe", sc->control_id); else snprintf(t, sizeof(t), "control probe cleared");
    logger_event(EV_CONTROL, sc->control_id >= 0 ? sc->control_id : 255, 0, t);
}
String logger_drift_json() {
    JsonDocument d; JsonArray a = d.to<JsonArray>();
    for (int id = 0; id < SCORE_N; id++) { DriftKind k = drift_kind(&drift, id); if (k) { JsonObject o = a.add<JsonObject>(); o["id"] = id; o["r"] = drift_kind_name(k); } }
    String out; serializeJson(d, out); return out;
}
void logger_drift_reset_all() { for (int id = 0; id < SCORE_N; id++) drift_reset_node(&drift, id); }

// ---- init / observe --------------------------------------------------------------------------------------------
static void drift_event(int id, DriftKind k, const char *msg, void *) { (void)k; logger_event(EV_DRIFT, (uint8_t)id, 0, msg); }

void logger_init(ScoreState *s) {
    sc = s;
    lk = xSemaphoreCreateMutex();
    evq = xQueueCreate(24, sizeof(Event));
    if (!LittleFS.begin(true)) Serial.println("logger: LittleFS mount FAILED (events will not persist)");
    sess_buf = (uint8_t *)ps_malloc(SESSION_BYTES);
    uint32_t cap = SESSION_BYTES;
    if (!sess_buf) { cap = 64 * 1024; sess_buf = (uint8_t *)malloc(cap); }
    ringlog_init(&sess, sess_buf, sess_buf ? cap : 0);
    evlog_init(&elog);
    store_load();
    evdet_init(&det, sink, nullptr);
    tags_init(&tags);
    drift_init(&drift, drift_event, nullptr);
    Preferences p; if (p.begin("hubcfg", false)) { int8_t c = p.getChar("ctl", -1); score_set_control(sc, c); p.end(); }
    Serial.printf("logger: session ring %u KB (%s), %u stored events, control=%d\n", (unsigned)(cap / 1024), psramFound() && cap == SESSION_BYTES ? "PSRAM" : "heap", (unsigned)elog.count, sc->control_id);
    logger_event(EV_SYSTEM, 255, 0, "hub boot");
}

void logger_observe(const GridFrame *f, bool pairing, int cal_phase) {
    uint32_t now = millis();
    evdet_observe(&det, f, pairing, cal_phase, now);
    const CalState *cs = calibration_state();
    if (cs && sc) { for (int id = 0; id < SCORE_N; id++) drift_set_moved(&drift, id, (sc->moved_mask >> id) & 1u); drift_update(&drift, f, cs, sc, now); }
    if (++frame_n % SESSION_DIV == 0 && sess_buf) {
        uint8_t rec[SESS_MAX_REC];
        uint16_t n = sess_encode(f, now, rec, sizeof(rec));
        if (n) { xSemaphoreTake(lk, portMAX_DELAY); ringlog_push(&sess, rec, n); xSemaphoreGive(lk); }
    }
}

// ---- queries ---------------------------------------------------------------------------------------------------
String logger_events_json(uint32_t since, int limit) {
    if (limit < 1) limit = 1; if (limit > 300) limit = 300;
    Event *buf = (Event *)malloc(sizeof(Event) * limit);
    if (!buf) return "{\"events\":[]}";
    xSemaphoreTake(lk, portMAX_DELAY);
    uint32_t total = elog.count, last = elog.next_seq - 1;
    int n = evlog_tail(&elog, since, buf, limit);
    xSemaphoreGive(lk);
    JsonDocument d; JsonArray a = d["events"].to<JsonArray>();
    for (int i = 0; i < n; i++) {
        JsonObject o = a.add<JsonObject>();
        o["seq"] = buf[i].seq; o["t"] = buf[i].t_ms; o["epoch"] = buf[i].epoch_s; o["type"] = ev_type_name(buf[i].type);
        o["id"] = buf[i].id == 255 ? -1 : (int)buf[i].id; o["score"] = buf[i].score; o["text"] = buf[i].text;
    }
    d["last"] = last; d["total"] = total; d["epoch_known"] = epoch_known;
    free(buf);
    String out; serializeJson(d, out); return out;
}

String logger_session_info_json() {
    xSemaphoreTake(lk, portMAX_DELAY);
    uint32_t cnt = sess.count, oldest = 0, newest = 0;
    if (cnt) {
        RingIter it; ringlog_iter(&sess, &it); uint8_t rec[SESS_MAX_REC]; uint16_t n; uint32_t last = 0;
        bool first = true;
        while (ringlog_next(&sess, &it, rec, sizeof(rec), &n)) { last = sess_time(rec); if (first) { oldest = last; first = false; } }
        newest = last;
    }
    uint32_t cap = sess.cap, used = sess.used;
    xSemaphoreGive(lk);
    JsonDocument d;
    d["count"] = cnt; d["oldest_ms"] = oldest; d["newest_ms"] = newest; d["rate_hz"] = 20 / SESSION_DIV;
    d["bytes_used"] = used; d["bytes_cap"] = cap; d["epoch_known"] = epoch_known; d["epoch_off_ms"] = (double)epoch_off_ms;
    String out; serializeJson(d, out); return out;
}

int logger_session_frames(uint32_t from_ms, int limit, SessFrame *out) {
    int n = 0;
    xSemaphoreTake(lk, portMAX_DELAY);
    RingIter it;
    if (sess_seek(&sess, &it, from_ms)) {
        uint8_t rec[SESS_MAX_REC]; uint16_t len;
        while (n < limit && ringlog_next(&sess, &it, rec, sizeof(rec), &len)) { if (sess_decode(rec, len, &out[n])) n++; }
    }
    xSemaphoreGive(lk);
    return n;
}

CsvStream *logger_session_csv_open() {
    CsvStream *c = new CsvStream;
    csv_begin(c, &sess, epoch_known, epoch_off_ms);
    return c;
}
size_t logger_session_csv_fill(CsvStream *c, uint8_t *out, size_t max) {
    xSemaphoreTake(lk, portMAX_DELAY); size_t n = csv_fill(c, out, max); xSemaphoreGive(lk); return n;
}
EvCsvJob *logger_events_csv_open() {
    EvCsvJob *j = new EvCsvJob; j->snap = (Event *)ps_malloc(sizeof(Event) * EV_LOG_N);
    if (!j->snap) j->snap = (Event *)malloc(sizeof(Event) * EV_LOG_N);
    int n = 0;
    if (j->snap) { xSemaphoreTake(lk, portMAX_DELAY); n = evlog_snapshot(&elog, j->snap, EV_LOG_N); xSemaphoreGive(lk); }
    evcsv_begin(&j->csv, j->snap, (uint32_t)n, epoch_known, epoch_off_ms);
    return j;
}
size_t logger_events_csv_fill(EvCsvJob *j, uint8_t *out, size_t max) { return evcsv_fill(&j->csv, out, max); }
