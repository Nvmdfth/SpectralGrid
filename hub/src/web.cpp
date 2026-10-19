// SoftAP, captive portal DNS, dashboard, /ws telemetry and REST (SpectralGrid.md §7.2, §7.3).
// AP is open by default (AP_PASS in config.h): anyone in range can reach /api/pair and the other controls. Fine for a
// field rig on a closed channel; set a WPA2 password for anything else.
#include <Arduino.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <math.h>
#include "config.h"
#include "hub.h"
#include "calibration.h"
#include "logger.h"
#include "ota.h"
#include "csicap.h"
#include "fieldlog.h"
#include "p2p.h"
#include <LittleFS.h>
#include <memory>
#include "gen/dashboard_gz.h"

static AsyncWebServer server(80);
static AsyncWebSocket ws("/ws");
static DNSServer dns;
static const IPAddress AP_IP(192, 168, 69, 1);   // own subnet: the home LAN is 192.168.4.0/22, so the Pi's wlan0 can stay joined to the hub
static char jbuf[4608];
static uint32_t last_cleanup_ms = 0;

static float r1(float v) { return roundf(v * 10.0f) / 10.0f; }
static float r2(float v) { return roundf(v * 100.0f) / 100.0f; }

static void serve_index(AsyncWebServerRequest *req) {
    AsyncWebServerResponse *r = req->beginResponse(200, "text/html", dashboard_gz, dashboard_gz_len);
    r->addHeader("Content-Encoding", "gzip");
    r->addHeader("Cache-Control", "no-store");
    req->send(r);
}

static void redirect_home(AsyncWebServerRequest *req) { req->redirect(String("http://") + AP_IP.toString() + "/"); }

static void json_reply(AsyncWebServerRequest *req, int code, const char *body) { req->send(code, "application/json", body); }

// node=all -> -1, node=N -> N (0..16). Anything else is rejected.
static bool parse_node(AsyncWebServerRequest *req, int *out) {
    if (!req->hasParam("node")) return false;
    String v = req->getParam("node")->value();
    if (v == "all") { *out = -1; return true; }
    if (v.length() == 0 || v.length() > 2) return false;
    for (size_t i = 0; i < v.length(); i++) if (v[i] < '0' || v[i] > '9') return false;
    int n = v.toInt();
    if (n > MESH_MAX_PROBES) return false;
    *out = n;
    return true;
}

static String qs(AsyncWebServerRequest *r, const char *k, const char *def = "") { return r->hasParam(k) ? r->getParam(k)->value() : String(def); }

void web_event(const Event *e) {
    if (ws.count() == 0) return;
    JsonDocument d; JsonObject o = d["ev"].to<JsonObject>();
    o["seq"] = e->seq; o["t"] = e->t_ms; o["epoch"] = e->epoch_s; o["type"] = ev_type_name(e->type);
    o["id"] = e->id == 255 ? -1 : (int)e->id; o["score"] = e->score; o["text"] = e->text;
    char b[320]; size_t n = serializeJson(d, b, sizeof(b));
    if (n > 0 && n < sizeof(b)) ws.textAll(b, n);
}

static void on_ws(AsyncWebSocket *, AsyncWebSocketClient *c, AwsEventType type, void *, uint8_t *, size_t) {
    if (type == WS_EVT_CONNECT) { Serial.printf("ws: client %u connected (%u total)\n", c->id(), (unsigned)ws.count()); logger_radio_activity(); }
    else if (type == WS_EVT_DISCONNECT) { Serial.printf("ws: client %u left\n", c->id()); logger_radio_activity(); }
}

