#include <string.h>
#include "ringlog.h"

static uint8_t rd8(const RingLog *r, uint32_t pos) { return r->buf[pos % r->cap]; }
static uint16_t rd16(const RingLog *r, uint32_t pos) { return (uint16_t)(rd8(r, pos) | (rd8(r, pos + 1) << 8)); }

// Block copies (at most one wrap): a byte loop with a modulo per byte made a full-ring seek hold the logger lock for ~3.4 s on the hub.
static void copy_in(RingLog *r, uint32_t pos, const uint8_t *src, uint32_t n) {
    uint32_t p = pos % r->cap, a = r->cap - p; if (a > n) a = n;
    memcpy(r->buf + p, src, a); if (n > a) memcpy(r->buf, src + a, n - a);
}
static void copy_out(const RingLog *r, uint32_t pos, uint8_t *dst, uint32_t n) {
    uint32_t p = pos % r->cap, a = r->cap - p; if (a > n) a = n;
    memcpy(dst, r->buf + p, a); if (n > a) memcpy(dst + a, r->buf, n - a);
}

void ringlog_init(RingLog *r, uint8_t *buf, uint32_t cap) { memset(r, 0, sizeof(*r)); r->buf = buf; r->cap = cap; }

static void pop_oldest(RingLog *r) {
    uint16_t len = rd16(r, r->tail);
    uint32_t sz = 2u + len;
    r->tail = (r->tail + sz) % r->cap;
    r->used -= sz; r->count--; r->dropped++;
}

bool ringlog_push(RingLog *r, const uint8_t *rec, uint16_t len) {
    uint32_t need = 2u + len;
    if (need > r->cap) return false;
    while (r->cap - r->used < need) pop_oldest(r);
    uint32_t head = (r->tail + r->used) % r->cap;
    uint8_t hdr[2] = {(uint8_t)(len & 0xFF), (uint8_t)(len >> 8)};
    copy_in(r, head, hdr, 2);
    copy_in(r, head + 2, rec, len);
    r->used += need; r->count++;
    return true;
}

void ringlog_iter(const RingLog *r, RingIter *it) { it->pos = r->tail; it->remaining = r->count; it->dropped_at_start = r->dropped; }

bool ringlog_next(const RingLog *r, RingIter *it, uint8_t *out, uint16_t max, uint16_t *len) {
    if (it->dropped_at_start != r->dropped || it->remaining == 0) return false;
    uint16_t n = rd16(r, it->pos);
    uint16_t c = n < max ? n : max;
    copy_out(r, it->pos + 2, out, c);
    *len = c;
    it->pos = (it->pos + 2u + n) % r->cap;
    it->remaining--;
    return true;
}
