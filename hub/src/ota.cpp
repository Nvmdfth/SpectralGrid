#include <Arduino.h>
#include <LittleFS.h>
#include <Update.h>
#include <MD5Builder.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <esp_ota_ops.h>
#include "config.h"
#include "hub.h"
#include "ota.h"
#include "logger.h"
#include "ota_auth.h"
#include "linkgate.h"
#include "fieldlog.h"
#include "calibration.h"

#define OTA_WINDOW_MS  300000u      // 5 min
#define OTA_HOLD_MS    8000u        // BOOT held this long opens the window (pairing opens at 3 s)
#define FW_PATH        "/fw/probe.bin"

static uint32_t window_until = 0, restart_at = 0, hold_start = 0;
static bool hold_latched = false;
// Armed probe update: queued while the OTA window is open, sent only once the probe's link has been stable (linkgate.h).
static LinkGate gates[SCORE_N]; static LinkGateCfg gcfg = lg_default_cfg();
static bool armed = false; static int arm_node = 0; static uint32_t arm_until = 0;
#define ARM_TTL_MS (2UL * 3600UL * 1000UL)
static void armed_poll();
static void gcfg_load() { Preferences p; if (p.begin("hubcfg", true)) { gcfg.gate_dbm = p.getFloat("og_gate", gcfg.gate_dbm); gcfg.spread_db = p.getFloat("og_spread", gcfg.spread_db); gcfg.loss_max = p.getFloat("og_loss", gcfg.loss_max); gcfg.stable_s = p.getFloat("og_stable", gcfg.stable_s); p.end(); } }
static void gcfg_save() { Preferences p; if (p.begin("hubcfg", false)) { p.putFloat("og_gate", gcfg.gate_dbm); p.putFloat("og_spread", gcfg.spread_db); p.putFloat("og_loss", gcfg.loss_max); p.putFloat("og_stable", gcfg.stable_s); p.end(); } }
// hub self-update state
static bool upd_active = false, upd_done = false; static String upd_err;
// probe firmware upload state
static File fw_file; static MD5Builder fw_md5; static size_t fw_size = 0; static bool fw_active = false, fw_done = false; static String fw_err;

void ota_open_window(uint32_t ms) { window_until = millis() + ms; if (!window_until) window_until = 1; logger_event(EV_SYSTEM, 255, 0, "OTA window open"); }
void ota_close_window() { window_until = 0; }
bool ota_window_open() { return window_until != 0 && (int32_t)(window_until - millis()) > 0; }

void ota_loop() {
    static bool cfg_loaded = false; if (!cfg_loaded) { cfg_loaded = true; gcfg_load(); }
    armed_poll();
    uint32_t now = millis();
    if (digitalRead(PIN_BOOT) == LOW) {
        if (!hold_start) hold_start = now;
        else if (!hold_latched && now - hold_start >= OTA_HOLD_MS) { hold_latched = true; ota_open_window(OTA_WINDOW_MS); Serial.println("ota: window open for 5 min (BOOT held 8 s)"); }
    } else { hold_start = 0; hold_latched = false; }
    if (restart_at && (int32_t)(now - restart_at) >= 0) { Serial.println("ota: rebooting"); delay(100); ESP.restart(); }
}

static void json_reply(AsyncWebServerRequest *req, int code, const String &body) { req->send(code, "application/json", body); }

static void fw_meta(uint32_t *size, String *md5) {
    Preferences p; *size = 0; *md5 = "";
    if (p.begin("hubcfg", false)) { *size = p.getUInt("fw_size", 0); *md5 = p.getString("fw_md5", ""); p.end(); }
}

