// Field log writer (see fieldlog.h). Producers (Wi-Fi task, Core 1 loop) push framed records into a PSRAM ring under a
// spinlock; a low-priority task drains the ring into a LittleFS file so a flash stall never blocks sensing.
#include "fieldlog.h"
#include "field_format.h"
#include <LittleFS.h>
#include <Preferences.h>
#include <ArduinoJson.h>

#define FL_RING_BYTES   (256 * 1024)
#define FL_FLUSH_MS     5000
#define FL_MIN_FREE     (96 * 1024)         // stop logging when the filesystem has less free than this
#define FL_PATH         "/field.bin"
#define FL_NVS          "hubfield"

static uint8_t *ring = nullptr; static uint32_t ring_n = 0;
static volatile uint32_t head = 0, tail = 0, drops = 0;
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool on = false, full = false;
static File file; static uint32_t file_bytes = 0, boot_index = 0, session_bytes = 0, session_start_ms = 0;
static SemaphoreHandle_t fl_lock = nullptr;           // guards the file handle (writer task vs. erase/start/stop)
static uint32_t last_seq[SCORE_N], last_node_ms = 0; static float last_thr[SCORE_N], last_rb[SCORE_N];

const char *fieldlog_path() { return FL_PATH; }
bool fieldlog_on() { return on; }

static void push(uint8_t type, const uint8_t *payload, size_t len) {
    if (!on || !ring) return;
    uint8_t fr[FL_MAX_PAYLOAD + 4];
    size_t n = fl_frame(fr, type, payload, len);
    if (!n) return;
    portENTER_CRITICAL(&mux);
    if (ring_n - (head - tail) < n) { drops = drops + 1; portEXIT_CRITICAL(&mux); return; }
    for (size_t i = 0; i < n; i++) ring[(head + i) % ring_n] = fr[i];
    head = head + (uint32_t)n;
    portEXIT_CRITICAL(&mux);
}

static void rec_boot() {
    uint8_t p[FL_MAX_PAYLOAD]; char tag[40]; snprintf(tag, sizeof(tag), "build %s %s", __DATE__, __TIME__);
    fl_u32(p, millis()); fl_u32(p + 4, boot_index); size_t tl = strlen(tag); memcpy(p + 8, tag, tl);
    push(FR_BOOT, p, 8 + tl);
}

static void drain_once(bool force_flush) {
    static uint8_t buf[2048]; static uint32_t last_flush = 0;
    for (int rounds = 0; rounds < 8; rounds++) {
        uint32_t n = 0;
        portENTER_CRITICAL(&mux);
        uint32_t avail = head - tail; n = avail > sizeof(buf) ? sizeof(buf) : avail;
        for (uint32_t i = 0; i < n; i++) buf[i] = ring[(tail + i) % ring_n];
        tail = tail + n;
        portEXIT_CRITICAL(&mux);
        if (!n) break;
        xSemaphoreTake(fl_lock, portMAX_DELAY);
        if (file) { size_t w = file.write(buf, n); file_bytes += (uint32_t)w; session_bytes += (uint32_t)w; }
        xSemaphoreGive(fl_lock);
    }
    uint32_t now = millis();
    if (force_flush || now - last_flush >= FL_FLUSH_MS) {
        last_flush = now;
        xSemaphoreTake(fl_lock, portMAX_DELAY);
        if (file) file.flush();
        xSemaphoreGive(fl_lock);
        if (on && !full && LittleFS.totalBytes() - LittleFS.usedBytes() < FL_MIN_FREE) {
            full = true; on = false;
            xSemaphoreTake(fl_lock, portMAX_DELAY); if (file) file.close(); xSemaphoreGive(fl_lock);
            Serial.println("fieldlog: filesystem nearly full, logging stopped");
        }
    }
}

static void writer_task(void *) {
    for (;;) { vTaskDelay(pdMS_TO_TICKS(250)); if (ring) drain_once(false); }
}

