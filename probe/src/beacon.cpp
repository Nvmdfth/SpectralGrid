// Core 1: pairing, slotted ESP-NOW telemetry TX, LED (§5.2, §5.4, §6.2).
// States: PAIRING (broadcast PairReq) -> BEACON (unicast telemetry in our slot) -> HUB_LOST (no HubSync for HUBLOST_MS).
#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_timer.h>
#include "config.h"
#include "tasks.h"
#include "ota_auth.h"
#include "csi_window.h"
#include <Preferences.h>
#include <esp_ota_ops.h>
#include <esp_heap_caps.h>
#if OTA_HAVE_SECRET
#include <HTTPClient.h>
#include <Update.h>
#endif

typedef enum { ST_PAIRING, ST_BEACON, ST_HUB_LOST } State;

static State state = ST_PAIRING;
static PairingState ps;
static const uint8_t BCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// Written by the ESP-NOW receive callback (WiFi task), consumed by beacon_task.
static volatile bool ack_pending = false;
static PairAck ack_pkt;
static uint8_t ack_mac[6];
static volatile int64_t sync_offset_us = 0;      // hub clock - local clock
static volatile int64_t last_sync_us = 0;
#if OTA_HAVE_SECRET
static OtaCmd ota_cmd;
static volatile bool ota_pending = false;
#endif

// ---- v4: runtime flags (set by authenticated hub commands), peer CSI links, diagnostics --------------------------------------
#define MAX_PEERS 5                                   // other probes this one tracks (CsiWin ~5.3 KB each, allocated when p2p is on)
typedef struct { uint8_t node; uint8_t mac[6]; volatile uint32_t cnt; CsiWin *w; volatile float tstd, rssi; volatile uint32_t wseq; uint32_t last_cnt; uint8_t fps; } Peer;
static Peer peers[MAX_PEERS];
static volatile uint8_t npeers = 0;
static volatile bool f_bcast = false, f_p2p = false;       // both off at every boot; the hub switches them on
static ProbeCmd cmd_pkt; static volatile bool cmd_pending = false;
static uint32_t cmd_nonce = 0; static uint16_t cmd_last_seq = 0; static bool cmd_have = false;
static bool diag_now = false;
static volatile bool fw_confirm_pending = false;           // an OTA image is running unconfirmed (see the boot fallback)
static volatile bool hub_heard = false;                    // any frame from our hub this boot (a boot without the hub does not count against an image)

static uint32_t build_id() { uint32_t h = 2166136261u; for (const char *p = __DATE__ __TIME__; *p; p++) { h ^= (uint8_t)*p; h *= 16777619u; } return h; }

static void p2p_csi_cb(void *, wifi_csi_info_t *info) {
    if (!f_p2p || !info || !info->buf || info->len < 128) return;
    uint8_t n = npeers;
    for (uint8_t i = 0; i < n; i++) if (memcmp(info->mac, peers[i].mac, 6) == 0) {
        peers[i].cnt = peers[i].cnt + 1;
        CsiWin *w = peers[i].w;
        float t, r;
        if (w && csiw_push_amp(w, info->buf, info->rx_ctrl.rssi, &t, &r, nullptr)) { peers[i].tstd = t; peers[i].rssi = r; peers[i].wseq = peers[i].wseq + 1; }
        return;
    }
}

static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    if (len < (int)sizeof(PktHdr)) return;
    const PktHdr *h = (const PktHdr *)data;
    if (!hdr_compatible(h)) return;
    int64_t now = esp_timer_get_time();
    if (ps.paired && memcmp(info->src_addr, ps.hub_mac, 6) == 0) hub_heard = true;
    if (h->type == PKT_TELEMETRY && h->node_id != NODE_ID_HUB && h->node_id != NODE_ID_UNASSIGNED && h->node_id != ps.node_id) {   // another probe's (broadcast) telemetry: learn its MAC
        uint8_t n = npeers, k = 0;
        while (k < n && peers[k].node != h->node_id) k++;
        if (k == n && n < MAX_PEERS) { peers[n].node = h->node_id; memcpy(peers[n].mac, info->src_addr, 6); npeers = n + 1; }
        else if (k < n && memcmp(peers[k].mac, info->src_addr, 6) != 0) memcpy(peers[k].mac, info->src_addr, 6);
    }
    if (h->type == PKT_PAIR_ACK && len >= (int)sizeof(PairAck) && !ack_pending) {
        memcpy(&ack_pkt, data, sizeof(PairAck));
        memcpy(ack_mac, info->src_addr, 6);
        ack_pending = true;
    }
