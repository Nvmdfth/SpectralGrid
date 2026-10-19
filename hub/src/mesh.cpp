// Hub mesh manager (SpectralGrid.md §5, §7.2): pairing, slot assignment, 1 Hz HubSync, probe telemetry RX, liveness/loss.
// ESP-NOW runs on the STA interface (probes learn the hub's STA MAC from the PairAck source address).
#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include "config.h"
#include "hub.h"
#include "ota_auth.h"
#include "peers.h"

typedef struct {
    uint32_t last_seen_ms, last_ack_ms;
    uint32_t last_seq; bool seq_valid;
    uint32_t rx, lost, rx_mark, lost_mark;
    int8_t rssi;
    float loss_pct;
    ProbeTelemetry t;
    PktDiag diag; uint32_t diag_ms;       // last PKT_DIAG from this probe (diag_ms 0 = none yet)
    bool diag_old;                        // it sent a PKT_DIAG of another (older) layout: its firmware predates this hub's
} PeerRt;

static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
static PeerTable table;
static PeerRt rt[MESH_MAX_PROBES + 1];          // indexed by node_id (1..16)
static QueueHandle_t reqq;                       // 6-byte MACs that need a PairAck
static uint32_t pair_until_ms = 0, last_sync_ms = 0, last_loss_ms = 0, boot_down_ms = 0;
static const uint8_t BCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static const char *NS = "hubmesh";

// ---- persistence -----------------------------------------------------------------------------
static void table_save() {
    PeerTable copy;
    portENTER_CRITICAL(&mux); copy = table; portEXIT_CRITICAL(&mux);
    Preferences p;
    if (p.begin(NS, false)) { p.putBytes("tbl", &copy, sizeof(copy)); p.end(); }
}

static void table_load() {
    peers_init(&table, MESH_DEFAULT_TX_HZ);
    Preferences p;
    if (!p.begin(NS, false)) return;      // read-write: creates the namespace on first boot
    PeerTable t;
    if (p.getBytesLength("tbl") == sizeof(t)) {
        p.getBytes("tbl", &t, sizeof(t));
        t.tx_hz = proto_clamp_tx_hz(t.tx_hz);
        bool ok = true;
        for (int i = 0; i < MESH_MAX_PROBES; i++)
            if (t.rec[i].used && (t.rec[i].slot != i || t.rec[i].node_id != i + 1 || (uint32_t)i >= proto_slots_per_frame(t.tx_hz))) ok = false;
        if (ok) table = t; else Serial.println("mesh: stored table invalid, starting empty");
    }
    p.end();
}

// ---- radio -----------------------------------------------------------------------------------
static void add_peer(const uint8_t *mac) {
    if (esp_now_is_peer_exist(mac)) return;
    esp_now_peer_info_t p = {};
    memcpy(p.peer_addr, mac, 6);
    p.channel = 0;
    p.ifidx = WIFI_IF_STA;
    p.encrypt = false;
    esp_now_add_peer(&p);
}

static void send_ack(const PeerRec &r) {
    PairAck a;
    a.h = make_hdr(PKT_PAIR_ACK, r.node_id);
    a.slot = r.slot;
    a.tx_hz = table.tx_hz;
    a.hub_time_ms = millis();
    add_peer(r.mac);
    esp_now_send(r.mac, (const uint8_t *)&a, sizeof(a));
}

static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    if (len < (int)sizeof(PktHdr)) return;
    const PktHdr *h = (const PktHdr *)data;
    if (!hdr_compatible(h)) return;
    if (h->type == PKT_PAIR_REQ) { xQueueSend(reqq, info->src_addr, 0); return; }
    if (h->type == PKT_DIAG) {
        portENTER_CRITICAL(&mux);
        PeerRt *dr = nullptr; PeerRec *r = peers_find_mac(&table, info->src_addr);
        if (r && r->node_id == h->node_id) dr = &rt[r->node_id];
        if (dr && len == (int)sizeof(PktDiag)) { memcpy(&dr->diag, data, sizeof(PktDiag)); dr->diag_ms = millis() | 1; dr->diag_old = false; }
        else if (dr) dr->diag_old = true;
        portEXIT_CRITICAL(&mux);
        return;
    }
    if (h->type != PKT_TELEMETRY || len < (int)sizeof(ProbeTelemetry)) return;

    ProbeTelemetry t;
    memcpy(&t, data, sizeof(t));
    int8_t rssi = info->rx_ctrl ? info->rx_ctrl->rssi : 0;
    bool known = false, matches = false;
    portENTER_CRITICAL(&mux);
    PeerRec *r = peers_find_mac(&table, info->src_addr);
    if (r) {
        known = true;
        matches = (r->node_id == h->node_id);
        if (matches) {
            PeerRt &x = rt[r->node_id];
            if (x.seq_valid && t.seq > x.last_seq) { uint32_t gap = t.seq - x.last_seq - 1; if (gap < 1000) x.lost += gap; }
            x.seq_valid = true; x.last_seq = t.seq;
            x.rx++; x.rssi = rssi; x.last_seen_ms = millis(); x.t = t;
        }
    }
    portEXIT_CRITICAL(&mux);
    // Probe still beaconing under an id we no longer have (hub table cleared/renumbered): re-ack if allowed.
    if (!matches && (known || mesh_pairing_open())) xQueueSend(reqq, info->src_addr, 0);
}

