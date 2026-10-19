#pragma once
#include <stdint.h>
#include "protocol.h"

typedef struct { uint8_t node_id; uint16_t emf_raw, emf_p2p; float temp_c, d_temp; bool online; int8_t rssi; float loss_pct; uint16_t vbat_mv; uint8_t touch; } NodeSample;   // vbat_mv 0 = unknown
typedef struct {
    uint8_t node_id; float csi_var; uint32_t seq; int8_t rssi;
    float tstd, rssi_mean; uint32_t win_seq;    // csi_window.h result of the latest 1 s window (win_seq 0 = none yet)
    uint8_t pres, pscore, moving;               // presence outputs (PresState, 0..100, bool); filled by score_compute
    float tratio, drop;                         // calibrated links only: tstd / threshold (>1 = moving), baseline RSSI minus the window RSSI in dB (>1.5 = shadowed)
} LinkSample;

typedef struct {
    uint8_t score;
    char vector[24];
    int8_t vec_id;              // node/link id the vector refers to, -1 if none
    uint8_t top_ch, top_val;    // strongest channel behind the vector: 0 none, 1 EMF, 2 RF/CSI, 3 thermal, 4 presence; its 0..100 score
    int8_t top_id;              // node that channel belongs to, -1 if none
    int8_t control_id;          // probe designated as the reference/control node, -1 if none (set by score_compute)
    uint8_t n_nodes, n_links;
    NodeSample nodes[MESH_MAX_PROBES + 1];
    LinkSample links[MESH_MAX_PROBES];
} GridFrame;

// acquisition.cpp (Core 0)
void acquisition_start();
bool acquisition_get(NodeSample *out);   // hub's own node 0 (id 0); false until the first valid window or on sensor fault
uint32_t acquisition_windows();            // windows completed since boot (for rate diagnostics)
// mesh.cpp: probe table, pairing, HubSync, ESP-NOW RX (WiFi AP_STA, ch 6, ESP-NOW on the STA interface)
void mesh_init();
void mesh_loop();
void mesh_set_light(uint8_t score, uint8_t state);   // what the next HubSync broadcasts so probes mirror the hub's lights (see protocol.h)                          // call >= 10 Hz from Core 1
int  mesh_snapshot(NodeSample *out, int max);          // paired probes only (node 0 comes from acquisition)
int  mesh_node_for_mac(const uint8_t *mac);            // node_id or -1; safe from the Wi-Fi task
int  mesh_paired_count();
bool mesh_mac_for_node(uint8_t node_id, uint8_t *mac_out);   // false if the node is not paired
bool mesh_send_raw(const uint8_t *mac, const uint8_t *data, size_t len);   // unicast ESP-NOW frame (used for OTA commands)
void mesh_open_pairing(uint32_t ms);
bool mesh_get_diag(uint8_t node_id, PktDiag *out, uint32_t *age_ms);   // last PKT_DIAG from a probe
bool mesh_diag_old(uint8_t node_id);                    // its PKT_DIAG has another layout: probe firmware older than the hub's
void mesh_clear_diag_old(uint8_t node_id);              // forget that (the probe reconnected: it may have been updated)
bool mesh_send_cmd(int node, uint8_t op, int32_t arg);   // node 0 = all paired probes; false if none/not provisioned
bool mesh_pairing_open();
bool mesh_unpair(uint8_t node_id);
bool mesh_set_tx_hz(uint8_t hz);                       // false if slots would not fit; re-acks all probes on success
// csi.cpp: per-MAC CSI callback (call after mesh_init)
void csi_init();
int  csi_snapshot(LinkSample *out, int max);           // mean variance since last call; stale links omitted
void csi_stats(uint32_t *total, uint32_t *foreign, uint32_t *too_short, uint8_t *last_len, uint8_t *last_sig);
// score.h: stateful scoring (§8); included where needed
// web.cpp (Core 1)
void web_init();                           // SoftAP (ch 6), captive DNS, HTTP, /ws
void web_loop();                           // DNS + websocket housekeeping; call every loop
void web_broadcast(const GridFrame *f);
// console.cpp: serial commands (pair, ota, status, time)
void console_loop();
// led.cpp
void led_init();                           // starts the LED task (Core 1, 100 Hz)
void led_show(uint8_t score);              // composite score meter (§7.2)
uint8_t led_gesture_get();                 // current BOOT-gesture mode (for the probe lights)
void led_touch_pulse();                    // EMF-wire touch event: blue/white flash sequence for 3 s
bool led_touch_active();
void led_gesture(uint8_t mode);            // BOOT-gesture feedback: 0 none, 1 countdown, 2 rejected, 3 accepted
void led_presence(uint8_t pres);           // worst link PresState (0 off, 1 clear, 2 motion, 3 on-line): drives the IO2 LED
void led_state(bool pairing, bool calibrating);   // overrides: blue blink / cyan breathe
