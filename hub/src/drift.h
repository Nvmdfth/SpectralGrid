// Calibration drift detector (spec §9.2 item 5, "auto-prompt"): notices when a calibrated node no longer matches its
// calibration (antenna wire changed, noise floor moved) or when a node has run uncalibrated for a while.
// Arduino-free, host-tested. Output is a per-node kind plus a one-shot sink callback when a condition first holds.
#pragma once
#include <stdint.h>
#include "hub.h"
#include "score.h"
#include "calib.h"

typedef enum { DRIFT_NONE = 0, DRIFT_UNCAL = 1, DRIFT_ANTENNA = 2, DRIFT_NOISE = 3, DRIFT_MOVED = 4 } DriftKind;
typedef void (*DriftSink)(int id, DriftKind kind, const char *msg, void *ctx);

#define DRIFT_UNCAL_AFTER_MS   300000u   // online this long with no calibration -> suggest one
#define DRIFT_ANTENNA_HOLD_MS   10000u   // raw touch level off by > max(5 %, 600 counts) this long
#define DRIFT_NOISE_HOLD_MS     60000u   // live noise baseline above calibration + max(5 sigma, 5 counts) this long
#define DRIFT_CLEAR_MS          30000u   // condition must be gone this long to clear and re-arm

typedef struct {
    bool first_set, raw_init;
    uint32_t first_ms, ant_since, noise_since, clear_since;
    bool ant_on, noise_on, moved;
    float raw_ema;
    uint8_t kind;
} DriftNode;
typedef struct { DriftNode n[SCORE_N]; DriftSink sink; void *ctx; } DriftState;

void drift_init(DriftState *d, DriftSink sink, void *ctx);
void drift_update(DriftState *d, const GridFrame *f, const CalState *c, const ScoreState *s, uint32_t now_ms);
DriftKind drift_kind(const DriftState *d, int id);
void drift_set_moved(DriftState *d, int id, bool moved);   // every RF path of the node sits off its calibrated level (moved.h); call before drift_update
void drift_reset_node(DriftState *d, int id);            // after a calibration is accepted for that node
const char *drift_kind_name(DriftKind k);