// ---- hub self-update: POST /api/ota (multipart file field) --------------------------------------------------------
static void on_hub_upload(AsyncWebServerRequest *req, const String &, size_t index, uint8_t *data, size_t len, bool final) {
    if (index == 0) {
        upd_done = false; upd_err = "";
        if (!ota_window_open()) { upd_active = false; upd_err = "OTA window closed"; return; }
        upd_active = Update.begin(UPDATE_SIZE_UNKNOWN);
        if (!upd_active) { upd_err = Update.errorString(); return; }
        if (req->hasParam("md5")) Update.setMD5(req->getParam("md5")->value().c_str());
        Serial.println("ota: hub firmware upload started");
    }
    if (!upd_active) return;
    if (Update.write(data, len) != len) { upd_err = Update.errorString(); Update.abort(); upd_active = false; return; }
    if (final) {
        if (Update.end(true)) { upd_done = true; Serial.println("ota: hub firmware written and verified"); }
        else upd_err = Update.errorString();
        upd_active = false;
    }
}
static void on_hub_done(AsyncWebServerRequest *req) {
    if (upd_done) { restart_at = millis() + 1500; logger_event(EV_SYSTEM, 255, 0, "hub firmware updated, rebooting"); json_reply(req, 200, "{\"ok\":true,\"rebooting\":true}"); }
    else if (upd_err == "OTA window closed") json_reply(req, 403, "{\"error\":\"OTA window closed (hold BOOT 8 s, or type 'ota' on the serial console)\"}");
    else json_reply(req, 500, String("{\"error\":\"") + (upd_err.length() ? upd_err : String("upload incomplete")) + "\"}");
}

// ---- probe firmware storage: POST /api/fw/probe, served at GET /fw/probe.bin ----------------------------------------
static void on_fw_upload(AsyncWebServerRequest *, const String &, size_t index, uint8_t *data, size_t len, bool final) {
    if (index == 0) {
        fw_done = false; fw_err = ""; fw_size = 0;
        if (!ota_window_open()) { fw_active = false; fw_err = "OTA window closed"; return; }
        LittleFS.mkdir("/fw");
        fw_file = LittleFS.open(FW_PATH, FILE_WRITE);
        fw_active = (bool)fw_file;
        if (!fw_active) { fw_err = "cannot open file"; return; }
        fw_md5.begin();
    }
    if (!fw_active) return;
    if (fw_file.write(data, len) != len) { fw_err = "flash full"; fw_file.close(); LittleFS.remove(FW_PATH); fw_active = false; return; }
    fw_md5.add(data, len); fw_size += len;
    if (final) {
        fw_file.close(); fw_md5.calculate(); fw_active = false; fw_done = true;
        Preferences p; if (p.begin("hubcfg", false)) { p.putUInt("fw_size", fw_size); p.putString("fw_md5", fw_md5.toString()); p.end(); }
        Serial.printf("ota: probe firmware stored, %u bytes md5 %s\n", (unsigned)fw_size, fw_md5.toString().c_str());
    }
}
static void on_fw_done(AsyncWebServerRequest *req) {
    if (fw_done) { char b[96]; snprintf(b, sizeof(b), "{\"ok\":true,\"size\":%u,\"md5\":\"%s\"}", (unsigned)fw_size, fw_md5.toString().c_str()); json_reply(req, 200, b); }
    else if (fw_err == "OTA window closed") json_reply(req, 403, "{\"error\":\"OTA window closed\"}");
    else json_reply(req, 500, String("{\"error\":\"") + (fw_err.length() ? fw_err : String("upload incomplete")) + "\"}");
}

static bool hex16(const String &h, uint8_t *out) {
    if (h.length() != 32) return false;
    for (int i = 0; i < 16; i++) { char b[3] = {h[2 * i], h[2 * i + 1], 0}; char *e; out[i] = (uint8_t)strtoul(b, &e, 16); if (*e) return false; }
    return true;
}

// Build and send the authenticated OTA command to one probe (3 repeats). false + err when it cannot.
static bool send_probe_cmd(int node, const char **err) {
#if OTA_HAVE_SECRET
    uint8_t mac[6];
    if (node < 1 || !mesh_mac_for_node((uint8_t)node, mac)) { *err = "no such probe"; return false; }
    uint32_t sz; String md5; fw_meta(&sz, &md5);
    OtaCmd c; memset(&c, 0, sizeof(c));
    if (!sz || !hex16(md5, c.md5)) { *err = "no probe firmware stored"; return false; }
    c.h = make_hdr(PKT_OTA, NODE_ID_HUB); c.size = sz;
    ota_compute_tag(&c, c.tag);
    for (int i = 0; i < 3; i++) { mesh_send_raw(mac, (const uint8_t *)&c, sizeof(c)); delay(25); }
    return true;
#else
    (void)node; *err = "probe OTA not provisioned"; return false;
#endif
}

