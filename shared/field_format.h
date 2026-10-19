// Field log record framing (hub/src/fieldlog.cpp writes it) (Arduino-free, host-tested; decoded by hub/tools/field_decode.py).
// One record: 0xA5 | type | len | payload[len] | crc8(type, len, payload). All integers little-endian. A reader that hits
// garbage (power loss mid-write, flash fault) scans forward to the next 0xA5 whose CRC checks out, so a damaged tail loses
// only the damaged records. Time is the hub's millis() (uptime); a CLOCK record maps it to wall time once a phone connects.
//
//   BOOT  1: u32 t_ms, u32 boot_index, char tag[]                      (tag = firmware build string)
//   FEAT  2: u32 t_ms, u8 node, u16 tstd_x1e5, i16 rssi_x10, u8 state, u8 score, u8 moving   (one per 1 s window, 0.5 s step)
//   NODE  3: u32 t_ms, u8 node, u8 online, u16 emf_p2p, i16 temp_x100, u16 vbat_mv, i8 rssi, u8 loss_x10 [, u16 emf_raw]   (2 Hz per node; emf_raw added 2026-10-08, 16-byte payload; older logs have 14)
//   AMP   4: u32 t_ms, u8 node, i8 rssi, u8 amp[52]                    (amplitude x 4, ~25 frames/s per link)
//   LABEL 5: u32 t_ms, char text[]                                     (phone label)
//   EVENT 6: u32 t_ms, u8 type, u8 id, char text[]
//   CAL   7: u32 t_ms, u8 node, f32 threshold, f32 rssi_base           (presence calibration in effect)
//   CLOCK 8: u32 t_ms, i64 epoch_ms
//   P2P   9: u32 t_ms, u8 rx, u8 tx, u16 tstd_x1e5, i16 rssi_x10, u8 state, u8 score, u16 thr_x1e5, i16 rssi_base_x10
//            (one per probe-to-probe window as reported, ~2 Hz per directed link; added 2026-10-19)
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define FL_SYNC        0xA5
#define FL_MAX_PAYLOAD 80
typedef enum { FR_BOOT = 1, FR_FEAT = 2, FR_NODE = 3, FR_AMP = 4, FR_LABEL = 5, FR_EVENT = 6, FR_CAL = 7, FR_CLOCK = 8, FR_P2P = 9 } FieldRec;

static inline uint8_t fl_crc8(const uint8_t *d, size_t n, uint8_t crc = 0) {
    for (size_t i = 0; i < n; i++) { crc ^= d[i]; for (int b = 0; b < 8; b++) crc = (uint8_t)((crc & 0x80) ? (crc << 1) ^ 0x07 : crc << 1); }
    return crc;
}

// Frame a record into out (needs len + 4 bytes). Returns the frame size, 0 if the payload is too long.
static inline size_t fl_frame(uint8_t *out, uint8_t type, const uint8_t *payload, size_t len) {
    if (len > FL_MAX_PAYLOAD) return 0;
    out[0] = FL_SYNC; out[1] = type; out[2] = (uint8_t)len;
    memcpy(out + 3, payload, len);
    out[3 + len] = fl_crc8(out + 1, 2 + len);
    return len + 4;
}

typedef struct { uint8_t type, len; uint8_t payload[FL_MAX_PAYLOAD]; } FlRec;

// Parse the next valid record at or after buf. Returns the bytes consumed (>= 1 while n > 0); *ok says whether a record was
// decoded. A truncated record at the end of the buffer returns consumed = 0 with *ok = false (wait for more data).
static inline size_t fl_parse(const uint8_t *buf, size_t n, FlRec *r, bool *ok) {
    *ok = false;
    size_t i = 0;
    while (i < n) {
        if (buf[i] != FL_SYNC) { i++; continue; }
        if (i + 3 > n) return i;
        size_t len = buf[i + 2];
        if (len > FL_MAX_PAYLOAD) { i++; continue; }
        if (i + 4 + len > n) return i;                       // maybe just not fully read yet
        if (fl_crc8(buf + i + 1, 2 + len) != buf[i + 3 + len]) { i++; continue; }
        r->type = buf[i + 1]; r->len = (uint8_t)len; memcpy(r->payload, buf + i + 3, len);
        *ok = true;
        return i + 4 + len;
    }
    return i;
}

static inline void fl_u16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void fl_u32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static inline void fl_i64(uint8_t *p, int64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)((uint64_t)v >> (8 * i)); }
static inline void fl_f32(uint8_t *p, float v) { uint32_t u; memcpy(&u, &v, 4); fl_u32(p, u); }
