// g++ -std=c++17 -Wall -I../../shared -I../src test_score.cpp ../src/score.cpp -o /tmp/t && /tmp/t
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "score.h"
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)
#define VEC(f, s) (strcmp((f).vector, s) == 0)

static void mk(GridFrame *f, int n_probes, uint16_t emf0, uint16_t emf1, uint16_t emf2, float csi1, float d1) {
    memset(f, 0, sizeof(*f));
    f->n_nodes = 1 + n_probes;
    f->nodes[0] = {0, 0, emf0, 23.f, 0.f, true, 0, 0};
    if (n_probes >= 1) f->nodes[1] = {1, 0, emf1, 22.f, d1, true, -50, 0};
    if (n_probes >= 2) f->nodes[2] = {2, 0, emf2, 22.f, 0.f, true, -50, 0};
    f->n_links = n_probes;
    for (int i = 1; i <= n_probes; i++) f->links[i - 1] = {(uint8_t)i, (i == 1 ? csi1 : 10.f), 0, -50};
    if (n_probes == 0) { f->n_links = 1; f->links[0] = {0, 10.f, 0, 0}; }
}
static uint32_t t_ms = 0;
static void run(ScoreState *s, GridFrame *f, int n, int np, uint16_t e0, uint16_t e1, uint16_t e2, float c1, float d1) {
    for (int i = 0; i < n; i++) { mk(f, np, e0, e1, e2, c1, d1); score_compute(s, f, t_ms); t_ms += 50; }
}