static bool open_file() {
    xSemaphoreTake(fl_lock, portMAX_DELAY);
    if (file) file.close();
    file = LittleFS.open(FL_PATH, FILE_APPEND);
    bool ok = (bool)file;
    if (ok) file_bytes = file.size();
    xSemaphoreGive(fl_lock);
    return ok;
}

bool fieldlog_set(bool want) {
    if (want == on) return true;
    Preferences p;
    if (want) {
        if (LittleFS.totalBytes() - LittleFS.usedBytes() < FL_MIN_FREE) { full = true; return false; }
        if (!open_file()) return false;
        full = false; session_bytes = 0; session_start_ms = millis();
        for (int i = 0; i < SCORE_N; i++) { last_seq[i] = 0; last_thr[i] = 0; last_rb[i] = 0; }
        if (p.begin(FL_NVS, false)) { boot_index = p.getUInt("boots", 0) + 1; p.putUInt("boots", boot_index); p.putBool("on", true); p.end(); }
        on = true; rec_boot();
        Serial.println("fieldlog: logging started");
    } else {
        on = false; drain_once(true);
        xSemaphoreTake(fl_lock, portMAX_DELAY); if (file) file.close(); xSemaphoreGive(fl_lock);
        if (p.begin(FL_NVS, false)) { p.putBool("on", false); p.end(); }
        Serial.println("fieldlog: logging stopped");
    }
    return true;
}

void fieldlog_init() {
    fl_lock = xSemaphoreCreateMutex();
    ring_n = FL_RING_BYTES; ring = (uint8_t *)ps_malloc(ring_n);
    if (!ring) { ring_n = 24 * 1024; ring = (uint8_t *)malloc(ring_n); }
    if (!ring) { Serial.println("fieldlog: no memory for the ring, disabled"); return; }
    if (LittleFS.exists(FL_PATH)) { File f = LittleFS.open(FL_PATH, FILE_READ); file_bytes = f ? f.size() : 0; if (f) f.close(); }
    xTaskCreatePinnedToCore(writer_task, "fieldlog", 4096, nullptr, 1, nullptr, 1);
    Preferences p; bool was_on = false;
    if (p.begin(FL_NVS, true)) { was_on = p.getBool("on", false); boot_index = p.getUInt("boots", 0); p.end(); }
    Serial.printf("fieldlog: ring %u KB, file %u bytes, %s\n", (unsigned)(ring_n / 1024), (unsigned)file_bytes, was_on ? "resuming" : "off");
    if (was_on) { Preferences q; if (q.begin(FL_NVS, false)) { q.putBool("on", false); q.end(); } fieldlog_set(true); }   // re-arm through the normal path (new boot record)
}

