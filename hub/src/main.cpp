#ifndef BRINGUP
// SpectralGrid hub entry. Spec: SpectralGrid.md §7.
#include <Arduino.h>
#include <esp_task_wdt.h>
#include "config.h"
#include "hub.h"
#include "score.h"
#include "calibration.h"
#include "logger.h"
#include "ota.h"
#include "fieldlog.h"
#include "fieldcal.h"
#include "touch.h"
#include "p2p.h"
#include <Preferences.h>

// Radio watchdog. Once (2026-10-15) the hub stopped receiving radio frames altogether (console alive, every probe strobing HUB_LOST,
// CSI ~0.1/s) and only a reset cured it. With probes paired and fewer than RWD_MIN_FPS CSI frames/s in total (probes AND foreign
// Wi-Fi) for RWD_SECS, restart the hub: calibration and pairing live in flash and probes re-sync by themselves. Never during an OTA
// window or a calibration capture; at most RWD_MAX restarts in a row until the radio has worked for an hour, so a real fault (or
// every probe switched off where no other Wi-Fi exists) cannot loop. The next boot logs it.
#define RWD_MIN_FPS 2.0f
#define RWD_SECS    120
#define RWD_MAX     3
static void radio_watchdog(uint32_t now) {
    static uint32_t last_ms = 0, last_tot = 0, low_since = 0, ok_since = 0; static bool told = false, cleared = false;
    if (last_ms && now - last_ms < 5000) return;
    uint32_t tot, fo, sh; uint8_t ln, sg; csi_stats(&tot, &fo, &sh, &ln, &sg);
    if (!last_ms) { last_ms = now ? now : 1; last_tot = tot; return; }
    float fps = (tot - last_tot) * 1000.0f / (float)(now - last_ms); last_ms = now; last_tot = tot;
    if (!(fps < RWD_MIN_FPS && mesh_paired_count() > 0)) {
        low_since = 0;
        if (!ok_since) ok_since = now ? now : 1;
        else if (!cleared && now - ok_since > 3600000UL) { cleared = true; Preferences p; if (p.begin("hubwd", false)) { if (p.getUChar("n", 0)) p.putUChar("n", 0); p.end(); } }
        return;
    }
    ok_since = 0;
    if (!low_since) { low_since = now ? now : 1; return; }
    if (now - low_since < RWD_SECS * 1000UL || ota_window_open() || calibration_phase() == 1) return;
    Preferences p; if (!p.begin("hubwd", false)) return;
    uint8_t n = p.getUChar("n", 0);
    if (n >= RWD_MAX) { p.end(); if (!told) { told = true; logger_event(EV_SYSTEM, 255, 0, "radio watchdog: no frames received, restart limit reached"); } return; }
    p.putUChar("n", n + 1); p.putBool("fired", true); p.end();
    Serial.printf("radio watchdog: %.1f CSI frames/s for %d s with probes paired, restarting (%u of %d)\n", fps, RWD_SECS, n + 1, RWD_MAX);
    delay(200); ESP.restart();
}
static void radio_watchdog_boot_report() {
    Preferences p; if (!p.begin("hubwd", false)) return;
    if (p.getBool("fired", false)) { char m[64]; snprintf(m, sizeof(m), "restarted by the radio watchdog (%u of %d in a row)", p.getUChar("n", 0), RWD_MAX); logger_event(EV_SYSTEM, 255, 0, m); p.putBool("fired", false); }
    p.end();
}

static GridFrame frame;
static ScoreState score;
static TouchDet touch_det[SCORE_N];     // EMF-wire touch / near-hand events: separate from the presence score

void setup() {
    Serial.begin(115200);
    // Heavy Wi-Fi traffic (a client joining while a firmware upload runs) can keep the Wi-Fi task on core 0 busy for more than the default
    // 5 s, which tripped the IDLE0 watchdog and rebooted the hub (seen 2026-10-08, ppTask in the backtrace). Do not watch the idle tasks;
    // keep a long timeout for any task that does subscribe.
    { esp_task_wdt_config_t wdt = {}; wdt.timeout_ms = 20000; wdt.idle_core_mask = 0; wdt.trigger_panic = true; esp_task_wdt_reconfigure(&wdt); }
    led_init();
    score_init(&score);
    for (int i = 0; i < SCORE_N; i++) touch_reset(&touch_det[i], i == 0 ? 80.0f : 50.0f);   // the hub's raw level is noisier
    calibration_init(&score);
    p2p_init();
    logger_init(&score);
    radio_watchdog_boot_report();
    fieldlog_init();    // after LittleFS is mounted by logger_init; resumes a field log that was running before a power cycle
    mesh_init();       // WIFI_AP_STA, ch 6, ESP-NOW, NVS probe table
    web_init();        // SoftAP, DNS, /ws, REST (before CSI: AP start must not reset the CSI callback)
    csi_init();
    acquisition_start();   // pinned to Core 0
}

