// Event log and detector (SpectralGrid.md §9.2 item 6). Arduino-free, host-tested.
// The detector watches each aggregation frame and emits discrete events (vector changes, alerts, nodes joining or
// dropping, pairing, calibration, low battery); the firmware glue stores them in the EventLog and pushes them to clients.
#pragma once
#include <stdint.h>
#include "hub.h"

typedef enum { EV_SYSTEM = 0, EV_VECTOR = 1, EV_ALERT = 2, EV_NODE_UP = 3, EV_NODE_DOWN = 4, EV_PAIRING = 5, EV_CAL = 6,
               EV_TAG = 7, EV_DRIFT = 8, EV_BATT = 9, EV_CONTROL = 10, EV_TOUCH = 11 } EvType;

typedef struct { uint32_t seq, t_ms, epoch_s; uint8_t type, id, score, _r; char text[44]; } Event;

#define EV_LOG_N 512
typedef struct { Event ev[EV_LOG_N]; uint32_t head, count, next_seq; } EventLog;   // head = next write slot

void evlog_init(EventLog *l);
const Event *evlog_push(EventLog *l, uint32_t t_ms, uint32_t epoch_s, uint8_t type, uint8_t id, uint8_t score, const char *text);
int evlog_since(const EventLog *l, uint32_t since_seq, Event *out, int max);       // seq > since_seq, oldest first (the first `max`)
int evlog_tail(const EventLog *l, uint32_t since_seq, Event *out, int max);        // the newest `max` with seq > since_seq, oldest first
const char *ev_type_name(uint8_t type);

typedef void (*EventSink)(const Event *e, void *ctx);   // e->seq/t_ms/epoch_s are filled by the log, not the detector

#define EVDET_N (MESH_MAX_PROBES + 1)
typedef struct {
    EventSink sink; void *ctx;
    bool have_vec; char vec[24]; int8_t vec_id;             // last reported vector
    char cand[24]; int8_t cand_id; int cand_n;              // candidate waiting for stability
    bool alerting;
    bool seen[EVDET_N], online[EVDET_N], low_batt[EVDET_N];
    bool hub_startup;                                       // hub sensor was offline at first sight: its first recovery is silent
    bool have_pairing, pairing; int cal_phase; bool have_cal;
    uint32_t radio_ms; bool radio_valid;
} EventDetector;

void evdet_init(EventDetector *d, EventSink sink, void *ctx);
void evdet_radio_activity(EventDetector *d, uint32_t now_ms);   // a client joined/left: may explain hub EMF noise
void evdet_observe(EventDetector *d, const GridFrame *f, bool pairing, int cal_phase, uint32_t now_ms);
void evdet_emit(EventDetector *d, uint8_t type, uint8_t id, uint8_t score, const char *text);   // for glue-originated events

// Streaming CSV export of an event snapshot (oldest first). Same chunked-callback contract as the session CSV.
typedef struct {
    const Event *snap; uint32_t n, idx;
    bool epoch_known; int64_t epoch_off_ms;       // wall clock = t_ms + epoch_off_ms for events without an epoch of their own
    bool header_done, done; char row[160]; uint16_t row_len, row_off;
} EvCsv;
void evcsv_begin(EvCsv *c, const Event *snap, uint32_t n, bool epoch_known, int64_t epoch_off_ms);
size_t evcsv_fill(EvCsv *c, uint8_t *out, size_t max);
int evlog_snapshot(const EventLog *l, Event *out, int max);     // all events, oldest first
