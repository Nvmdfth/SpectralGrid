// Host test: g++ -std=c++17 -I.. test_shared.cpp -o /tmp/t && /tmp/t
#include <cstdio>
#include <cstdlib>
#include "protocol.h"
#include "dsp.h"
#include "batt.h"
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)
int main() {
    PktHdr h = make_hdr(PKT_TELEMETRY, 3);
    CHECK(hdr_compatible(&h)); h.proto_ver = 99; CHECK(!hdr_compatible(&h));
    uint16_t v[] = {10, 50, 20, 5}; CHECK(p2p_u16(v, 4) == 45);
    Baseline b; baseline_init(&b, 0.02f);
    for (int i = 0; i < 500; i++) baseline_update(&b, 100.0f + (i % 2));
    CHECK(baseline_score(&b, 100.5f, 2.5f, 50, 100) == 0);
    CHECK(baseline_score(&b, 160.0f, 2.5f, 50, 100) > 50);
    CHECK(baseline_score(&b, 160.0f, 2.5f, 50, 100000) == 0);
    float y[] = {0, 1, 2, 3}; float s = slope_ls(y, 4); CHECK(s > 0.99f && s < 1.01f);
    int8_t flat[128]; for (int i = 0; i < 128; i++) flat[i] = (i & 1) ? 10 : 0;
    CHECK(csi_amp_variance(flat, 6, 58) < 1e-3f);
    // slot timing
    CHECK(proto_clamp_tx_hz(60) == 50 && proto_clamp_tx_hz(30) == 25 && proto_clamp_tx_hz(1) == 10);
    CHECK(proto_slots_per_frame(50) == 13 && proto_slots_per_frame(25) == 26);
    CHECK(slot_wait_us(0, 50, 0) == 20000);                 // at the instant: next frame, never 0
    CHECK(slot_wait_us(0, 50, 2) == 3000);
    CHECK(slot_wait_us(2999, 50, 2) == 1);
    CHECK(slot_wait_us(3001, 50, 2) == 19999);
    CHECK(slot_wait_us(20000ull * 7 + 500, 50, 1) == 1000); // phase independent of frame count
    for (uint64_t t = 0; t < 100000; t += 777)               // distinct slots never fire at the same instant
        for (int a = 0; a < 13; a++) for (int b = a + 1; b < 13; b++)
            CHECK(((t + slot_wait_us(t, 50, a)) % 20000) != ((t + slot_wait_us(t, 50, b)) % 20000));
    // masked LLTF variance: guard/DC carriers must not matter, usable carriers must
    int8_t l[128]; for (int k = 0; k < 64; k++) { l[2 * k] = 0; l[2 * k + 1] = 10; }
    CHECK(csi_lltf_variance(l) < 1e-3f);
    for (int k = 27; k <= 37; k++) l[2 * k + 1] = 120;           // wild guard carriers
    l[1] = 100;                                                   // wild DC
    CHECK(csi_lltf_variance(l) < 1e-3f);
    l[2 * 10 + 1] = 60;                                           // usable carrier moves
    CHECK(csi_lltf_variance(l) > 10.0f);
    // slope ring: constant => 0; ramp of 0.1/step => 3.0 once the ring is full; partial ring compares to first sample
    SlopeRing sr; slope_ring_init(&sr);
    CHECK(slope_ring_push(&sr, 25.0f) == 0.0f);
    for (int i = 0; i < 40; i++) CHECK(slope_ring_push(&sr, 25.0f) == 0.0f);
    slope_ring_init(&sr);
    float d = 0;
    for (int i = 0; i < 10; i++) d = slope_ring_push(&sr, 20.0f + 0.1f * i);
    CHECK(d > 0.89f && d < 0.91f);                               // 9 steps back to first sample
    for (int i = 10; i < 60; i++) d = slope_ring_push(&sr, 20.0f + 0.1f * i);
    CHECK(d > 2.99f && d < 3.01f);                               // full ring: 30 steps back
    // battery helpers
    CHECK(batt_mv_from_pin(1950, 2.0f) == 3900 && batt_mv_from_pin(0, 2.0f) == 0 && batt_mv_from_pin(100000, 2.0f) == 65535);
    CHECK(batt_pct(0) == 0 && batt_pct(3300) == 0 && batt_pct(4200) == 100 && batt_pct(4300) == 100 && batt_pct(3750) == 50);
    CHECK(!batt_low(0) && batt_low(3299) && !batt_low(3300));
    puts("ok"); return 0;
}
