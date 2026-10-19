// SpectralGrid wire format. Single source of truth for hub/ and probe/.
// Little-endian, packed. See SpectralGrid.md §5.
#pragma once
#include <stdint.h>
#include <string.h>

#define PROTO_MAGIC     0x5A
#define PROTO_VERSION   3       // still 3: PKT_CMD (hub->probe runtime commands, HMAC) and PKT_DIAG (probe->hub diagnostics + peer-link numbers) are additive, old code ignores unknown types; v3: HubSync carries the hub's score + detection state (probe mirrors the hub lights); v2: ProbeTelemetry gained vbat_mv, PKT_OTA added
#define MESH_CHANNEL    6
#define MESH_MAX_PROBES 16
#define FRAME_US        20000   // 50 Hz frame
#define SLOT_US         1500    // per-probe TX slot offset within a frame
#define HUBSYNC_MS      250     // 4 Hz: the probe lights follow the hub within a quarter second
#define NODE_ID_HUB     0
#define NODE_ID_UNASSIGNED 0xFF
#define HUBLOST_MS      5000    // probe: no HubSync for this long => hub lost
#define PROBE_DEFAULT_TX_HZ 50

enum PktType : uint8_t { PKT_TELEMETRY = 1, PKT_PAIR_REQ = 2, PKT_PAIR_ACK = 3, PKT_HUB_SYNC = 4, PKT_OTA = 5, PKT_CMD = 6, PKT_DIAG = 7 };
enum ProbeFlags : uint8_t { FLAG_LOW_BATT = 1, FLAG_SENSOR_FAULT = 2, FLAG_LOCAL_ALARM = 4 };

typedef struct __attribute__((packed)) {
    uint8_t magic;
    uint8_t proto_ver;
    uint8_t type;
    uint8_t node_id;
} PktHdr;

typedef struct __attribute__((packed)) {
    PktHdr   h;
    uint32_t seq;
    uint16_t emf_raw;
    uint16_t emf_p2p;
    int16_t  temp_centi_c;
    int16_t  temp_slope_centi;
    uint8_t  flags;
    uint16_t vbat_mv;       // battery voltage at the cell in mV; 0 = not measured
} ProbeTelemetry;

typedef struct __attribute__((packed)) {
    PktHdr   h;
    uint8_t  slot;
    uint8_t  tx_hz;
    uint32_t hub_time_ms;
} PairAck;

// state byte: bits 0-1 worst presence state (0 off, 1 clear, 2 motion, 3 on-line), bit 2 calibrating, bits 3-4 BOOT-gesture
// feedback (0 none, 1 countdown, 2 rejected, 3 accepted), bit 5 hub pairing window open, bit 6 EMF-wire touch event (blue/white flashes).
typedef struct __attribute__((packed)) {
    PktHdr   h;
    uint32_t hub_time_ms;
    uint8_t  score;         // the hub's headline score 0..100 (what its RGB LED shows)
    uint8_t  state;
} HubSync;

static_assert(sizeof(PktHdr) == 4, "PktHdr size");
static_assert(sizeof(ProbeTelemetry) == 19, "ProbeTelemetry size");
static_assert(sizeof(PairAck) == 10, "PairAck size");
static_assert(sizeof(HubSync) == 10, "HubSync size");

// Hub -> probe: "pull firmware from the hub over Wi-Fi and flash it". Authenticated with HMAC-SHA256 over h||size||md5
// using a shared secret (shared/ota_secret.h, never committed); probe OTA code exists only in builds where that file is present.
// The md5 is inside the authenticated part, so a rogue access point cannot substitute a different image.
typedef struct __attribute__((packed)) {
    PktHdr   h;
    uint32_t size;
    uint8_t  md5[16];
    uint8_t  tag[16];
} OtaCmd;
static_assert(sizeof(OtaCmd) == 40, "OtaCmd size");

