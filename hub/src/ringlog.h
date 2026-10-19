// Byte ring buffer holding variable-length records ([u16 len][payload]); pushing drops the oldest records when full.
// Arduino-free, host-tested. Not thread-safe: callers lock.
#pragma once
#include <stdint.h>

typedef struct { uint8_t *buf; uint32_t cap, tail, used, count, dropped; } RingLog;
typedef struct { uint32_t pos, remaining, dropped_at_start; } RingIter;

void ringlog_init(RingLog *r, uint8_t *buf, uint32_t cap);
bool ringlog_push(RingLog *r, const uint8_t *rec, uint16_t len);   // false only if the record can never fit
void ringlog_iter(const RingLog *r, RingIter *it);                  // positions at the oldest record
// Copies the next record to `out` (at most `max` bytes, *len = copied). False at the end, or if the ring dropped
// records since the iterator was created (positions are then stale: call ringlog_iter again and re-seek).
bool ringlog_next(const RingLog *r, RingIter *it, uint8_t *out, uint16_t max, uint16_t *len);