int main() {
    ScoreState s; GridFrame f;
    score_init(&s);
    run(&s, &f, 400, 2, 10, 10, 10, 10.f, 0.f);                         // warm up with 2 probes
    CHECK(VEC(f, "QUIET") && f.score == 0);
    // single-node EMF spike on probe 1, links quiet
    run(&s, &f, 1, 2, 10, 90, 10, 10.f, 0.f);
    CHECK(VEC(f, "LOCAL_NODE_1") && f.vec_id == 1 && f.score > 0);
    run(&s, &f, 400, 2, 10, 10, 10, 10.f, 0.f);                         // let baseline settle
    // csi spike on link 1 only
    run(&s, &f, 1, 2, 10, 10, 10, 60.f, 0.f);
    CHECK(VEC(f, "BARRIER_BREACH_1"));
    run(&s, &f, 400, 2, 10, 10, 10, 10.f, 0.f);
    // two emf + csi
    run(&s, &f, 1, 2, 10, 90, 90, 60.f, 0.f);
    CHECK(VEC(f, "GRID_ANOMALY"));
    run(&s, &f, 400, 2, 10, 10, 10, 10.f, 0.f);
    // thermal: sub-gate and exactly-one-step drops are ignored (die sensor quantises ~1 degC); two steps count
    run(&s, &f, 1, 2, 10, 10, 10, 10.f, -0.4f);
    CHECK(VEC(f, "QUIET") && f.score == 0);
    run(&s, &f, 1, 2, 10, 10, 10, 10.f, -1.0f);
    CHECK(VEC(f, "QUIET") && f.score == 0);
    run(&s, &f, 1, 2, 10, 10, 10, 10.f, -0.99999f);
    CHECK(VEC(f, "QUIET"));
    run(&s, &f, 1, 2, 10, 10, 10, 10.f, 1.5f);                           // warming never counts
    CHECK(VEC(f, "QUIET"));
    run(&s, &f, 1, 2, 10, 10, 10, 10.f, -1.5f);
    CHECK(VEC(f, "THERMAL_1") && s.temp_hat[1] > 45.0f && s.temp_hat[1] < 55.0f);
    run(&s, &f, 1, 2, 10, 10, 10, 10.f, -2.0f);
    CHECK(VEC(f, "THERMAL_1") && s.temp_hat[1] >= 99.0f);
    // probes vanish -> degraded after hysteresis, not immediately
    run(&s, &f, 10, 0, 10, 0, 0, 0, 0);
    CHECK(s.mode == SCORE_MODE_GRID);
    run(&s, &f, 80, 0, 10, 0, 0, 0, 0);
    CHECK(s.mode == SCORE_MODE_DEGRADED);
    // hub AP-side RF disturbance in degraded mode (baseline for node 0 / link 0 is fresh)
    run(&s, &f, 400, 0, 10, 0, 0, 0, 0);
    mk(&f, 0, 10, 0, 0, 0, 0); f.links[0].csi_var = 80.f; score_compute(&s, &f, t_ms);
    CHECK(VEC(f, "RF_DISTURBANCE"));
    // offline probe is ignored
    ScoreState s2; score_init(&s2); GridFrame g;
    for (int i = 0; i < 300; i++) { mk(&g, 1, 10, 10, 0, 10.f, 0.f); g.nodes[1].online = false; score_compute(&s2, &g, i * 50); }
    CHECK(s2.emf[1].n == 0);
    // ---- control probe (node 2) ----
    {
        ScoreState c; score_init(&c); score_set_control(&c, 2); GridFrame g; uint32_t tt = 0;
        CHECK(c.control_id == 2); score_set_control(&c, 0); CHECK(c.control_id == -1); score_set_control(&c, 99); CHECK(c.control_id == -1); score_set_control(&c, 2);
        for (int i = 0; i < 400; i++) { mk(&g, 2, 10, 10, 10, 10.f, 0.f); score_compute(&c, &g, tt); tt += 50; }
        mk(&g, 2, 10, 10, 90, 10.f, 0.f); score_compute(&c, &g, tt); tt += 50;                    // control alone is disturbed
        CHECK(g.score == 0 && VEC(g, "QUIET") && g.control_id == 2);
        mk(&g, 2, 10, 90, 90, 10.f, 0.f); score_compute(&c, &g, tt); tt += 50;                    // control and node 1 together
        CHECK(VEC(g, "COMMON_MODE") && g.score > 0);
        for (int i = 0; i < 400; i++) { mk(&g, 2, 10, 10, 10, 10.f, 0.f); score_compute(&c, &g, tt); tt += 50; }
        mk(&g, 2, 10, 90, 10, 10.f, 0.f); score_compute(&c, &g, tt); tt += 50;                    // node 1 alone: still a real local anomaly
        CHECK(VEC(g, "LOCAL_NODE_1"));
        ScoreState n2; score_init(&n2);                                                              // same data without a control probe
        for (int i = 0; i < 400; i++) { mk(&g, 2, 10, 10, 10, 10.f, 0.f); score_compute(&n2, &g, tt); tt += 50; }
        mk(&g, 2, 10, 10, 90, 10.f, 0.f); score_compute(&n2, &g, tt);
        CHECK(VEC(g, "LOCAL_NODE_2") && g.score > 0 && g.control_id == -1);
    }
    {   // one alarm rule (pathfuse.h): with >= 3 calibrated paths a single weak link is not a detection; two links or one strong one is
        static ScoreState q; score_init(&q);
        pres_link_set_cal(&q.pres[1], 0.030f, 0.0366f, -50.0f); pres_link_set_cal(&q.pres[2], 0.030f, 0.0366f, -50.0f);
        q.n_ext_cal = 1; q.n_ext = 1; q.ext[0].a = 3; q.ext[0].b = 4; q.ext[0].state = PRES_CLEAR; q.ext[0].score = 0;   // a quiet probe pair: 3 paths
        static GridFrame g; uint32_t tt = 100000, seq = 0;
        auto feed = [&](float t1, float t2, int frames) {
            for (int i = 0; i < frames; i++) { mk(&g, 2, 10, 10, 10, 10.f, 0.f); seq++;
                g.links[0].tstd = t1; g.links[1].tstd = t2; g.links[0].rssi_mean = g.links[1].rssi_mean = -50.0f; g.links[0].win_seq = g.links[1].win_seq = seq;
                score_compute(&q, &g, tt); tt += 500; }
        };
        feed(0.030f, 0.030f, 6); CHECK(VEC(g, "QUIET") && q.fused.state == PRES_CLEAR);
        feed(0.050f, 0.030f, 6); CHECK(q.pres[1].state == PRES_MOTION && q.fused.state == PRES_CLEAR && VEC(g, "QUIET") && g.score < 20);   // one weak link
        feed(0.030f, 0.030f, 10);
        feed(0.085f, 0.030f, 6); CHECK(q.fused.state == PRES_MOTION && q.fused.strong && VEC(g, "ROOM_MOTION_1") && g.score >= 60);    // one strong link
        feed(0.030f, 0.030f, 10);
        feed(0.050f, 0.050f, 6); CHECK(q.fused.state == PRES_MOTION && q.fused.active == 2 && (VEC(g, "ROOM_MOTION_1") || VEC(g, "ROOM_MOTION_2")));
        feed(0.030f, 0.030f, 10); CHECK(q.fused.state == PRES_CLEAR);
        q.ext[0].state = PRES_MOTION; q.ext[0].score = 55;                                                                          // probe pair + one hub link
        feed(0.050f, 0.030f, 6); CHECK(q.fused.state == PRES_MOTION && q.fused.active == 2);
        q.ext[0].score = 70; feed(0.030f, 0.030f, 10); CHECK(q.fused.state == PRES_MOTION && VEC(g, "ROOM_MOTION"));                   // a strong probe pair alone: no single id
    }
    puts("ok"); return 0;
}