static void handle_req(const uint8_t *mac) {
    PeerRec rec;
    bool known;
    portENTER_CRITICAL(&mux);
    PeerRec *r = peers_find_mac(&table, mac);
    known = r != nullptr;
    if (known) rec = *r;
    portEXIT_CRITICAL(&mux);

    uint32_t now = millis();
    if (!known) {
        if (!mesh_pairing_open()) return;
        bool full = false;
        portENTER_CRITICAL(&mux);
        r = peers_assign(&table, mac);
        if (r) { rec = *r; memset(&rt[rec.node_id], 0, sizeof(PeerRt)); } else full = true;
        portEXIT_CRITICAL(&mux);
        if (full) { Serial.println("mesh: pairing refused, mesh full"); return; }
        table_save();
        Serial.printf("mesh: paired %02x:%02x:%02x:%02x:%02x:%02x -> node %u slot %u\n",
                      mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], rec.node_id, rec.slot);
    } else if (now - rt[rec.node_id].last_ack_ms < PEER_ACK_MIN_MS) return;
    rt[rec.node_id].last_ack_ms = now;
    send_ack(rec);
}

// ---- public API ------------------------------------------------------------------------------
void mesh_init() {
    WiFi.mode(WIFI_AP_STA);
    WiFi.disconnect();
    esp_wifi_set_channel(MESH_CHANNEL, WIFI_SECOND_CHAN_NONE);
    WiFi.setTxPower(WIFI_POWER_13dBm);
    reqq = xQueueCreate(8, 6);
    if (esp_now_init() != ESP_OK) { Serial.println("mesh: esp_now_init FAILED"); return; }
    esp_now_register_recv_cb(on_recv);
    add_peer(BCAST);
    memset(rt, 0, sizeof(rt));
    table_load();
    for (int i = 0; i < MESH_MAX_PROBES; i++) if (table.rec[i].used) add_peer(table.rec[i].mac);
    pinMode(PIN_BOOT, INPUT_PULLUP);
    Serial.printf("mesh: %d probe(s) in table, tx_hz=%u, capacity=%d, pairing closed (hold BOOT %d s to open)\n",
                  peers_count(&table), table.tx_hz, peers_capacity(&table), PAIR_HOLD_MS / 1000);
}

static volatile uint8_t light_score = 0, light_state = 0;
void mesh_set_light(uint8_t score, uint8_t state) { light_score = score; light_state = state; }

void mesh_open_pairing(uint32_t ms) { pair_until_ms = millis() + ms; if (!pair_until_ms) pair_until_ms = 1; }
bool mesh_pairing_open() { return pair_until_ms != 0 && (int32_t)(pair_until_ms - millis()) > 0; }

void mesh_loop() {
    uint32_t now = millis();

    if (digitalRead(PIN_BOOT) == LOW) {
        if (!boot_down_ms) boot_down_ms = now;
        else if (now - boot_down_ms >= PAIR_HOLD_MS) {
            mesh_open_pairing(PAIR_WINDOW_MS);
            Serial.printf("mesh: pairing window open for %d s\n", PAIR_WINDOW_MS / 1000);
            boot_down_ms = 0xFFFFFFFFu;     // latch until release
        }
    } else boot_down_ms = 0;

    uint8_t mac[6];
    for (int i = 0; i < 4 && xQueueReceive(reqq, mac, 0) == pdTRUE; i++) handle_req(mac);

    if (now - last_sync_ms >= HUBSYNC_MS) {
        last_sync_ms = now;
        HubSync s;
        s.h = make_hdr(PKT_HUB_SYNC, NODE_ID_HUB);
        s.hub_time_ms = now; s.score = light_score; s.state = light_state;
        esp_now_send(BCAST, (const uint8_t *)&s, sizeof(s));
    }

    if (now - last_loss_ms >= 2000) {
        last_loss_ms = now;
        portENTER_CRITICAL(&mux);
        for (int id = 1; id <= MESH_MAX_PROBES; id++) {
            PeerRt &x = rt[id];
            uint32_t drx = x.rx - x.rx_mark, dlost = x.lost - x.lost_mark;
            x.loss_pct = (drx + dlost) ? 100.0f * dlost / (drx + dlost) : x.loss_pct;
            x.rx_mark = x.rx; x.lost_mark = x.lost;
        }
        portEXIT_CRITICAL(&mux);
    }
}