// What still stops an armed update from being sent (empty = go).
static void blockers(int node, JsonArray out) {
    uint32_t sz; String md5; fw_meta(&sz, &md5);
    uint8_t mac[6];
    if (!sz) out.add("no image stored");
    if (node < 1 || !mesh_mac_for_node((uint8_t)node, mac)) out.add("probe not paired");
    if (!lg_ready(&gates[node < SCORE_N && node >= 0 ? node : 1], &gcfg)) out.add("link not stable long enough");
    if (fieldlog_on()) out.add("field log running");
    if (calibration_phase() != 0) out.add("calibration in progress");
}

void ota_gate_feed(const GridFrame *f, uint32_t now_ms) {
    for (int i = 0; i < f->n_nodes; i++) {
        const NodeSample &n = f->nodes[i];
        if (n.node_id >= 1 && n.node_id < SCORE_N) lg_update(&gates[n.node_id], &gcfg, n.online, n.rssi, n.loss_pct, now_ms);
    }
}

static void armed_poll() {
    if (!armed) return;
    uint32_t now = millis(); char t[56];
    if ((int32_t)(now - arm_until) >= 0) { armed = false; snprintf(t, sizeof(t), "armed OTA for node %d expired", arm_node); logger_event(EV_SYSTEM, (uint8_t)arm_node, 0, t); return; }
    JsonDocument d; JsonArray b = d.to<JsonArray>(); blockers(arm_node, b);
    if (b.size()) return;
    const char *err = "";
    bool ok = send_probe_cmd(arm_node, &err);
    snprintf(t, sizeof(t), ok ? "OTA command sent to node %d (link stable)" : "armed OTA failed: %s", ok ? arm_node : 0, err);
    if (ok) snprintf(t, sizeof(t), "OTA command sent to node %d (link stable)", arm_node); else snprintf(t, sizeof(t), "armed OTA failed: %s", err);
    logger_event(EV_SYSTEM, (uint8_t)arm_node, 0, t);
    armed = false;
}