#if OTA_HAVE_SECRET
    else if (h->type == PKT_OTA && len >= (int)sizeof(OtaCmd) && ps.paired && memcmp(info->src_addr, ps.hub_mac, 6) == 0 && !ota_pending) {
        const OtaCmd *c = (const OtaCmd *)data;
        if (c->size > 0 && c->size < 3 * 1024 * 1024 && ota_tag_ok(c)) { memcpy(&ota_cmd, c, sizeof(ota_cmd)); ota_pending = true; }   // bad tag: silently ignored
    }
#endif
#if OTA_HAVE_SECRET
    else if (h->type == PKT_CMD && len >= (int)sizeof(ProbeCmd) && ps.paired && memcmp(info->src_addr, ps.hub_mac, 6) == 0 && !cmd_pending) {
        const ProbeCmd *c = (const ProbeCmd *)data;
        if (cmd_tag_ok(c) && (!cmd_have || c->nonce != cmd_nonce || c->seq > cmd_last_seq)) {     // bad tag or a replay: ignored silently
            cmd_nonce = c->nonce; cmd_last_seq = c->seq; cmd_have = true;
            memcpy(&cmd_pkt, c, sizeof(cmd_pkt)); cmd_pending = true;
        }
    }
#endif
    else if (h->type == PKT_HUB_SYNC && len >= (int)sizeof(HubSync) && ps.paired &&
               memcmp(info->src_addr, ps.hub_mac, 6) == 0) {
        const HubSync *s = (const HubSync *)data;
        sync_offset_us = (int64_t)s->hub_time_ms * 1000 - now;
        last_sync_us = now;
        led_hub_state(s->score, s->state);
    }
}

