// cd hub/host_test && g++ -std=c++17 -Wall -I../../shared -I../src test_calib.cpp ../src/calib.cpp ../src/score.cpp -o /tmp/t && /tmp/t
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include "calib.h"
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

static uint32_t rng = 12345;
static float noise() { rng = rng * 1664525u + 1013904223u; return ((rng >> 8) & 0xFFFF) / 65535.0f - 0.5f; }   // uniform +-0.5

static void mk(GridFrame *f, float p1, float p2, bool on2, float csi1) {
    memset(f, 0, sizeof(*f));
    f->n_nodes = 3;
    f->nodes[0] = {0, 0, (uint16_t)(3 + 2 * noise()), 31.f, 0.f, true, 0, 0};
    f->nodes[1] = {1, 0, (uint16_t)p1, 22.f, 0.f, true, -50, 0};
    f->nodes[2] = {2, 0, (uint16_t)p2, 22.f, 0.f, on2, -50, 0};
    f->n_links = 2;
    f->links[0] = {1, csi1, 0, -50};
    f->links[1] = {2, 10.f, 0, -50};
}

int main() {
    ScoreState s; score_init(&s);
    CalState c; cal_init(&c);
    GridFrame f; uint32_t t = 1000;

    CHECK(cal_start(&c, -1, CAL_SENS_NORMAL, t) && c.phase == CAL_CAPTURING);
    CHECK(!cal_start(&c, -1, CAL_SENS_NORMAL, t));                              // busy
    CHECK(cal_left_s(&c, t) > 14.9f);
    // quiet capture: node 1 p2p ~ 50 +- 3 (sd ~ 1.7), node 2 offline for the whole capture, link 1 csi ~ 12 +- 2
    for (; t < 1000 + CAL_CAPTURE_MS + 100; t += 50) { mk(&f, 50 + 6 * noise(), 5, false, 12 + 4 * noise()); cal_feed(&c, &f, &s, t); }
    CHECK(c.phase == CAL_PENDING);
    CHECK(c.pending[1].emf.valid && fabsf(c.pending[1].emf.mean - 50.0f) < 1.0f && c.pending[1].emf.sd > 1.2f && c.pending[1].emf.sd < 2.2f);
    CHECK(c.pending[1].emf.scale >= 12 * c.pending[1].emf.sd - 0.01f);          // normal sens = 12 sigma
    CHECK(c.pending[1].csi.valid && fabsf(c.pending[1].csi.mean - 12.0f) < 1.0f);
    CHECK(!c.pending[2].emf.valid && !c.pending[2].csi.valid);                  // offline node: not calibrated
    CHECK(c.pending[0].emf.valid && c.pending[0].emf.scale == CAL_EMF_FLOOR);   // tiny sigma => floor
    CHECK(c.pending[1].warn == CAL_OK);
    CHECK(s.emf_scale_n[1] == s.cfg.emf_scale);                                 // nothing applied before accept
    // accept
    CHECK(cal_accept(&c, &s) == 2 && c.phase == CAL_IDLE);                      // nodes 0 and 1
    CHECK(c.applied[1].emf.valid && s.emf[1].n >= s.cfg.warmup_n && s.emf_scale_n[1] > 15.0f);
    CHECK(c.applied[2].emf.valid == false && s.emf_scale_n[2] == s.cfg.emf_scale);
    // seeded baseline scores immediately (no 10 s warm-up)
    mk(&f, 50 + 150, 5, false, 12); score_compute(&s, &f, t);
    CHECK(s.emf_hat[1] > 50.0f);

    // spike during capture => warning; discard keeps previous calibration
    float keep = s.emf_scale_n[1];
    CHECK(cal_start(&c, 1, CAL_SENS_HIGH, t));
    for (uint32_t t0 = t, k = 0; t < t0 + CAL_CAPTURE_MS + 100; t += 50, k++) { mk(&f, (k == 100 ? 300.0f : 50 + 6 * noise()), 5, false, 12); cal_feed(&c, &f, &s, t); }
    CHECK(c.phase == CAL_PENDING && c.pending[1].warn == CAL_WARN_SPIKE);
    CHECK(c.pending[0].emf.valid == false);                                     // single-node target: hub untouched
    CHECK(c.pending[1].emf.prev_mean > 40.0f);                                  // before/after shown against live baseline
    cal_discard(&c);
    CHECK(c.phase == CAL_IDLE && s.emf_scale_n[1] == keep);

    // too few samples => channel invalid
    CHECK(cal_start(&c, 1, CAL_SENS_LOW, t));
    for (uint32_t t0 = t, k = 0; t < t0 + CAL_CAPTURE_MS + 100; t += 50, k++) { mk(&f, 50, 5, false, 12); f.nodes[1].online = (k < 20); cal_feed(&c, &f, &s, t); }
    CHECK(c.phase == CAL_PENDING && !c.pending[1].emf.valid && c.pending[1].emf.n == 20);
    CHECK(cal_accept(&c, &s) == 0 && c.applied[1].emf.valid);                   // nothing applied; old calibration survives

    // reset one node / all
    cal_reset(&c, &s, 1);
    CHECK(!c.applied[1].emf.valid && s.emf_scale_n[1] == s.cfg.emf_scale && s.emf[1].n == 0 && c.applied[0].emf.valid);
    cal_reset(&c, &s, -1);
    CHECK(!c.applied[0].emf.valid);
    CHECK(!cal_start(&c, 99, 1, t));                                            // bad target
    puts("ok"); return 0;
}