// ---- v4: runtime commands and diagnostics --------------------------------------------------------------------------------
// Hub -> probe. Authenticated like PKT_OTA (HMAC-SHA256 of everything before `tag`, shared/ota_secret.h; no secret = commands off).
// Replay: the probe accepts a command when its nonce (random per hub boot) is new or its seq is higher than the last one it took.
enum CmdOp : uint8_t { CMD_INFO = 1, CMD_BCAST = 2, CMD_P2P = 3, CMD_REBOOT = 4, CMD_IDENT = 5, CMD_LINKS = 6 };   // BCAST/P2P: arg 0/1; INFO: reply now; REBOOT: restart; IDENT: arg = solid colour 1-7 for 2 min (0 = off); LINKS: arg bit0 = BCAST, bit1 = P2P in one command
typedef struct __attribute__((packed)) {
    PktHdr   h;
    uint32_t nonce;
    uint16_t seq;
    uint8_t  op;
    int32_t  arg;
    uint8_t  tag[16];
} ProbeCmd;
static_assert(sizeof(ProbeCmd) == 31, "ProbeCmd size");

// Probe -> hub (unicast, in the probe's own slot). Sent after every command and about 1 Hz while the peer links (CMD_P2P) are on.
// flags: bit0 telemetry is broadcast, bit1 peer CSI links on, bit2 an OTA is still unconfirmed (fallback armed).
#define DIAG_MAX_PEERS 5
typedef struct __attribute__((packed)) {
    uint8_t  node;          // the peer probe's node id
    uint8_t  fps;           // its frames per second heard here with CSI
    int16_t  rssi_x10;      // mean RSSI over the last window, x10 (whole dB are too coarse for the 1.5 dB shadow test)
    uint16_t tstd_x10000;   // temporal std x 10000 (the hub's detector feature; x1000 was too coarse: one step was 3% of a quiet level)
    uint16_t win_seq;       // window counter (a window every 25 frames)
} PeerLinkRep;
typedef struct __attribute__((packed)) {
    PktHdr   h;
    uint16_t ack_seq;       // seq of the last command taken
    uint32_t build_id;      // hash of the build date/time: tells which image is running
    uint8_t  part;          // running OTA slot: 0 or 1 (255 = factory)
    uint8_t  flags;
    uint8_t  npeers;
    uint8_t  heap_kb;       // free internal heap / 4 KB
    uint32_t uptime_s;
    PeerLinkRep peer[DIAG_MAX_PEERS];
} PktDiag;
static_assert(sizeof(PeerLinkRep) == 8, "PeerLinkRep size");
static_assert(sizeof(PktDiag) == 4 + 2 + 4 + 4 + 4 + 40, "PktDiag size");

static inline int proto_cmd_op(const char *s) {   // console / HTTP names -> CmdOp, or -1
    if (!s) return -1;
    if (!strcmp(s, "info")) return CMD_INFO;
    if (!strcmp(s, "bcast")) return CMD_BCAST;
    if (!strcmp(s, "p2p")) return CMD_P2P;
    if (!strcmp(s, "reboot")) return CMD_REBOOT;
    if (!strcmp(s, "ident")) return CMD_IDENT;
    if (!strcmp(s, "links")) return CMD_LINKS;
    return -1;
}

static inline PktHdr make_hdr(PktType t, uint8_t node_id) {
    PktHdr h = { PROTO_MAGIC, PROTO_VERSION, (uint8_t)t, node_id };
    return h;
}
static inline bool hdr_valid(const PktHdr *h) { return h->magic == PROTO_MAGIC; }
static inline bool hdr_compatible(const PktHdr *h) { return hdr_valid(h) && h->proto_ver == PROTO_VERSION; }

// Probes that fit in one frame without slot collisions at a given rate (13 at 50 Hz, 26 at 25 Hz, ...).
// MESH_MAX_PROBES (16) therefore requires tx_hz <= 25 once more than 13 probes are paired.
static inline uint32_t proto_frame_us(uint8_t tx_hz) { return 1000000u / tx_hz; }
static inline uint32_t proto_slots_per_frame(uint8_t tx_hz) { return proto_frame_us(tx_hz) / SLOT_US; }
static inline uint8_t proto_clamp_tx_hz(uint8_t hz) { return hz >= 50 ? 50 : (hz >= 25 ? 25 : 10); }

// Microseconds from `hub_us` (hub clock) until this slot's next transmit instant. Result in [1, frame_us].
static inline uint32_t slot_wait_us(uint64_t hub_us, uint8_t tx_hz, uint8_t slot) {
    uint32_t frame = proto_frame_us(tx_hz);
    uint32_t target = ((uint32_t)slot * SLOT_US) % frame;
    uint32_t phase = (uint32_t)(hub_us % frame);
    uint32_t wait = (target + frame - phase) % frame;
    return wait == 0 ? frame : wait;
}