static void add_peer(const uint8_t *mac) {
    if (esp_now_is_peer_exist(mac)) return;
    esp_now_peer_info_t p = {};
    memcpy(p.peer_addr, mac, 6);
    p.channel = 0;              // current channel (MESH_CHANNEL)
    p.ifidx = WIFI_IF_STA;
    p.encrypt = false;
    if (esp_now_add_peer(&p) != ESP_OK) return;
    // Open item #14: ESP-NOW defaults to a legacy rate and CSI needs OFDM/HT frames. Confirm the hub sees CSI per probe frame.
    esp_now_rate_config_t rc = {};
    rc.phymode = WIFI_PHY_MODE_HT20;
    rc.rate = WIFI_PHY_RATE_MCS0_LGI;
    rc.ersu = false;
    rc.dcm = false;
    esp_err_t r = esp_now_set_peer_rate_config(mac, &rc);
    Serial.printf("peer %02x:%02x:%02x:%02x:%02x:%02x rate MCS0 -> %d\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], (int)r);
}

static void radio_init() {
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    esp_wifi_set_channel(MESH_CHANNEL, WIFI_SECOND_CHAN_NONE);
    WiFi.setTxPower(WIFI_POWER_13dBm);
    if (esp_now_init() != ESP_OK) { Serial.println("esp_now_init FAILED"); return; }
    esp_now_register_recv_cb(on_recv);
    { wifi_csi_config_t cfg = {}; cfg.lltf_en = true; cfg.htltf_en = false; cfg.stbc_htltf2_en = false; cfg.ltf_merge_en = false; cfg.channel_filter_en = false; cfg.manu_scale = false; cfg.shift = 0;
      esp_wifi_set_csi_config(&cfg); esp_wifi_set_csi_rx_cb(p2p_csi_cb, nullptr); esp_wifi_set_csi(false); }     // armed, but off until CMD_P2P
    add_peer(BCAST);
}

static uint32_t req_ok = 0, req_err = 0;     // PairReq broadcasts handed to the radio (not proof of reception)
static void send_pair_req() {
    PktHdr h = make_hdr(PKT_PAIR_REQ, ps.paired ? ps.node_id : NODE_ID_UNASSIGNED);
    (esp_now_send(BCAST, (const uint8_t *)&h, sizeof(h)) == ESP_OK) ? req_ok++ : req_err++;
}

static void accept_ack(uint32_t now_ms) {
    // First pairing: any hub. Afterwards (BEACON or HUB_LOST): only our own hub, which may reconfigure slot/rate or re-adopt us.
    bool ok = (state == ST_PAIRING) || (ps.paired && memcmp(ack_mac, ps.hub_mac, 6) == 0);
    if (ok && ack_pkt.h.node_id != NODE_ID_UNASSIGNED && ack_pkt.h.node_id != NODE_ID_HUB) {
        int64_t now = esp_timer_get_time();
        memcpy(ps.hub_mac, ack_mac, 6);
        ps.node_id = ack_pkt.h.node_id;
        ps.slot = ack_pkt.slot;
        ps.tx_hz = proto_clamp_tx_hz(ack_pkt.tx_hz);
        ps.paired = true;
        pairing_save(&ps);
        led_set_identity(ps.node_id);
        add_peer(ps.hub_mac);
        sync_offset_us = (int64_t)ack_pkt.hub_time_ms * 1000 - now;
        last_sync_us = now;
        state = ST_BEACON;
        Serial.printf("paired/reconfigured: node_id=%u slot=%u tx_hz=%u hub=%02x:%02x:%02x:%02x:%02x:%02x\n", ps.node_id, ps.slot, ps.tx_hz,
                      ps.hub_mac[0], ps.hub_mac[1], ps.hub_mac[2], ps.hub_mac[3], ps.hub_mac[4], ps.hub_mac[5]);
    }
    ack_pending = false;
    (void)now_ms;
}

static void send_diag() {
    PktDiag d; memset(&d, 0, sizeof(d));
    d.h = make_hdr(PKT_DIAG, ps.node_id);
    d.ack_seq = cmd_last_seq; d.build_id = build_id();
    const esp_partition_t *run = esp_ota_get_running_partition();
    d.part = run && run->subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_0 ? (uint8_t)(run->subtype - ESP_PARTITION_SUBTYPE_APP_OTA_0) : 255;
    d.flags = (f_bcast ? 1 : 0) | (f_p2p ? 2 : 0) | (fw_confirm_pending ? 4 : 0);
    { size_t hf = heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 4096; d.heap_kb = (uint8_t)(hf > 255 ? 255 : hf); }
    d.uptime_s = millis() / 1000;
    uint8_t n = npeers > DIAG_MAX_PEERS ? DIAG_MAX_PEERS : npeers; d.npeers = n;
    for (uint8_t i = 0; i < n; i++) {
        float r = peers[i].rssi, t = peers[i].tstd * 10000.0f;
        d.peer[i].node = peers[i].node; d.peer[i].fps = peers[i].fps;
        d.peer[i].rssi_x10 = (int16_t)lroundf(r < -127 ? -1270.0f : (r > 0 ? 0.0f : r * 10.0f));
        d.peer[i].tstd_x10000 = (uint16_t)(t < 0 ? 0 : (t > 65535 ? 65535 : t + 0.5f)); d.peer[i].win_seq = (uint16_t)peers[i].wseq;
    }
    esp_now_send(ps.hub_mac, (const uint8_t *)&d, sizeof(d));
}

#if OTA_HAVE_SECRET
// Pull the firmware the hub stored and flash it. Any failure reboots into the current firmware: the new image is written
// to the inactive OTA slot and only activated after a full write plus MD5 check (the MD5 came inside the HMAC).
static void do_ota() {
    ota_pending = false;
    Serial.printf("ota: authenticated command, %u bytes; leaving ESP-NOW to join the hub AP\n", (unsigned)ota_cmd.size);
    led_mode(LED_OTA);
    esp_now_deinit();
    WiFi.mode(WIFI_STA);
    WiFi.begin("SPECTRAL_GRID", nullptr, MESH_CHANNEL);
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) { led_update(millis()); delay(40); }
    if (WiFi.status() == WL_CONNECTED) {
        HTTPClient http;
        http.setTimeout(15000);
        http.begin(String("http://") + WiFi.gatewayIP().toString() + "/fw/probe.bin");   // the hub is the AP gateway; no hard-coded address
        int code = http.GET();
        if (code == 200 && (uint32_t)http.getSize() == ota_cmd.size) {
            char md5[33];
            for (int i = 0; i < 16; i++) sprintf(md5 + 2 * i, "%02x", ota_cmd.md5[i]);
            Update.setMD5(md5);
            if (Update.begin(ota_cmd.size)) {
                size_t w = Update.writeStream(*http.getStreamPtr());
                if (w == ota_cmd.size && Update.end(true)) { Serial.println("ota: success, rebooting"); { Preferences q; if (q.begin("fw", false)) { q.putBool("pend", true); q.putUChar("n", 0); q.end(); } } delay(200); ESP.restart(); }
                Serial.printf("ota: write/verify failed (%u of %u bytes): %s\n", (unsigned)w, (unsigned)ota_cmd.size, Update.errorString());
            } else Serial.printf("ota: Update.begin failed: %s\n", Update.errorString());
        } else Serial.printf("ota: download failed, http %d size %d (expected %u)\n", code, http.getSize(), (unsigned)ota_cmd.size);
    } else Serial.println("ota: could not join the hub AP");
    Serial.println("ota: failed, rebooting into the current firmware");
    delay(300);
    ESP.restart();
}
#endif

