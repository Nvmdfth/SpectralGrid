// Session recording: compact encoding of GridFrames into a RingLog, decoding, and a streaming CSV exporter.
// Arduino-free, host-tested. One record = one decimated frame (default 2 Hz). SpectralGrid.md §9.2 item 6.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "hub.h"
#include "ringlog.h"

#define SESS_MAX_NODES   (MESH_MAX_PROBES + 1)
#define SESS_MAX_LINKS   MESH_MAX_PROBES                 // GridFrame holds at most this many links
#define SESS_MAX_REC     (9 + SESS_MAX_NODES * 14 + SESS_MAX_LINKS * 3)

typedef struct { uint8_t id, online; uint16_t emf_raw, p2p; int16_t temp_c100, d_temp_c100; int8_t rssi; uint8_t loss_pct; uint16_t vbat_mv; } SessNode;
typedef struct { uint8_t id; uint16_t csi100; } SessLink;
typedef struct { uint32_t t_ms; uint8_t score, vec; int8_t vec_id; uint8_t n_nodes, n_links; SessNode nodes[SESS_MAX_NODES]; SessLink links[SESS_MAX_LINKS]; } SessFrame;

const char *sess_vec_name(uint8_t code);
uint8_t sess_vec_code(const char *name);                    // 255 = unknown
uint16_t sess_encode(const GridFrame *f, uint32_t t_ms, uint8_t *out, uint16_t max);   // 0 if it does not fit
bool sess_decode(const uint8_t *rec, uint16_t len, SessFrame *out);
uint32_t sess_time(const uint8_t *rec);                     // t_ms of an encoded record

// Streaming "long" CSV (one row per node per frame). Safe to use from a chunked HTTP callback: call csv_fill until it
// returns 0. If the ring drops records mid-export it re-seeks by timestamp, so rows are never repeated.
typedef struct {
    const RingLog *ring;
    RingIter it; bool iter_ok;
    uint32_t last_t; bool have_last;
    int64_t epoch_off_ms; bool epoch_known;               // wall clock = t_ms + epoch_off_ms
    bool header_done; SessFrame f; bool have_frame; int node_idx;
    char row[220]; uint16_t row_len, row_off;
    bool done;
} CsvStream;
void csv_begin(CsvStream *c, const RingLog *ring, bool epoch_known, int64_t epoch_off_ms);
size_t csv_fill(CsvStream *c, uint8_t *out, size_t max);

// Seek helper for the replay API: position `it` at the first record with t_ms >= from_ms. Returns false if none.
bool sess_seek(const RingLog *r, RingIter *it, uint32_t from_ms);
