// Composite anomaly engine (SpectralGrid.md §8). Arduino-free: host-testable.
#pragma once
#include "hub.h"
#include "dsp.h"
#include "presence.h"
#include "pathfuse.h"
#include "moved.h"

#define SCORE_N (MESH_MAX_PROBES + 1)   // indexed by node_id; 0 = hub

typedef struct {
    float w_csi, w_emf, w_t;            // probes-present weights (§8.2)
    float wd_csi, wd_emf, wd_t;         // degraded-mode weights (§8.3)
    float alpha;                        // EMA alpha (§8.1)
    float k_sigma;                      // gate: only deviation beyond k*sigma counts
    float emf_scale, csi_scale;         // deviation that maps to 100. UNCALIBRATED placeholders
    float temp_gate_c, temp_gate_tol;   // thermal: a 3 s drop must exceed gate+tol (degC) to count; die sensor steps ~1 degC
    float temp_scale;                   // degC above the gate that maps to 100
    uint32_t warmup_n;                  // samples before a channel is scored
    float lo, hi;                       // vector thresholds on per-channel 0..100 scores
    uint32_t hysteresis_ms;             // mode switch debounce
} ScoreConfig;

typedef enum { SCORE_MODE_DEGRADED = 0, SCORE_MODE_GRID = 1 } ScoreMode;

typedef struct {
    ScoreConfig cfg;
    Baseline emf[SCORE_N], csi[SCORE_N];            // thermal has no baseline: it is an absolute 3 s drop
    float emf_hat[SCORE_N], temp_hat[SCORE_N], csi_hat[SCORE_N];   // last per-channel scores
    float emf_scale_n[SCORE_N], csi_scale_n[SCORE_N];              // per-node/link scale; defaults from cfg, set by calibration
    ScoreMode mode, pending_mode;
    bool pending_set;
    uint32_t pending_since_ms;
    PresLink pres[SCORE_N];              // CSI presence detector per link; inert (PRES_OFF) until calibrated, then replaces csi_hat
    PathAct ext[FUSE_MAX_PATHS];         // probe<->probe paths (p2p.cpp, p2p_paths), set by the caller before score_compute
    MovedPath mv[SCORE_N];               // hub links: distance from the calibrated RSSI (moved.h)
    uint32_t moved_mask;                 // nodes whose every path is off its calibrated level (a board moved or bumped); bit n = node n
    uint8_t n_ext, n_ext_cal;            // paths in ext[]; calibrated probe pairs
    FuseVerdict fused;                   // the one alarm rule over hub and probe paths (pathfuse.h): score floor, vectors, lights
    int control_id;                      // reference probe excluded from scoring (-1 none); see score_set_control
} ScoreState;

void score_config_default(ScoreConfig *c);
void score_init(ScoreState *s);
// d_temp in GridFrame is the 3 s temperature change in degC (negative = drop).
// Link entries with node_id 0 are the hub's own AP-side pseudo-link (degraded mode).
void score_compute(ScoreState *s, GridFrame *f, uint32_t now_ms);
// Calibration hooks: seed a channel's baseline (skips the warm-up) and set its scale; reset restores defaults.
void score_seed_emf(ScoreState *s, int id, float mean, float sd, float scale);
void score_seed_csi(ScoreState *s, int id, float mean, float sd, float scale);
void score_reset_node(ScoreState *s, int id);
// Control probe (false-positive control, spec §9.2 item 7): a probe placed away from the area of interest. It is excluded
// from the score and vectors; if it shows a disturbance at the same time as another node the event is classified
// COMMON_MODE (likely environmental: mains, HVAC, a nearby transmitter) instead of a local/grid anomaly.
void score_set_control(ScoreState *s, int id);       // -1 clears; node 0 (the hub) cannot be the control