void beacon_task(void *) {
    {   // Fallback after an OTA: bootloader rollback is not available, so count unconfirmed boots of a freshly flashed image.
        // Three boots without ever syncing with the hub (see the confirm step in the loop) => boot the other slot (the previous image).
        Preferences q;
        if (q.begin("fw", false)) {
            if (q.getBool("pend", false)) {
                uint8_t n = q.getUChar("n", 0) + 1; q.putUChar("n", n);
                const esp_partition_t *other = esp_ota_get_next_update_partition(nullptr);
                uint8_t magic = 0;
                if (n >= 3 && other && esp_partition_read(other, 0, &magic, 1) == ESP_OK && magic == 0xE9) {
                    q.putBool("pend", false); q.putUChar("n", 0); q.end();
                    Serial.println("fw: new image never synced in 3 boots, falling back to the previous slot");
                    esp_ota_set_boot_partition(other); delay(100); ESP.restart();
                }
                fw_confirm_pending = true;
            }
            q.end();
        }
    }
    pairing_load(&ps);
    led_set_identity(ps.node_id);
    radio_init();
    pinMode(PIN_BOOT, INPUT_PULLUP);

    if (ps.paired) { add_peer(ps.hub_mac); state = ST_BEACON; last_sync_us = esp_timer_get_time(); sync_offset_us = 0; }
    else state = ST_PAIRING;
    Serial.printf("boot state=%s node_id=%u slot=%u tx_hz=%u\n", ps.paired ? "BEACON" : "PAIRING", ps.node_id, ps.slot, ps.tx_hz);

    uint32_t seq = 0, tx_ok = 0, tx_err = 0, last_req_ms = 0, last_print_ms = 0, boot_down_ms = 0;
    int64_t last_tx_us = 0;
    bool synced_once = false;

    for (;;) {
        uint32_t now_ms = millis();
        int64_t now_us = esp_timer_get_time();

        // BOOT held PAIR_HOLD_MS => forget hub and re-pair
        if (digitalRead(PIN_BOOT) == LOW) {
            if (!boot_down_ms) boot_down_ms = now_ms;
            else if (now_ms - boot_down_ms >= PAIR_HOLD_MS) {
                pairing_clear(&ps);
                led_set_identity(NODE_ID_UNASSIGNED);
                state = ST_PAIRING; synced_once = false; boot_down_ms = 0;
                Serial.println("BOOT held: pairing cleared, re-pairing");
                vTaskDelay(pdMS_TO_TICKS(1000));   // wait for release
            }
        } else boot_down_ms = 0;

        if (ack_pending) accept_ack(now_ms);
#if OTA_HAVE_SECRET
        if (ota_pending && state == ST_BEACON) do_ota();
#endif

        if (state == ST_BEACON) {
            if (last_sync_us > 0 && (now_us - last_sync_us) / 1000 > HUBLOST_MS) {
                state = ST_HUB_LOST;
                Serial.println("hub lost");
            }
        }

        led_mode(state == ST_BEACON ? LED_BEACON : (state == ST_PAIRING ? LED_PAIRING : LED_HUB_LOST));
        ProbeSample s;
        bool have = mailbox_read(&s);
        led_local_score(have ? s.local_score : 0);
        led_update(now_ms);

        if (cmd_pending) {
            switch (cmd_pkt.op) {
                case CMD_BCAST: f_bcast = cmd_pkt.arg != 0; break;
                case CMD_P2P:   f_p2p = cmd_pkt.arg != 0; esp_wifi_set_csi(f_p2p); break;
                case CMD_LINKS: f_bcast = (cmd_pkt.arg & 1) != 0; f_p2p = (cmd_pkt.arg & 2) != 0; esp_wifi_set_csi(f_p2p); break;
                case CMD_IDENT: led_ident((uint8_t)cmd_pkt.arg, 120000); break;
                case CMD_REBOOT: Serial.println("cmd: reboot"); delay(100); ESP.restart(); break;
                default: break;                                                                  // CMD_INFO: just the reply below
            }
            Serial.printf("cmd: op=%u arg=%d -> bcast=%d p2p=%d\n", cmd_pkt.op, (int)cmd_pkt.arg, (int)f_bcast, (int)f_p2p);
            cmd_pending = false; diag_now = true;
        }
        if (f_p2p) for (uint8_t i = 0; i < npeers; i++) if (!peers[i].w) { peers[i].w = (CsiWin *)malloc(sizeof(CsiWin)); if (peers[i].w) csiw_reset(peers[i].w); }
        { static uint32_t last_fps_ms = 0;
          if (now_ms - last_fps_ms >= 1000) { last_fps_ms = now_ms;
            for (uint8_t i = 0; i < npeers; i++) { uint32_t c = peers[i].cnt, d = c - peers[i].last_cnt; peers[i].last_cnt = c; peers[i].fps = d > 255 ? 255 : d; } } }
        if (fw_confirm_pending && !hub_heard && now_ms > 300000) {   // 5 min and not one frame from the hub: the hub is off, not the image broken
            Preferences q; if (q.begin("fw", false)) { uint8_t n = q.getUChar("n", 0); if (n) q.putUChar("n", n - 1); q.end(); }   // this boot does not count
            fw_confirm_pending = false; Serial.println("fw: hub absent this boot, not counted against the new image");
        }
        if (fw_confirm_pending && state == ST_BEACON && now_ms > 20000 && (now_us - last_sync_us) < 2000000) {   // synced with the hub for a while: this image is good
            Preferences q; if (q.begin("fw", false)) { q.putBool("pend", false); q.putUChar("n", 0); q.end(); }
            fw_confirm_pending = false; Serial.println("fw: new image confirmed");
        }
        if (state == ST_PAIRING || state == ST_HUB_LOST) {
            uint32_t every = state == ST_PAIRING ? PAIR_REQ_MS : HUBLOST_REQ_MS;
            if (now_ms - last_req_ms >= every) { last_req_ms = now_ms; send_pair_req(); }
            vTaskDelay(pdMS_TO_TICKS(10));
        } else {  // ST_BEACON
            uint64_t hub_us = (uint64_t)(now_us + sync_offset_us);
            uint32_t wait = slot_wait_us(hub_us, ps.tx_hz, ps.slot);
            if (wait >= 3000) {
                vTaskDelay(pdMS_TO_TICKS(constrain((int)(wait / 1000) - 2, 1, 10)));
            } else if (have && now_us - last_tx_us > (int64_t)proto_frame_us(ps.tx_hz) / 2) {
                int64_t target = now_us + wait;
                while (esp_timer_get_time() < target) { }
                ProbeTelemetry p = s.t;
                p.h = make_hdr(PKT_TELEMETRY, ps.node_id);
                p.seq = seq++;
                last_tx_us = esp_timer_get_time();
                (esp_now_send(f_bcast ? BCAST : ps.hub_mac, (const uint8_t *)&p, sizeof(p)) == ESP_OK) ? tx_ok++ : tx_err++;
                if (diag_now || (f_p2p && p.seq % ps.tx_hz == 0)) { send_diag(); diag_now = false; }
                if (p.seq % ps.tx_hz == 0) led_tx_pulse();      // ~1 Hz heartbeat
                if (!synced_once) synced_once = true;
            } else vTaskDelay(1);
        }

        if (now_ms - last_print_ms >= 2000) {
            last_print_ms = now_ms;
            Serial.printf("[%s] id=%u slot=%u %uHz tx ok=%u err=%u req=%u/%u | emf raw=%u p2p=%u score=%u | T=%.2fC d3s=%.2f | flags=0x%02x | sync_age=%lldms\n",
                          state == ST_BEACON ? "BEACON" : (state == ST_PAIRING ? "PAIRING" : "HUBLOST"),
                          ps.node_id, ps.slot, ps.tx_hz, (unsigned)tx_ok, (unsigned)tx_err, (unsigned)req_ok, (unsigned)req_err,
                          s.t.emf_raw, s.t.emf_p2p, s.local_score, s.t.temp_centi_c / 100.0f, s.t.temp_slope_centi / 100.0f,
                          s.t.flags, (long long)((now_us - last_sync_us) / 1000));
        }
    }
}
