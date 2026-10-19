// Calibration engine (SpectralGrid.md §9.2 item 5). Arduino-free, host-tested.
// Flow: start -> CAPTURING (30 s of quiet-period stats from the normal telemetry stream) -> PENDING (review) ->
// accept (apply + persist) or discard. Nothing changes until accept. Probes need no changes: their telemetry
// already carries emf_p2p, so the hub captures the statistics itself.
#pragma once
#include <stdint.h>
#include "hub.h"
#include "score.h"

#define CAL_CAPTURE_MS   30000     // 2026-10-08: 15 -> 30 s (a longer quiet capture gives a steadier threshold and catches rare disturbances)
#define CAL_MIN_SAMPLES  100        // 5 s at 20 Hz; fewer => channel not calibrated
#define CAL_EMF_FLOOR    10.0f      // scale never below this (counts): the floating pin quantises at ~1-2 counts
#define CAL_CSI_FLOOR    6.0f
#define CAL_SPIKE_SIGMA  12.0f      // max excursion beyond this many sigma (and beyond the floor/2...) => "not quiet" warning

typedef enum { CAL_IDLE = 0, CAL_CAPTURING = 1, CAL_PENDING = 2 } CalPhase;
typedef enum { CAL_OK = 0, CAL_WARN_SPIKE = 1 } CalWarn;
typedef enum { CAL_SENS_LOW = 0, CAL_SENS_NORMAL = 1, CAL_SENS_HIGH = 2 } CalSens;

typedef struct { uint32_t n; float mean, m2, max; } CalStat;                         // Welford
typedef struct { bool valid; uint32_t n; float mean, sd, max, scale, prev_mean, prev_sd, raw_mean; } CalChan;   // raw_mean: EMF channel only (touch level at calibration)
typedef struct { CalChan emf, csi; uint8_t sens, warn; } NodeCal;

typedef struct {
    CalPhase phase;
    int target;                          // -1 = all nodes, else node id
    uint8_t sens;
    uint32_t start_ms;
    CalStat emf[SCORE_N], csi[SCORE_N], raw[SCORE_N];
    NodeCal pending[SCORE_N];
    NodeCal applied[SCORE_N];            // active calibration (valid=false => defaults in use)
} CalState;

void cal_init(CalState *c);
bool cal_start(CalState *c, int target, uint8_t sens, uint32_t now_ms);   // false if already capturing or bad target
// Call once per aggregation frame, before score_compute. Moves CAPTURING -> PENDING when the time is up.
void cal_feed(CalState *c, const GridFrame *f, const ScoreState *s, uint32_t now_ms);
int  cal_accept(CalState *c, ScoreState *s);          // applies valid pending channels; returns nodes applied
void cal_discard(CalState *c);
void cal_reset(CalState *c, ScoreState *s, int target);   // target -1 = all
void cal_apply_one(ScoreState *s, int id, const NodeCal *cal);   // also used to restore from storage at boot
float cal_left_s(const CalState *c, uint32_t now_ms);
