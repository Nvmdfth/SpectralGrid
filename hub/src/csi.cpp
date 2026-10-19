// Per-link CSI extraction (SpectralGrid.md §7.1). Callback runs in the Wi-Fi task: filter by source MAC, compute LLTF
// variance, accumulate; csi_snapshot() (Core 1) returns the mean since the previous call.
// Config choice: LLTF only (htltf off) so every frame type yields the same 128-byte layout and variances stay comparable.
// Hypothesis, verify on a real link: HT20 MCS0 ESP-NOW frames from the probe deliver LLTF CSI (open item #14).
#include <Arduino.h>
#include <esp_wifi.h>
#include <string.h>
#include "config.h"
#include "hub.h"
#include "dsp.h"
#include "csicap.h"
#include "csi_window.h"
#include "fieldlog.h"

typedef struct { float sum; uint32_t cnt, frames, last_ms; int8_t rssi; float last_mean; bool have_last; float tstd, rssi_mean; uint32_t win_seq; } Acc;
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
static CsiWin *wins[MESH_MAX_PROBES + 1];              // 5.3 KB each, allocated in csi_init (PSRAM if present)
static Acc acc[MESH_MAX_PROBES + 1];                 // by node_id; 0 = AP-client pseudo-link (standalone fallback)
static volatile uint32_t st_total, st_foreign, st_short;
static volatile uint8_t st_len, st_sig;
static uint8_t fb_mac[6]; static bool fb_valid; static uint32_t fb_last_ms;

static void csi_cb(void *, wifi_csi_info_t *info) {
    st_total = st_total + 1;
    if (!info || !info->buf || info->len < 128) { st_short = st_short + 1; return; }
    st_len = (uint8_t)(info->len > 255 ? 255 : info->len);
    st_sig = info->rx_ctrl.sig_mode;
    uint32_t now = millis();
    int id = mesh_node_for_mac(info->mac);
    csicap_push(info, id);
    if (id < 0) {
        // Standalone fallback (§7.1): with no probes paired, follow one AP client and report it as link 0.
        if (mesh_paired_count() > 0) { st_foreign = st_foreign + 1; return; }
        if (fb_valid && memcmp(fb_mac, info->mac, 6) != 0 && now - fb_last_ms < 5000) { st_foreign = st_foreign + 1; return; }
        memcpy(fb_mac, info->mac, 6); fb_valid = true; fb_last_ms = now;
        id = NODE_ID_HUB;
    }
    float v = csi_lltf_variance(info->buf);
    float wt = 0, wr = 0; uint8_t amp[CSIW_SC];
    static uint8_t amp_ctr[MESH_MAX_PROBES + 1];
    bool want_amp = fieldlog_on() && ((++amp_ctr[id]) & 1) == 0;           // every 2nd frame: ~25 frames/s per link into the field log
    bool wdone = wins[id] && csiw_push_amp(wins[id], info->buf, info->rx_ctrl.rssi, &wt, &wr, want_amp ? amp : nullptr);
    if (want_amp && wins[id]) fieldlog_amp(id, info->rx_ctrl.rssi, amp);
    portENTER_CRITICAL(&mux);
    Acc &a = acc[id];
    a.sum += v; a.cnt++; a.frames++; a.last_ms = now; a.rssi = info->rx_ctrl.rssi;
    if (wdone) { a.tstd = wt; a.rssi_mean = wr; a.win_seq++; }
    portEXIT_CRITICAL(&mux);
}

void csi_init() {
    csicap_init();
    for (int i = 0; i <= MESH_MAX_PROBES; i++) {
        wins[i] = (CsiWin *)ps_malloc(sizeof(CsiWin));
        if (!wins[i]) wins[i] = (CsiWin *)malloc(sizeof(CsiWin));
        if (wins[i]) csiw_reset(wins[i]);
    }
    wifi_csi_config_t cfg = {};
    cfg.lltf_en = true; cfg.htltf_en = false; cfg.stbc_htltf2_en = false; cfg.ltf_merge_en = false;
    cfg.channel_filter_en = false; cfg.manu_scale = false; cfg.shift = 0;
    esp_err_t a = esp_wifi_set_csi_config(&cfg);
    esp_err_t b = esp_wifi_set_csi_rx_cb(csi_cb, nullptr);
    esp_err_t c = esp_wifi_set_csi(true);
    Serial.printf("csi: config=%d cb=%d enable=%d\n", (int)a, (int)b, (int)c);
}

int csi_snapshot(LinkSample *out, int max) {
    int n = 0;
    uint32_t now = millis();
    for (int id = 0; id <= MESH_MAX_PROBES && n < max; id++) {
        Acc a;
        portENTER_CRITICAL(&mux);
        a = acc[id];
        acc[id].sum = 0; acc[id].cnt = 0;
        if (a.cnt) { acc[id].last_mean = a.sum / a.cnt; acc[id].have_last = true; }
        portEXIT_CRITICAL(&mux);
        float mean;
        if (a.cnt) mean = a.sum / a.cnt;
        else if (a.have_last && now - a.last_ms < CSI_STALE_MS) mean = a.last_mean;
        else continue;
        LinkSample &o = out[n++];
        o = LinkSample(); o.node_id = (uint8_t)id; o.csi_var = mean; o.seq = a.frames; o.rssi = a.rssi;
        o.tstd = a.tstd; o.rssi_mean = a.rssi_mean; o.win_seq = a.win_seq;
    }
    return n;
}

void csi_stats(uint32_t *total, uint32_t *foreign, uint32_t *too_short, uint8_t *last_len, uint8_t *last_sig) {
    *total = st_total; *foreign = st_foreign; *too_short = st_short; *last_len = st_len; *last_sig = st_sig;
}
