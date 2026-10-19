// Firmware-side calibration: thread-safe wrapper around calib.h plus flash persistence.
// REST handlers only enqueue requests; the Core 1 loop applies them, so ScoreState is never touched from the async web task.
#pragma once
#include <Arduino.h>
#include "hub.h"
#include "score.h"
#include "calib.h"

#define THERMAL_GATE_MIN 0.5f
#define THERMAL_GATE_MAX 3.0f
float calibration_thermal_gate();                         // required 3 s cooling in degC (default 1.0)
bool calibration_set_thermal_gate(float g);               // range-checked, saved to flash
void calibration_init(ScoreState *s);                     // loads and applies the stored calibration
void calibration_feed(const GridFrame *f);                // once per frame, before score_compute
bool calibration_request_start(int node, uint8_t sens);   // false if already capturing / bad node
bool calibration_request_accept();                        // false unless a result is pending
bool calibration_request_discard();
void calibration_request_reset(int node);                 // -1 = all
void calibration_debug_shift_baseline(float db);       // test aid (serial `presdbg <dB>`): make the learned baseline RSSI stale on purpose
bool calibration_pending_quiet(const char **why);   // pending capture usable and nobody moved (unattended accept); why = reason when false
int  calibration_phase();                                 // 0 idle, 1 capturing, 2 pending
float calibration_left_s();
String calibration_json();                                // full state for GET /api/calibration
const CalState *calibration_state();                    // read-only view for the Core 1 loop (drift detector); do not call from other tasks
