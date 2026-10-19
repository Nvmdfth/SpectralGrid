// Firmware glue for the event log, session recording, cause tags, control probe and drift prompts
// (SpectralGrid.md §9.2 items 6-8). All pure logic lives in events/session/tags/drift/ringlog (host-tested);
// this file adds locking, PSRAM, flash persistence and the clock.
#pragma once
#include <Arduino.h>
#include "hub.h"
#include "score.h"
#include "events.h"
#include "session.h"

void logger_init(ScoreState *s);                          // after calibration_init
void logger_observe(const GridFrame *f, bool pairing, int cal_phase);   // once per aggregation frame (Core 1)
void logger_event(uint8_t type, uint8_t id, uint8_t score, const char *text);   // thread-safe; also broadcast to clients
bool logger_pop_event(Event *out);
void web_event(const Event *e);                          // implemented in web.cpp: push one event to /ws clients (Core 1 loop only)                        // drains events queued for broadcast (call from the Core 1 loop)
void logger_radio_activity();                             // a client joined/left the AP: may explain hub EMF noise

// wall clock: the hub has no RTC; the dashboard posts the phone's clock when it connects
void logger_set_epoch_ms(int64_t epoch_ms);
bool logger_epoch_known();

bool logger_tag(const char *kind, const char *note, uint32_t dur_s);
bool logger_tag_clear(const char *kind);
String logger_tags_json();                                // JSON array of active tags
void logger_set_control(int id);                          // -1 clears; persisted
String logger_drift_json();                               // JSON array of active drift prompts
void logger_drift_reset_all();

String logger_events_json(uint32_t since_seq, int limit);
String logger_session_info_json();
int logger_session_frames(uint32_t from_ms, int limit, SessFrame *out);   // returns count copied (<= limit)

// chunked CSV exports: open returns a heap object (free with delete), fill returns 0 at the end
CsvStream *logger_session_csv_open();
size_t logger_session_csv_fill(CsvStream *c, uint8_t *out, size_t max);
struct EvCsvJob { EvCsv csv; Event *snap; ~EvCsvJob() { free(snap); } };
EvCsvJob *logger_events_csv_open();
size_t logger_events_csv_fill(EvCsvJob *j, uint8_t *out, size_t max);