void web_init() {
    WiFi.softAPConfig(AP_IP, AP_IP, IPAddress(255, 255, 255, 0));
    bool ok = WiFi.softAP(AP_SSID, AP_PASS, MESH_CHANNEL, 0, 4);
    WiFi.setTxPower(WIFI_POWER_13dBm);
    dns.start(53, "*", AP_IP);
    Serial.printf("web: softAP '%s' ch%d %s ip=%s\n", AP_SSID, MESH_CHANNEL, ok ? "up" : "FAILED", WiFi.softAPIP().toString().c_str());

    ws.onEvent(on_ws);
    server.addHandler(&ws);
    server.on("/", HTTP_GET, serve_index);
    // captive-portal probes (Android, iOS/macOS, Windows, Firefox)
    for (const char *p : {"/generate_204", "/gen_204", "/hotspot-detect.html", "/library/test/success.html", "/connecttest.txt",
                          "/ncsi.txt", "/fwlink", "/canonical.html", "/success.txt"})
        server.on(p, HTTP_GET, redirect_home);
    server.onNotFound(redirect_home);

    server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *req) {
        char b[160];
        snprintf(b, sizeof(b), "{\"paired\":%d,\"pairing\":%s,\"uptime_ms\":%u,\"heap\":%u,\"clients\":%u}",
                 mesh_paired_count(), mesh_pairing_open() ? "true" : "false", (unsigned)millis(), (unsigned)ESP.getFreeHeap(), (unsigned)ws.count());
        json_reply(req, 200, b);
    });
    server.on("/api/nodes", HTTP_GET, [](AsyncWebServerRequest *req) {
        NodeSample n[MESH_MAX_PROBES];
        int cnt = mesh_snapshot(n, MESH_MAX_PROBES);
        JsonDocument doc;
        JsonArray a = doc.to<JsonArray>();
        for (int i = 0; i < cnt; i++) {
            JsonObject o = a.add<JsonObject>();
            o["id"] = n[i].node_id; o["on"] = n[i].online ? 1 : 0; o["rssi"] = n[i].rssi; o["loss"] = r1(n[i].loss_pct);
        }
        String out;
        serializeJson(doc, out);
        json_reply(req, 200, out.c_str());
    });
    // Ground-truth labels for a serial CSI capture (started from the console only, like OTA). Harmless without one.
    server.on("/api/csi/status", HTTP_GET, [](AsyncWebServerRequest *req) { json_reply(req, 200, (csicap_on() || fieldlog_on()) ? "{\"on\":true}" : "{\"on\":false}"); });
    server.on("/api/csi/label", HTTP_POST, [](AsyncWebServerRequest *req) {
        if (!req->hasParam("l")) return json_reply(req, 400, "{\"error\":\"l required\"}");
        const char *lb = req->getParam("l")->value().c_str();
        bool ok = csicap_label(lb);
        ok = fieldlog_label(lb) || ok;                      // serial capture and/or the on-flash field log
        json_reply(req, ok ? 200 : 409, ok ? "{\"ok\":true}" : "{\"error\":\"capture is off\"}");
    });
    // Field log (FIELD_TEST.md): status, start/stop, download and erase. Sub-paths only: there is no bare /api/field route.
    server.on("/api/field/status", HTTP_GET, [](AsyncWebServerRequest *req) { json_reply(req, 200, fieldlog_status_json().c_str()); });
    server.on("/api/field/start", HTTP_POST, [](AsyncWebServerRequest *req) {
        bool ok = fieldlog_set(true); json_reply(req, ok ? 200 : 507, ok ? "{\"ok\":true}" : "{\"error\":\"cannot start (no space or file error)\"}");
    });
    server.on("/api/field/stop", HTTP_POST, [](AsyncWebServerRequest *req) { fieldlog_set(false); json_reply(req, 200, "{\"ok\":true}"); });
    server.on("/api/field/erase", HTTP_POST, [](AsyncWebServerRequest *req) {
        if (!req->hasParam("confirm")) return json_reply(req, 400, "{\"error\":\"confirm=1 required\"}");
        bool ok = fieldlog_erase(); json_reply(req, ok ? 200 : 500, ok ? "{\"ok\":true}" : "{\"error\":\"erase failed\"}");
    });
    server.on("/api/field/log", HTTP_GET, [](AsyncWebServerRequest *req) {
        if (!LittleFS.exists(fieldlog_path())) return json_reply(req, 404, "{\"error\":\"no field log yet\"}");
        req->send(LittleFS, fieldlog_path(), "application/octet-stream", true);
    });
    server.on("/api/probe/cmd", HTTP_POST, [](AsyncWebServerRequest *req) {       // node=<id|all>&op=<info|bcast|p2p|reboot>&arg=<0|1>
        if (!req->hasParam("node") || !req->hasParam("op")) return json_reply(req, 400, "{\"error\":\"node and op required\"}");
        String nv = req->getParam("node")->value();
        int op = proto_cmd_op(req->getParam("op")->value().c_str());
        int arg = req->hasParam("arg") ? req->getParam("arg")->value().toInt() : 0;
        if (op < 0) return json_reply(req, 400, "{\"error\":\"unknown op\"}");
        int node = nv == "all" ? 0 : nv.toInt();
        if (nv != "all" && (node < 1 || node > MESH_MAX_PROBES)) return json_reply(req, 400, "{\"error\":\"node must be 1-16 or all\"}");   // toInt() of junk is 0 = every probe
        bool ok = mesh_send_cmd(node, (uint8_t)op, arg);
        json_reply(req, ok ? 202 : 404, ok ? "{\"ok\":true}" : "{\"error\":\"no such probe, or probe commands not provisioned\"}");
    });
    server.on("/api/p2p", HTTP_GET, [](AsyncWebServerRequest *req) {            // last diagnostics per probe, incl. its peer-link numbers
        String o = "[";
        for (int id = 1; id <= MESH_MAX_PROBES; id++) {
            PktDiag d; uint32_t age;
            if (!mesh_get_diag((uint8_t)id, &d, &age)) continue;
            if (o.length() > 1) o += ",";
            char b[160]; snprintf(b, sizeof(b), "{\"node\":%d,\"age_ms\":%u,\"build\":\"%08x\",\"part\":%u,\"flags\":%u,\"up_s\":%u,\"heap_kb\":%u,\"peers\":[", id, (unsigned)age, (unsigned)d.build_id, d.part, d.flags, (unsigned)d.uptime_s, d.heap_kb * 4);
            o += b;
            for (int i = 0; i < d.npeers && i < DIAG_MAX_PEERS; i++) {
                snprintf(b, sizeof(b), "%s{\"node\":%u,\"fps\":%u,\"rssi\":%.1f,\"tstd\":%.4f,\"win\":%u}", i ? "," : "", d.peer[i].node, d.peer[i].fps, d.peer[i].rssi_x10 / 10.0f, d.peer[i].tstd_x10000 / 10000.0f, d.peer[i].win_seq);
                o += b;
            }
            o += "]}";
        }
        json_reply(req, 200, (o + "]").c_str());
    });
    server.on("/api/p2plinks", HTTP_GET, [](AsyncWebServerRequest *req) { json_reply(req, 200, p2p_json().c_str()); });      // fused probe-link presence + each link's state
    server.on("/api/p2penable", HTTP_POST, [](AsyncWebServerRequest *req) {      // on=0|1
        if (!req->hasParam("on")) return json_reply(req, 400, "{\"error\":\"on=0|1 required\"}");
        p2p_set_enabled(req->getParam("on")->value().toInt() != 0);
        json_reply(req, 200, p2p_enabled() ? "{\"enabled\":true}" : "{\"enabled\":false}");
    });
    server.on("/api/pair", HTTP_POST, [](AsyncWebServerRequest *req) {
        mesh_open_pairing(PAIR_WINDOW_MS);
        json_reply(req, 200, "{\"ok\":true,\"seconds\":60}");
    });
    server.on("/api/unpair", HTTP_POST, [](AsyncWebServerRequest *req) {
        if (!req->hasParam("id")) return json_reply(req, 400, "{\"error\":\"id required\"}");
        bool ok = mesh_unpair((uint8_t)req->getParam("id")->value().toInt());
        json_reply(req, ok ? 200 : 404, ok ? "{\"ok\":true}" : "{\"error\":\"no such node\"}");
    });
    server.on("/api/rate", HTTP_POST, [](AsyncWebServerRequest *req) {
        if (!req->hasParam("hz")) return json_reply(req, 400, "{\"error\":\"hz required\"}");
        bool ok = mesh_set_tx_hz((uint8_t)req->getParam("hz")->value().toInt());
        json_reply(req, ok ? 200 : 409, ok ? "{\"ok\":true}" : "{\"error\":\"slots would not fit\"}");
    });
    server.on("/api/thermal", HTTP_POST, [](AsyncWebServerRequest *req) {
        if (!req->hasParam("gate") || !calibration_set_thermal_gate(req->getParam("gate")->value().toFloat()))
            return json_reply(req, 400, "{\"error\":\"gate=0.5..3.0 required\"}");
        json_reply(req, 200, "{\"ok\":true}");
    });
    server.on("/api/thermal", HTTP_GET, [](AsyncWebServerRequest *req) {
        char b[80]; snprintf(b, sizeof(b), "{\"gate\":%.2f,\"min\":%.1f,\"max\":%.1f,\"default\":1.0}", calibration_thermal_gate(), THERMAL_GATE_MIN, THERMAL_GATE_MAX);
        json_reply(req, 200, b);
    });
    // Calibration (spec 9.2 item 5). NOTE: this server treats "/api/calibrate" as a prefix of "/api/calibrate/...", so the
    // sub-paths must be registered BEFORE the bare path or they would be swallowed by it.
    server.on("/api/calibrate/accept", HTTP_POST, [](AsyncWebServerRequest *req) {
        bool ok = calibration_request_accept();
        json_reply(req, ok ? 200 : 409, ok ? "{\"ok\":true}" : "{\"error\":\"nothing pending\"}");
    });
    server.on("/api/calibrate/discard", HTTP_POST, [](AsyncWebServerRequest *req) {
        bool ok = calibration_request_discard();
        json_reply(req, ok ? 200 : 409, ok ? "{\"ok\":true}" : "{\"error\":\"nothing pending\"}");
    });
    server.on("/api/calibrate/reset", HTTP_POST, [](AsyncWebServerRequest *req) {
        int node;
        if (!parse_node(req, &node)) return json_reply(req, 400, "{\"error\":\"node=all|0..16 required\"}");
        calibration_request_reset(node);
        json_reply(req, 200, "{\"ok\":true}");
    });
    server.on("/api/calibrate", HTTP_POST, [](AsyncWebServerRequest *req) {
        int node;
        if (!parse_node(req, &node)) return json_reply(req, 400, "{\"error\":\"node=all|0..16 required\"}");
        int sens = req->hasParam("sens") ? req->getParam("sens")->value().toInt() : 1;
        bool ok = calibration_request_start(node, (uint8_t)(sens < 0 || sens > 2 ? 1 : sens));
        { char b[40]; snprintf(b, sizeof(b), "{\"ok\":true,\"seconds\":%d}", CAL_CAPTURE_MS / 1000); json_reply(req, ok ? 202 : 409, ok ? b : "{\"error\":\"busy or bad node\"}"); }
    });
    server.on("/api/calibration", HTTP_GET, [](AsyncWebServerRequest *req) { json_reply(req, 200, calibration_json().c_str()); });
    // ---- event log, session recording, tags, control probe, clock (spec 9.2 items 6-8) ----
    // NOTE: this server prefix-matches ("/api/tag" also catches "/api/tag/..."), so sub-paths are registered first.
    server.on("/api/events.csv", HTTP_GET, [](AsyncWebServerRequest *req) {
        std::shared_ptr<EvCsvJob> job(logger_events_csv_open());
        AsyncWebServerResponse *r = req->beginChunkedResponse("text/csv", [job](uint8_t *buf, size_t max, size_t) -> size_t { return logger_events_csv_fill(job.get(), buf, max); });
        r->addHeader("Content-Disposition", "attachment; filename=\"spectralgrid_events.csv\"");
        req->send(r);
    });
    server.on("/api/events", HTTP_GET, [](AsyncWebServerRequest *req) {
        json_reply(req, 200, logger_events_json((uint32_t)strtoul(qs(req, "since", "0").c_str(), nullptr, 10), qs(req, "limit", "100").toInt()).c_str());
    });
    server.on("/api/session.csv", HTTP_GET, [](AsyncWebServerRequest *req) {
        std::shared_ptr<CsvStream> st(logger_session_csv_open());
        AsyncWebServerResponse *r = req->beginChunkedResponse("text/csv", [st](uint8_t *buf, size_t max, size_t) -> size_t { return logger_session_csv_fill(st.get(), buf, max); });
        r->addHeader("Content-Disposition", "attachment; filename=\"spectralgrid_session.csv\"");
        req->send(r);
    });
    server.on("/api/session/info", HTTP_GET, [](AsyncWebServerRequest *req) { json_reply(req, 200, logger_session_info_json().c_str()); });
    server.on("/api/session/frames", HTTP_GET, [](AsyncWebServerRequest *req) {
        uint32_t from = (uint32_t)strtoul(qs(req, "from", "0").c_str(), nullptr, 10);
        int limit = qs(req, "limit", "100").toInt(); if (limit < 1) limit = 1; if (limit > 120) limit = 120;
        SessFrame *fr = (SessFrame *)ps_malloc(sizeof(SessFrame) * limit);
        if (!fr) fr = (SessFrame *)malloc(sizeof(SessFrame) * limit);
        if (!fr) return json_reply(req, 503, "{\"error\":\"out of memory\"}");
        int n = logger_session_frames(from, limit, fr);
        AsyncResponseStream *r = req->beginResponseStream("application/json");
        r->print("{\"frames\":[");
        for (int i = 0; i < n; i++) {
            const SessFrame &f = fr[i];
            r->printf("%s{\"t\":%u,\"score\":%u,\"vector\":\"%s\",\"vec_id\":%d,\"nodes\":[", i ? "," : "", (unsigned)f.t_ms, f.score, sess_vec_name(f.vec), f.vec_id);
            for (int k = 0; k < f.n_nodes; k++) {
                const SessNode &x = f.nodes[k];
                r->printf("%s{\"id\":%u,\"on\":%u,\"emf\":%u,\"p2p\":%u,\"temp\":%.2f,\"d_temp\":%.2f,\"rssi\":%d,\"loss\":%u,\"vbat\":%u}", k ? "," : "", x.id, x.online, x.emf_raw, x.p2p, x.temp_c100 / 100.0f, x.d_temp_c100 / 100.0f, x.rssi, x.loss_pct, x.vbat_mv);
            }
            r->print("],\"links\":[");
            for (int k = 0; k < f.n_links; k++) r->printf("%s{\"id\":%u,\"csi\":%.2f}", k ? "," : "", f.links[k].id, f.links[k].csi100 / 100.0f);
            r->print("]}");
        }
        r->printf("],\"n\":%d}", n);
        free(fr);
        req->send(r);
    });
    server.on("/api/time", HTTP_POST, [](AsyncWebServerRequest *req) {
        int64_t ms = strtoll(qs(req, "ms", "0").c_str(), nullptr, 10);
        if (ms < 1600000000000LL) return json_reply(req, 400, "{\"error\":\"ms (epoch milliseconds) required\"}");
        logger_set_epoch_ms(ms);
        json_reply(req, 200, "{\"ok\":true}");
    });
    server.on("/api/tag/clear", HTTP_POST, [](AsyncWebServerRequest *req) {
        bool ok = logger_tag_clear(qs(req, "kind").c_str());
        json_reply(req, ok ? 200 : 404, ok ? "{\"ok\":true}" : "{\"error\":\"no such active tag\"}");
    });
    server.on("/api/tag", HTTP_POST, [](AsyncWebServerRequest *req) {
        String note = qs(req, "note"); note.trim();
        bool ok = logger_tag(qs(req, "kind").c_str(), note.c_str(), (uint32_t)strtoul(qs(req, "dur", "0").c_str(), nullptr, 10));
        json_reply(req, ok ? 200 : 400, ok ? "{\"ok\":true}" : "{\"error\":\"kind must be hvac|phone|appliance|person|pet|other|note (and fewer than 6 active tags)\"}");
    });
    server.on("/api/control", HTTP_POST, [](AsyncWebServerRequest *req) {
        String n = qs(req, "node");
        if (n == "none") { logger_set_control(-1); return json_reply(req, 200, "{\"ok\":true}"); }
        int id = n.toInt();
        if (n.length() == 0 || id < 1 || id > MESH_MAX_PROBES) return json_reply(req, 400, "{\"error\":\"node=1..16 or none\"}");
        logger_set_control(id);
        json_reply(req, 200, "{\"ok\":true}");
    });
    ota_register(server);
    server.begin();
}