void loop() {  // Core 1: aggregation @ SCORE_HZ
    mesh_loop();
    web_loop();
    frame.n_nodes = 0;
    NodeSample n0;
    if (acquisition_get(&n0)) frame.nodes[frame.n_nodes++] = n0;
    frame.n_nodes += mesh_snapshot(frame.nodes + frame.n_nodes, MESH_MAX_PROBES);
    frame.n_links = csi_snapshot(frame.links, MESH_MAX_PROBES);
    calibration_feed(&frame);   // applies queued calibration requests, then accumulates; before scoring so Accept takes effect at once
    for (int i = 0; i < frame.n_nodes; i++) {           // EMF-wire touch events (an event, not part of the score)
        NodeSample &n = frame.nodes[i];
        if (n.node_id >= SCORE_N || !n.online) continue;
        if (touch_update(&touch_det[n.node_id], (float)n.emf_raw, millis())) {
            char m[44]; snprintf(m, sizeof(m), n.node_id ? "touch probe %u wire +%.0f" : "touch hub wire +%.0f", n.node_id ? (unsigned)n.node_id : 0u, touch_det[n.node_id].peak);
            if (n.node_id == 0) snprintf(m, sizeof(m), "touch hub wire +%.0f", touch_det[0].peak);
            logger_event(EV_TOUCH, n.node_id, 0, m);
            led_touch_pulse();
        }
        n.touch = touch_det[n.node_id].active ? 1 : 0;
    }
    p2p_loop(&frame, millis());       // probe-to-probe links: keep them on, feed their reports, fuse
    { int nc = 0; score.n_ext = (uint8_t)p2p_get_paths(score.ext, FUSE_MAX_PATHS, &nc, millis()); score.n_ext_cal = (uint8_t)nc; }
    score_compute(&score, &frame, millis());
    fieldlog_frame(&frame, &score, millis());
    fieldcal_loop();
    led_show(frame.score);
    uint8_t worst = (uint8_t)score.fused.state;   // lights follow the one alarm rule over hub + probe paths (0 = nothing calibrated)
    led_presence(worst);
    mesh_set_light(frame.score, (uint8_t)((worst & 3) | (calibration_phase() == 1 ? 4 : 0) | ((led_gesture_get() & 3) << 3) | (mesh_pairing_open() ? 32 : 0) | (led_touch_active() ? 64 : 0)));
    led_state(mesh_pairing_open(), calibration_phase() == 1);
    logger_observe(&frame, mesh_pairing_open(), calibration_phase());
    console_loop();
    radio_watchdog(millis());
    ota_gate_feed(&frame, millis());
    ota_loop();
    web_broadcast(&frame);
    static uint32_t last_print = 0;
    if (millis() - last_print >= 2000) {   // no UI yet: serial is the observability path
        last_print = millis();
        uint32_t tot, fo, sh; uint8_t ln, sg;
        csi_stats(&tot, &fo, &sh, &ln, &sg);
        static uint32_t last_win = 0;
        uint32_t w = acquisition_windows();
        NodeSample h = {};
        bool hv = acquisition_get(&h);
        Serial.printf("[hub] acq win/s=%.0f node0 %s emf=%u p2p=%u T=%.2f d=%.2f\n", (w - last_win) / 2.0f,
                      hv ? (h.online ? "ON " : "FAULT") : "none", h.emf_raw, h.emf_p2p, h.temp_c, h.d_temp);
        last_win = w;
        Serial.printf("[hub] paired=%d pairing=%d score=%u vec=%s | csi total=%u foreign=%u short=%u len=%u sig=%u\n",
                      mesh_paired_count(), (int)mesh_pairing_open(), frame.score, frame.vector,
                      (unsigned)tot, (unsigned)fo, (unsigned)sh, ln, sg);
        for (int i = 0; i < frame.n_nodes; i++) {
            const NodeSample &n = frame.nodes[i];
            if (n.node_id == NODE_ID_HUB) continue;
            float csi = -1, ts = -1; unsigned pr = 0; for (int j = 0; j < frame.n_links; j++) if (frame.links[j].node_id == n.node_id) { csi = frame.links[j].csi_var; ts = frame.links[j].tstd; pr = frame.links[j].pres; }
            Serial.printf("      node %u %s emf=%u p2p=%u T=%.2f d=%.2f rssi=%d loss=%.1f%% csi=%.2f tstd=%.4f pres=%u\n", n.node_id,
                          n.online ? "ON " : "off", n.emf_raw, n.emf_p2p, n.temp_c, n.d_temp, n.rssi, n.loss_pct, csi, ts, pr);
        }
    }
    delay(1000 / SCORE_HZ);
}
#endif  // !BRINGUP
