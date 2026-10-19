// Probe-to-probe link presence on the hub: keeps the probes' peer-CSI links switched on, feeds their reports into the
// shared/p2pfuse.h detector, calibrates with the normal calibration capture, and gives the score one fused verdict.
// All calls except p2p_json / p2p_set_enabled / p2p_enabled are for the Core 1 loop.
#pragma once
#include <Arduino.h>
#include "hub.h"
#include "p2pfuse.h"

void p2p_init();                                          // loads the on/off switch and the stored link calibrations
void p2p_loop(const GridFrame *f, uint32_t now_ms);       // once per frame, before score_compute
P2pVerdict p2p_current();                                 // probe-pairs-only verdict of the last p2p_loop (status / JSON)
int p2p_get_paths(PathAct *out, int max, int *n_cal, uint32_t now_ms);   // probe pairs as pathfuse.h paths (score.cpp fuses them with the hub links)
void p2p_set_enabled(bool on);                            // saved; off sends the probes "links off" once
bool p2p_enabled();
// calibration hooks (calibration.cpp, Core 1)
void p2p_cal_start(int target);                           // node id or -1 = all links
void p2p_cal_done(float g);                               // capture finished: pending results computed
int  p2p_cal_accept_all();                                // links calibrated (saved to flash)
void p2p_cal_discard_all();
void p2p_cal_reset_all();
void p2p_cal_reset_node(int node);                         // links with this probe at either end (-1 = all)
bool p2p_cal_pending_info(bool *warn);                    // anything pending / any capture saw movement
String p2p_json();                                        // links + states for GET /api/p2p
