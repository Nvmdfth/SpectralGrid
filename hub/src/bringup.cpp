// Single-board bring-up diagnostics. Built only by `-e bringup` (-DBRINGUP); replaces main.cpp.
// Answers SpectralGrid.md §12 open items #1, #2, #3, #9 and the radio-bleed premise on GPIO 7.
// Serial report every 2 s at 115200. Phase 1 (0-10 s) = radio off, phase 2 = AP + ESP-NOW + CSI on.
#ifdef BRINGUP
#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <FastLED.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include "driver/temperature_sensor.h"
#include "protocol.h"
#include "dsp.h"

#define PIN_EMF   7
#define LED_PIN_A 48    // DevKitC-1 v1.0
#define LED_PIN_B 38    // DevKitC-1 v1.1
#define WIN       24
#define RADIO_ON_MS 10000

static CRGB ledA[1], ledB[1];
static temperature_sensor_handle_t tsens = nullptr;
static AsyncWebServer server(80);
static bool radio_on = false;

// sampler stats (single writer: sample_task on Core 0; reader copies, benign races for diagnostics)
static volatile uint32_t s_windows, s_dt_sum_us, s_dt_max_us, s_gap_max_us;
static volatile uint32_t s_p2p_sum, s_p2p_max, s_raw_min = 0xFFFFFFFF, s_raw_max;
// radio stats
static volatile uint32_t csi_count, csi_len, nowtx_ok, nowtx_err;
static volatile int8_t csi_rssi;
static volatile float csi_var;

static void sample_task(void *) {
    uint32_t w[WIN];
    TickType_t t = xTaskGetTickCount();
    uint32_t last_start = micros();
    for (;;) {
        uint32_t t0 = micros();
        uint32_t gap = t0 - last_start; last_start = t0;
        for (int i = 0; i < WIN; i++) w[i] = touchRead(PIN_EMF);
        uint32_t dt = micros() - t0;
        uint32_t lo = w[0], hi = w[0];
        for (int i = 1; i < WIN; i++) { if (w[i] < lo) lo = w[i]; if (w[i] > hi) hi = w[i]; }
        uint32_t p2p = hi - lo;
        s_windows++; s_dt_sum_us += dt; if (dt > s_dt_max_us) s_dt_max_us = dt;
        if (s_windows > 2 && gap > s_gap_max_us) s_gap_max_us = gap;
        s_p2p_sum += p2p; if (p2p > s_p2p_max) s_p2p_max = p2p;
        if (lo < s_raw_min) s_raw_min = lo;
        if (hi > s_raw_max) s_raw_max = hi;
        vTaskDelayUntil(&t, pdMS_TO_TICKS(5));
    }
}

static void csi_cb(void *, wifi_csi_info_t *info) {
    csi_count++;
    csi_len = info->len;
    csi_rssi = info->rx_ctrl.rssi;
    if (info->len >= 2 * 59) csi_var = csi_amp_variance(info->buf, 6, 58);
}

static void radio_start() {
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP("SPECTRAL_BRINGUP", nullptr, MESH_CHANNEL);
    WiFi.setTxPower(WIFI_POWER_13dBm);

    wifi_csi_config_t cfg = {};
    cfg.lltf_en = true; cfg.htltf_en = true; cfg.stbc_htltf2_en = true;
    cfg.ltf_merge_en = true; cfg.channel_filter_en = false; cfg.manu_scale = false; cfg.shift = 0;
    esp_wifi_set_csi_config(&cfg);
    esp_wifi_set_csi_rx_cb(csi_cb, nullptr);
    esp_wifi_set_csi(true);

    if (esp_now_init() == ESP_OK) {
        esp_now_peer_info_t peer = {};
        memset(peer.peer_addr, 0xFF, 6);
        peer.channel = 0; peer.ifidx = WIFI_IF_AP; peer.encrypt = false;
        esp_now_add_peer(&peer);
    }
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *r) {
        char b[160];
        snprintf(b, sizeof(b), "{\"csi_frames\":%u,\"csi_len\":%u,\"rssi\":%d,\"clients\":%d}",
                 (unsigned)csi_count, (unsigned)csi_len, (int)csi_rssi, WiFi.softAPgetStationNum());
        r->send(200, "application/json", b);
    });
    server.begin();
    radio_on = true;
}

