// Field log: persistent record of detector windows, node channels, raw CSI amplitudes ("raw-lite"), labels and events in a
// LittleFS file, for evaluation after a battery-powered run (FIELD_TEST.md). Format: shared/fieldlog.h. Firmware-only.
#pragma once
#include <Arduino.h>
#include "hub.h"
#include "score.h"

void fieldlog_init();                       // after LittleFS is mounted (logger_init); resumes logging if it was on before the reboot
bool fieldlog_on();
bool fieldlog_set(bool on);                 // persists the choice; false if the file cannot be opened
void fieldlog_frame(const GridFrame *f, const ScoreState *s, uint32_t now_ms);   // Core 1 loop, every aggregation frame
void fieldlog_amp(int node, int8_t rssi, const uint8_t *amp52);                   // Wi-Fi task: ~25 frames/s per link
void fieldlog_p2p(uint8_t rx, uint8_t tx, float tstd, float rssi, uint8_t state, uint8_t score, float thr, float rssi_base);   // one probe-to-probe window
bool fieldlog_label(const char *text);      // false when logging is off
void fieldlog_event(uint8_t type, uint8_t id, const char *text);
void fieldlog_clock(int64_t epoch_ms);
String fieldlog_status_json();
bool fieldlog_erase();
const char *fieldlog_path();
