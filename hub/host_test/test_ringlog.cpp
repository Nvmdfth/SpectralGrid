// cd hub/host_test && g++ -std=c++17 -Wall -I../src test_ringlog.cpp ../src/ringlog.cpp -o /tmp/t && /tmp/t
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "ringlog.h"
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)
static uint32_t rng = 1;
static uint32_t rnd() { rng = rng * 1664525u + 1013904223u; return rng >> 8; }
int main() {
    uint8_t buf[200]; RingLog r; ringlog_init(&r, buf, sizeof(buf));
    uint8_t rec[64], out[64]; uint16_t n;
    CHECK(!ringlog_push(&r, rec, 199));                              // 2 + 199 > 200: can never fit
    // fill with identifiable records and check order, content and wrap integrity across many pushes
    uint32_t seq = 0, expect_first = 0;
    for (int i = 0; i < 5000; i++) {
        uint16_t len = 4 + rnd() % 40;
        memset(rec, 0, sizeof(rec));
        memcpy(rec, &seq, 4); for (int k = 4; k < len; k++) rec[k] = (uint8_t)(seq + k);
        CHECK(ringlog_push(&r, rec, len)); seq++;
        CHECK(r.used <= r.cap && r.count > 0 && r.count == seq - r.dropped);
    }
    RingIter it; ringlog_iter(&r, &it);
    uint32_t prev = 0; int got = 0; bool first = true;
    while (ringlog_next(&r, &it, out, sizeof(out), &n)) {
        uint32_t s; memcpy(&s, out, 4);
        if (!first) { CHECK(s == prev + 1); }
        first = false; prev = s; got++;
        for (int k = 4; k < n; k++) CHECK(out[k] == (uint8_t)(s + k));
    }
    CHECK((uint32_t)got == r.count && prev == seq - 1 && r.dropped == seq - r.count);
    (void)expect_first;
    // iterator is invalidated when the ring drops records underneath it
    ringlog_iter(&r, &it); ringlog_push(&r, rec, 60); ringlog_push(&r, rec, 60); ringlog_push(&r, rec, 60);
    CHECK(!ringlog_next(&r, &it, out, sizeof(out), &n));
    // exact fit and empty ring
    RingLog e; uint8_t eb[10]; ringlog_init(&e, eb, 10);
    ringlog_iter(&e, &it); CHECK(!ringlog_next(&e, &it, out, sizeof(out), &n));
    CHECK(ringlog_push(&e, rec, 8) && e.used == 10 && ringlog_push(&e, rec, 8) && e.count == 1 && e.dropped == 1);
    puts("ok"); return 0;
}