void setup() {
    Serial.begin(115200);
    delay(1500);
    Serial.printf("\n== SpectralGrid bring-up | %s rev%d | cores=%d | flash=%uMB | psram=%uB ==\n",
                  ESP.getChipModel(), ESP.getChipRevision(), ESP.getChipCores(),
                  (unsigned)(ESP.getFlashChipSize() >> 20), (unsigned)ESP.getPsramSize());
    FastLED.addLeds<WS2812, LED_PIN_A, GRB>(ledA, 1);
    FastLED.addLeds<WS2812, LED_PIN_B, GRB>(ledB, 1);
    FastLED.setBrightness(20);

    temperature_sensor_config_t tc = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    esp_err_t e1 = temperature_sensor_install(&tc, &tsens);
    esp_err_t e2 = tsens ? temperature_sensor_enable(tsens) : ESP_FAIL;
    Serial.printf("temperature_sensor install=%d enable=%d\n", (int)e1, (int)e2);

    xTaskCreatePinnedToCore(sample_task, "sample", 4096, nullptr, 5, nullptr, 0);   // Core 0
    Serial.println("LED test: cycles R,G,B on GPIO48 AND GPIO38 - note which one lights");
}

void loop() {
    static uint32_t last = 0, n_report = 0, col = 0, last_csi = 0, last_win = 0;
    uint32_t now = millis();
    FastLED.showColor(CRGB(col % 3 == 0 ? 255 : 0, col % 3 == 1 ? 255 : 0, col % 3 == 2 ? 255 : 0));
    if (!radio_on && now >= RADIO_ON_MS) { radio_start(); Serial.println(">>> radio ON (AP ch6, 13 dBm, ESP-NOW, CSI)"); }
    if (radio_on) {
        static const uint8_t pkt[17] = {PROTO_MAGIC, PROTO_VERSION};
        const uint8_t bc[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        (esp_now_send(bc, pkt, sizeof(pkt)) == ESP_OK) ? nowtx_ok++ : nowtx_err++;
    }
    if (now - last >= 2000) {
        last = now; col++;
        uint32_t win = s_windows, dsum = s_dt_sum_us, dmax = s_dt_max_us, gmax = s_gap_max_us;
        uint32_t psum = s_p2p_sum, pmax = s_p2p_max, rmin = s_raw_min, rmax = s_raw_max;
        s_windows = 0; s_dt_sum_us = 0; s_dt_max_us = 0; s_gap_max_us = 0;
        s_p2p_sum = 0; s_p2p_max = 0; s_raw_min = 0xFFFFFFFF; s_raw_max = 0;
        float tc = NAN; if (tsens) temperature_sensor_get_celsius(tsens, &tc);
        uint32_t csi = csi_count;
        Serial.printf("[%2u] %s | win/s=%.0f read=%.0fus(max %u) gap_max=%uus | raw=%u..%u p2p avg=%.1f max=%u | T=%.1fC | heap=%u psram=%u\n",
                      (unsigned)n_report++, radio_on ? "RADIO" : "quiet",
                      win / 2.0f, win ? (float)dsum / win : 0.f, (unsigned)dmax, (unsigned)gmax,
                      (unsigned)rmin, (unsigned)rmax, win ? (float)psum / win : 0.f, (unsigned)pmax, tc,
                      (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram());
        if (radio_on)
            Serial.printf("      csi=%.0f/s len=%u rssi=%d var=%.2f | clients=%d | espnow tx ok=%u err=%u\n",
                          (csi - last_csi) / 2.0f, (unsigned)csi_len, (int)csi_rssi, csi_var,
                          WiFi.softAPgetStationNum(), (unsigned)nowtx_ok, (unsigned)nowtx_err);
        last_csi = csi; last_win = win; (void)last_win;
    }
    delay(20);
}
#endif  // BRINGUP