void fieldlog_frame(const GridFrame *f, const ScoreState *s, uint32_t now_ms) {
    if (!on) return;
    uint8_t p[FL_MAX_PAYLOAD];
    if (now_ms - last_node_ms >= 500) {
        last_node_ms = now_ms;
        for (int i = 0; i < f->n_nodes; i++) {
            const NodeSample &n = f->nodes[i];
            fl_u32(p, now_ms); p[4] = n.node_id; p[5] = n.online ? 1 : 0; fl_u16(p + 6, n.emf_p2p);
            fl_u16(p + 8, (uint16_t)(int16_t)lroundf(n.temp_c * 100.0f)); fl_u16(p + 10, n.vbat_mv); p[12] = (uint8_t)n.rssi;
            float l10 = n.loss_pct * 10.0f; p[13] = (uint8_t)(l10 < 0 ? 0 : (l10 > 255 ? 255 : l10));
            fl_u16(p + 14, n.emf_raw);
            push(FR_NODE, p, 16);
        }
    }
    for (int i = 0; i < f->n_links; i++) {
        const LinkSample &l = f->links[i];
        if (l.node_id >= SCORE_N || !l.win_seq || l.win_seq == last_seq[l.node_id]) continue;
        last_seq[l.node_id] = l.win_seq;
        fl_u32(p, now_ms); p[4] = l.node_id;
        float t = l.tstd * 100000.0f; fl_u16(p + 5, (uint16_t)(t < 0 ? 0 : (t > 65535 ? 65535 : t)));
        fl_u16(p + 7, (uint16_t)(int16_t)lroundf(l.rssi_mean * 10.0f)); p[9] = l.pres; p[10] = l.pscore; p[11] = l.moving;
        push(FR_FEAT, p, 12);
    }
    for (int id = 0; id < SCORE_N; id++) {            // log the presence calibration whenever it changes (and once at the start)
        const PresLink &pl = s->pres[id];
        if (pl.cal && (last_thr[id] == 0 || fabsf(pl.thr - last_thr[id]) > 0.03f * last_thr[id] || fabsf(pl.rssi_base - last_rb[id]) > 0.5f)) {   // learning moves it slowly: log when the threshold moved 3 % or the baseline RSSI 0.5 dB
            last_thr[id] = pl.thr; last_rb[id] = pl.rssi_base;
            fl_u32(p, now_ms); p[4] = (uint8_t)id; fl_f32(p + 5, pl.thr); fl_f32(p + 9, pl.rssi_base);
            push(FR_CAL, p, 13);
        }
    }
}

void fieldlog_amp(int node, int8_t rssi, const uint8_t *amp52) {
    if (!on) return;
    uint8_t p[FL_MAX_PAYLOAD];
    fl_u32(p, millis()); p[4] = (uint8_t)node; p[5] = (uint8_t)rssi; memcpy(p + 6, amp52, 52);
    push(FR_AMP, p, 58);
}

bool fieldlog_label(const char *text) {
    if (!on) return false;
    uint8_t p[FL_MAX_PAYLOAD]; char s[28]; int n = 0;
    for (; text && *text && n < 24; text++) if (isalnum((unsigned char)*text) || *text == '_' || *text == '-') s[n++] = *text;
    if (!n) return false;
    fl_u32(p, millis()); memcpy(p + 4, s, n); push(FR_LABEL, p, 4 + n);
    return true;
}

void fieldlog_event(uint8_t type, uint8_t id, const char *text) {
    if (!on) return;
    uint8_t p[FL_MAX_PAYLOAD]; size_t n = strlen(text); if (n > 60) n = 60;
    fl_u32(p, millis()); p[4] = type; p[5] = id; memcpy(p + 6, text, n); push(FR_EVENT, p, 6 + n);
}

void fieldlog_clock(int64_t epoch_ms) {
    if (!on) return;
    uint8_t p[12]; fl_u32(p, millis()); fl_i64(p + 4, epoch_ms); push(FR_CLOCK, p, 12);
}

String fieldlog_status_json() {
    JsonDocument d;
    uint32_t freeb = LittleFS.totalBytes() - LittleFS.usedBytes();
    d["on"] = on; d["full"] = full; d["bytes"] = file_bytes; d["free"] = freeb; d["drops"] = drops; d["boots"] = boot_index;
    uint32_t up = (millis() - session_start_ms) / 1000;              // average rate since logging started (this session)
    float rate = (on && session_bytes && up > 10) ? (float)session_bytes / up : 1700.0f;   // measured ~1.65 KB/s with hub + 1 probe
    d["minutes_left"] = (int)((freeb > FL_MIN_FREE ? freeb - FL_MIN_FREE : 0) / rate / 60.0f);
    String out; serializeJson(d, out); return out;
}

bool fieldlog_erase() {
    bool was = on;
    if (was) fieldlog_set(false);
    xSemaphoreTake(fl_lock, portMAX_DELAY);
    bool ok = !LittleFS.exists(FL_PATH) || LittleFS.remove(FL_PATH);
    file_bytes = 0; full = false; drops = 0;
    xSemaphoreGive(fl_lock);
    if (was) fieldlog_set(true);
    return ok;
}