void web_loop() {
    dns.processNextRequest();
    uint32_t now = millis();
    if (now - last_cleanup_ms >= 1000) { last_cleanup_ms = now; ws.cleanupClients(); }
    Event ev;
    while (logger_pop_event(&ev)) web_event(&ev);
}

void web_broadcast(const GridFrame *f) {
    if (ws.count() == 0) return;
    JsonDocument doc;
    doc["t"] = millis();
    doc["score"] = f->score;
    doc["vector"] = f->vector;
    doc["vec_id"] = f->vec_id;
    doc["top"] = f->top_ch; doc["top_v"] = f->top_val; doc["top_id"] = f->top_id;
    doc["pairing"] = mesh_pairing_open();
    int cph = calibration_phase();
    if (cph != 0) { JsonObject c = doc["cal"].to<JsonObject>(); c["s"] = cph == 1 ? "capturing" : "pending"; c["left"] = r1(calibration_left_s()); }
    JsonArray na = doc["nodes"].to<JsonArray>();
    for (int i = 0; i < f->n_nodes; i++) {
        const NodeSample &n = f->nodes[i];
        JsonObject o = na.add<JsonObject>();
        o["id"] = n.node_id; o["on"] = n.online ? 1 : 0; o["emf"] = n.emf_raw; o["p2p"] = n.emf_p2p;
        o["temp"] = r1(n.temp_c); o["d_temp"] = r2(n.d_temp); o["rssi"] = n.rssi; o["loss"] = r1(n.loss_pct);
        o["vbat"] = n.vbat_mv; o["ctl"] = (n.node_id == f->control_id) ? 1 : 0; if (n.touch) o["tc"] = 1;
    }
    String tg = logger_tags_json(), dr = logger_drift_json();
    if (tg.length() > 2) doc["tags"] = serialized(tg.c_str());
    if (dr.length() > 2) doc["drift"] = serialized(dr.c_str());
    JsonArray la = doc["links"].to<JsonArray>();
    for (int i = 0; i < f->n_links; i++) {
        JsonObject o = la.add<JsonObject>();
        o["id"] = f->links[i].node_id; o["csi"] = r2(f->links[i].csi_var);
        if (f->links[i].pres > 0) { o["pr"] = f->links[i].pres; o["ps"] = f->links[i].pscore; o["mv"] = f->links[i].moving; o["tr"] = r2(f->links[i].tratio); o["dr"] = r1(f->links[i].drop); }   // only once the link is calibrated
    }
    size_t n = serializeJson(doc, jbuf, sizeof(jbuf));
    if (n > 0 && n < sizeof(jbuf)) ws.textAll(jbuf, n);
}