void ota_register(AsyncWebServer &server) {
    // NOTE: this server treats "/api/ota" as a prefix of "/api/ota/...": register the sub-paths first.
    server.on("/api/ota/status", HTTP_GET, [](AsyncWebServerRequest *req) {
        uint32_t sz; String md5; fw_meta(&sz, &md5);
        JsonDocument d;
        d["window"] = ota_window_open(); d["left_s"] = ota_window_open() ? (int)((window_until - millis()) / 1000) : 0;
        d["partition"] = esp_ota_get_running_partition() ? esp_ota_get_running_partition()->label : "?";
        d["sketch_md5"] = ESP.getSketchMD5(); d["probe_ota_enabled"] = (bool)OTA_HAVE_SECRET;
        if (sz) { JsonObject f = d["probe_fw"].to<JsonObject>(); f["size"] = sz; f["md5"] = md5; }
        String out; serializeJson(d, out); json_reply(req, 200, out);
    });
    // Armed (link-gated) probe update. Sub-paths first: this server matches "/api/ota/probe" as a prefix of them.
    server.on("/api/ota/probe/status", HTTP_GET, [](AsyncWebServerRequest *req) {
        int node = req->hasParam("node") ? req->getParam("node")->value().toInt() : (armed ? arm_node : 1);
        if (node < 1 || node >= SCORE_N) node = 1;
        JsonDocument d; uint32_t sz; String md5; fw_meta(&sz, &md5);
        d["window"] = ota_window_open(); d["window_left_s"] = ota_window_open() ? (int)((window_until - millis()) / 1000) : 0;
        d["armed"] = armed; d["node"] = armed ? arm_node : node;
        if (armed) d["expires_s"] = (int)((arm_until - millis()) / 1000);
        const LinkGate &g = gates[node];
        d["stable_s"] = g.ok_s; d["need_s"] = (int)gcfg.stable_s; d["rssi"] = g.rssi_now; d["spread"] = g.spread; d["loss"] = g.loss_now; d["online"] = g.online;
        d["ready"] = lg_ready(&g, &gcfg);
        JsonArray b = d["blockers"].to<JsonArray>(); blockers(node, b);
        JsonObject c = d["cfg"].to<JsonObject>(); c["gate"] = gcfg.gate_dbm; c["spread"] = gcfg.spread_db; c["loss"] = gcfg.loss_max; c["stable"] = (int)gcfg.stable_s;
        if (sz) { JsonObject f = d["fw"].to<JsonObject>(); f["size"] = sz; f["md5"] = md5; }
        d["probe_ota_enabled"] = (bool)OTA_HAVE_SECRET;
        String out; serializeJson(d, out); json_reply(req, 200, out);
    });
    server.on("/api/ota/probe/arm", HTTP_POST, [](AsyncWebServerRequest *req) {
        if (!OTA_HAVE_SECRET) return json_reply(req, 501, "{\"error\":\"probe OTA not provisioned\"}");
        if (!ota_window_open()) return json_reply(req, 403, "{\"error\":\"OTA window closed (hold BOOT 8 s, or type 'ota' on the serial console)\"}");
        int node = req->hasParam("node") ? req->getParam("node")->value().toInt() : 1;
        uint8_t mac[6]; uint32_t sz; String md5; fw_meta(&sz, &md5);
        if (node < 1 || !mesh_mac_for_node((uint8_t)node, mac)) return json_reply(req, 404, "{\"error\":\"no such probe\"}");
        if (!sz) return json_reply(req, 409, "{\"error\":\"no probe firmware stored: upload an image first\"}");
        armed = true; arm_node = node; arm_until = millis() + ARM_TTL_MS;
        char t[64]; snprintf(t, sizeof(t), "OTA armed for node %d: waits for a stable link", node); logger_event(EV_SYSTEM, (uint8_t)node, 0, t);
        json_reply(req, 200, "{\"ok\":true,\"armed\":true}");
    });
    server.on("/api/ota/probe/cancel", HTTP_POST, [](AsyncWebServerRequest *req) {
        if (armed) { armed = false; logger_event(EV_SYSTEM, (uint8_t)arm_node, 0, "armed OTA cancelled"); }
        json_reply(req, 200, "{\"ok\":true}");
    });
    server.on("/api/ota/gate", HTTP_POST, [](AsyncWebServerRequest *req) {          // thresholds for the armed update (persisted)
        auto num = [&](const char *k, float lo, float hi, float *dst) { if (req->hasParam(k)) { float v = req->getParam(k)->value().toFloat(); if (v >= lo && v <= hi) *dst = v; } };
        num("gate", -90, -20, &gcfg.gate_dbm); num("spread", 1, 20, &gcfg.spread_db); num("loss", 0, 20, &gcfg.loss_max); num("stable", 10, 3600, &gcfg.stable_s);
        gcfg_save(); json_reply(req, 200, "{\"ok\":true}");
    });
    server.on("/api/ota/probe", HTTP_POST, [](AsyncWebServerRequest *req) {          // immediate send (no link gate): the manual override
        if (!OTA_HAVE_SECRET) return json_reply(req, 501, "{\"error\":\"probe OTA not provisioned: create shared/ota_secret.h and rebuild hub and probes\"}");
        if (!ota_window_open()) return json_reply(req, 403, "{\"error\":\"OTA window closed\"}");
        if (!req->hasParam("node")) return json_reply(req, 400, "{\"error\":\"node required\"}");
        int node = req->getParam("node")->value().toInt();
        const char *err = "";
        if (!send_probe_cmd(node, &err)) return json_reply(req, strstr(err, "stored") ? 409 : 404, String("{\"error\":\"") + err + "\"}");
        char t[40]; snprintf(t, sizeof(t), "OTA command sent to node %d", node); logger_event(EV_SYSTEM, (uint8_t)node, 0, t);
        json_reply(req, 202, "{\"ok\":true}");
    });
    server.on("/api/ota/window", HTTP_POST, [](AsyncWebServerRequest *req) {          // convenience: close early
        if (req->hasParam("close")) { ota_close_window(); return json_reply(req, 200, "{\"ok\":true}"); }
        json_reply(req, 403, "{\"error\":\"the OTA window can only be opened by holding BOOT 8 s or from the serial console\"}");
    });
    server.on("/api/fw/probe", HTTP_POST, on_fw_done, on_fw_upload);
    server.on("/api/ota", HTTP_POST, on_hub_done, on_hub_upload);
    server.serveStatic("/fw", LittleFS, "/fw/").setFilter([](AsyncWebServerRequest *req) {    // a probe that fetches its image shows up in the event log
        if (req->url() == FW_PATH) { char t[64]; snprintf(t, sizeof(t), "probe firmware download from %s", req->client()->remoteIP().toString().c_str()); logger_event(EV_SYSTEM, 255, 0, t); }
        return true;
    });
}