int mesh_snapshot(NodeSample *out, int max) {
    int n = 0;
    uint32_t now = millis();
    portENTER_CRITICAL(&mux);
    for (int i = 0; i < MESH_MAX_PROBES && n < max; i++) {
        const PeerRec &r = table.rec[i];
        if (!r.used) continue;
        const PeerRt &x = rt[r.node_id];
        NodeSample &o = out[n++];
        o.node_id = r.node_id;
        o.online = x.last_seen_ms != 0 && (now - x.last_seen_ms) < PEER_ONLINE_MS;
        o.emf_raw = x.t.emf_raw;
        o.emf_p2p = x.t.emf_p2p;
        o.temp_c = x.t.temp_centi_c / 100.0f;
        o.d_temp = x.t.temp_slope_centi / 100.0f;
        o.rssi = x.rssi;
        o.loss_pct = x.loss_pct;
        o.vbat_mv = x.t.vbat_mv;
    }
    portEXIT_CRITICAL(&mux);
    return n;
}

int mesh_node_for_mac(const uint8_t *mac) {
    int id = -1;
    portENTER_CRITICAL(&mux);
    PeerRec *r = peers_find_mac(&table, mac);
    if (r) id = r->node_id;
    portEXIT_CRITICAL(&mux);
    return id;
}

bool mesh_mac_for_node(uint8_t node_id, uint8_t *mac_out) {
    bool ok = false;
    portENTER_CRITICAL(&mux);
    PeerRec *r = peers_find_id(&table, node_id);
    if (r) { memcpy(mac_out, r->mac, 6); ok = true; }
    portEXIT_CRITICAL(&mux);
    return ok;
}

bool mesh_send_raw(const uint8_t *mac, const uint8_t *data, size_t len) {
    add_peer(mac);
    return esp_now_send(mac, data, len) == ESP_OK;
}

void mesh_clear_diag_old(uint8_t node_id) {
    if (node_id < 1 || node_id > MESH_MAX_PROBES) return;
    portENTER_CRITICAL(&mux); rt[node_id].diag_old = false; portEXIT_CRITICAL(&mux);
}

bool mesh_diag_old(uint8_t node_id) {
    if (node_id < 1 || node_id > MESH_MAX_PROBES) return false;
    portENTER_CRITICAL(&mux); bool o = rt[node_id].diag_old; portEXIT_CRITICAL(&mux);
    return o;
}

bool mesh_get_diag(uint8_t node_id, PktDiag *out, uint32_t *age_ms) {
    if (node_id < 1 || node_id > MESH_MAX_PROBES) return false;
    portENTER_CRITICAL(&mux);
    bool ok = rt[node_id].diag_ms != 0;
    if (ok) { *out = rt[node_id].diag; *age_ms = millis() - rt[node_id].diag_ms; }
    portEXIT_CRITICAL(&mux);
    return ok;
}

// node 0 = every paired probe. Sent twice (the probe drops the duplicate by seq). False when probe commands are not provisioned.
bool mesh_send_cmd(int node, uint8_t op, int32_t arg) {
#if OTA_HAVE_SECRET
    static uint32_t nonce = esp_random(); static uint16_t seq = 0;
    uint8_t macs[MESH_MAX_PROBES][6]; int n = 0;
    for (int id = 1; id <= MESH_MAX_PROBES; id++) if ((node == 0 || node == id) && mesh_mac_for_node((uint8_t)id, macs[n])) n++;
    if (!n) return false;
    ProbeCmd c; memset(&c, 0, sizeof(c));
    c.h = make_hdr(PKT_CMD, NODE_ID_HUB); c.nonce = nonce; c.seq = ++seq; c.op = op; c.arg = arg;
    cmd_compute_tag(&c, c.tag);
    for (int i = 0; i < n; i++) { mesh_send_raw(macs[i], (const uint8_t *)&c, sizeof(c)); mesh_send_raw(macs[i], (const uint8_t *)&c, sizeof(c)); }
    return true;
#else
    (void)node; (void)op; (void)arg; return false;
#endif
}

int mesh_paired_count() {
    portENTER_CRITICAL(&mux);
    int n = peers_count(&table);
    portEXIT_CRITICAL(&mux);
    return n;
}

bool mesh_unpair(uint8_t node_id) {
    uint8_t mac[6] = {0};
    bool ok;
    portENTER_CRITICAL(&mux);
    PeerRec *r = peers_find_id(&table, node_id);
    if (r) memcpy(mac, r->mac, 6);
    ok = peers_remove(&table, node_id);
    if (ok) memset(&rt[node_id], 0, sizeof(PeerRt));
    portEXIT_CRITICAL(&mux);
    if (ok) { esp_now_del_peer(mac); table_save(); }
    return ok;
}

bool mesh_set_tx_hz(uint8_t hz) {
    PeerRec copy[MESH_MAX_PROBES];
    bool ok;
    portENTER_CRITICAL(&mux);
    ok = peers_set_tx_hz(&table, hz);
    memcpy(copy, table.rec, sizeof(copy));
    portEXIT_CRITICAL(&mux);
    if (!ok) return false;
    table_save();
    for (int i = 0; i < MESH_MAX_PROBES; i++) if (copy[i].used) send_ack(copy[i]);   // probes accept a reconfigure from their hub
    return true;
}
