// Host test: g++ -std=c++17 -I.. test_p2p.cpp -o /tmp/tp && /tmp/tp [CSV CAL_T0 CAL_T1 [EVAL_T0 EVAL_T1]]
// Without arguments: synthetic checks. With a recording ("t_s,link,tstd,rssi", links like "3>4" = node 3 heard node 4): calibrates on
// CAL_T0..CAL_T1, then prints the share of seconds the fused verdict is not CLEAR inside EVAL_T0..EVAL_T1.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include "p2pfuse.h"
static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
static void synthetic() {
    P2pSet s; p2p_reset(&s);
    uint32_t t = 1000; uint16_t w = 0;
    auto step = [&](float a, float b, float c) {                       // pairs 1-2, 1-3, 2-3 (one direction each) at 1 Hz
        t += 1000; w++;
        p2p_feed(&s, 1, 2, a, -35, w, t); p2p_feed(&s, 1, 3, b, -36, w, t); p2p_feed(&s, 2, 3, c, -37, w, t);
    };
    p2p_cal_begin(&s, -1);
    for (int i = 0; i < 30; i++) step(0.040f, 0.038f, 0.036f);
    p2p_cal_finish(&s, PRES_G_NORMAL);
    bool warn; CHECK(p2p_cal_pending(&s, &warn) && !warn);
    int sk = 0; CHECK(p2p_cal_accept(&s, &sk) == 3 && sk == 0);
    for (int i = 0; i < 20; i++) step(0.040f, 0.038f, 0.036f);
    CHECK(p2p_verdict(&s, t).state == PRES_CLEAR);
    for (int i = 0; i < 6; i++) step(0.200f, 0.038f, 0.036f);          // one pair alone: not enough
    CHECK(p2p_verdict(&s, t).state == PRES_CLEAR);
    for (int i = 0; i < 6; i++) step(0.200f, 0.150f, 0.036f);          // two pairs
    P2pVerdict v = p2p_verdict(&s, t); CHECK(v.state == PRES_MOTION && v.pairs == 2 && v.score >= 40);
    for (int i = 0; i < 6; i++) { t += 1000; w++; p2p_feed(&s, 1, 2, 0.200f, -35, w, t); p2p_feed(&s, 2, 1, 0.200f, -35, w, t); p2p_feed(&s, 1, 3, 0.038f, -36, w, t); }
    CHECK(p2p_verdict(&s, t).pairs <= 2);                              // both directions of one pair are still ONE pair
    t += 10000; CHECK(p2p_verdict(&s, t).state == PRES_CLEAR);        // no reports for 10 s: stale links are ignored
    // a moving capture must not be accepted
    p2p_cal_begin(&s, -1);
    for (int i = 0; i < 30; i++) step(i % 3 ? 0.040f : 0.200f, 0.038f, 0.036f);
    p2p_cal_finish(&s, PRES_G_NORMAL); sk = 0;
    int ok = p2p_cal_accept(&s, &sk); CHECK(ok == 2 && sk == 1);
    // an uncalibrated set reports OFF
    P2pSet e; p2p_reset(&e); p2p_feed(&e, 1, 2, 0.04f, -35, 1, 1000); CHECK(p2p_verdict(&e, 1500).state == PRES_OFF);
}
static void fusion_rule() {
    auto P = [](uint8_t a, uint8_t b, int st, float sc) { PathAct x; x.a = a; x.b = b; x.state = (uint8_t)st; x.score = sc; return x; };
    PathAct p[4];
    p[0] = P(0, 3, PRES_MOTION, 50); CHECK(fuse_paths(p, 1, 2).state == PRES_MOTION);          // < 3 calibrated paths: one is enough
    CHECK(fuse_paths(p, 1, 6).state == PRES_CLEAR);                                             // 6 paths: one weak path is not
    p[0] = P(0, 3, PRES_MOTION, 70); FuseVerdict v = fuse_paths(p, 1, 6);
    CHECK(v.state == PRES_MOTION && v.strong && v.a == 0 && v.b == 3);                          // ... unless it is strong
    p[0] = P(0, 3, PRES_MOTION, 50); p[1] = P(3, 4, PRES_MOTION, 45); v = fuse_paths(p, 2, 6);
    CHECK(v.state == PRES_MOTION && v.active == 2 && v.score == 50 && v.b == 3);               // two weak paths
    p[1] = P(0, 3, PRES_MOTION, 48); CHECK(fuse_paths(p, 2, 6).state == PRES_CLEAR);           // the same path twice counts once
    p[0] = P(1, 4, PRES_ON_LINE, 62); p[1] = P(3, 4, PRES_MOTION, 45); CHECK(fuse_paths(p, 2, 6).state == PRES_ON_LINE);
    p[0] = P(1, 4, PRES_CLEAR, 0); CHECK(fuse_paths(p, 1, 6).state == PRES_CLEAR);
    p[0] = P(0, 1, PRES_ON_LINE, 61); v = fuse_paths(p, 1, 6); CHECK(v.state == PRES_ON_LINE && v.strong);   // one shadowed line alarms alone
    CHECK(fuse_paths(p, 0, 0).state == PRES_OFF);
    // p2p_paths: both directions of a pair become one path with the stronger state; calibrated pairs are counted
    P2pSet s; p2p_reset(&s); uint32_t t = 1000; uint16_t w = 0;
    p2p_cal_begin(&s, -1);
    for (int i = 0; i < 30; i++) { t += 1000; w++; p2p_feed(&s, 3, 4, 0.04f, -36, w, t); p2p_feed(&s, 4, 3, 0.04f, -36, w, t); p2p_feed(&s, 1, 3, 0.03f, -33, w, t); }
    p2p_cal_finish(&s, PRES_G_NORMAL); int sk; CHECK(p2p_cal_accept(&s, &sk) == 3);
    for (int i = 0; i < 4; i++) { t += 1000; w++; p2p_feed(&s, 3, 4, 0.04f, -36, w, t); p2p_feed(&s, 4, 3, 0.20f, -36, w, t); p2p_feed(&s, 1, 3, 0.03f, -33, w, t); }
    PathAct out[8]; int nc = 0; int n = p2p_paths(&s, t, out, 8, &nc);
    CHECK(n == 2 && nc == 2);
    bool found = false; for (int i = 0; i < n; i++) if (out[i].a == 3 && out[i].b == 4) { found = true; CHECK(out[i].state == PRES_MOTION && out[i].score >= 60); }
    CHECK(found);
    t += 20000; n = p2p_paths(&s, t, out, 8, &nc); CHECK(n == 0 && nc == 2);                    // stale: no paths, still calibrated
}
static void recorded(const char *path, double c0, double c1, double e0, double e1) {
    FILE *f = fopen(path, "r"); if (!f) { printf("cannot open %s\n", path); fails++; return; }
    static P2pSet s; p2p_reset(&s);
    char line[128]; double t; char lk[16]; float ts; int rs; uint16_t w[64] = {0};
    bool began = false, fin = false; int secs = 0, act = 0; int last_sec = -1; bool sec_act = false;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "%lf,%15[^,],%f,%d", &t, lk, &ts, &rs) != 4) continue;
        int a, b; if (sscanf(lk, "%d>%d", &a, &b) != 2) continue;     // hub links (h-N) are not part of this
        if (t >= c0 && !began) { p2p_cal_begin(&s, -1); began = true; }
        if (t >= c1 && began && !fin) { p2p_cal_finish(&s, PRES_G_NORMAL); int sk; int ok = p2p_cal_accept(&s, &sk); printf("calibrated %d link(s), skipped %d\n", ok, sk); fin = true; }
        uint32_t ms = (uint32_t)(t * 1000) + 1;
        if (t < c0 && !began) { continue; }
        p2p_feed(&s, (uint8_t)a, (uint8_t)b, ts, (float)rs, ++w[a * 8 + b], ms);
        if (fin && t >= e0 && t < e1) {
            P2pVerdict v = p2p_verdict(&s, ms);
            int sec = (int)t; if (sec != last_sec) { if (last_sec >= 0) { secs++; if (sec_act) act++; } last_sec = sec; sec_act = false; }
            if (v.state > PRES_CLEAR) sec_act = true;
        }
    }
    fclose(f);
    printf("eval %.0f-%.0f s: %d s, fused verdict not CLEAR in %d s (%.2f%%)\n", e0, e1, secs, act, secs ? 100.0 * act / secs : 0.0);
}
int main(int argc, char **argv) {
    synthetic();
    fusion_rule();
    if (argc >= 4) recorded(argv[1], atof(argv[2]), atof(argv[3]), argc > 5 ? atof(argv[4]) : atof(argv[3]), argc > 5 ? atof(argv[5]) : 1e9);
    printf(fails ? "FAILED\n" : "ok\n");
    return fails ? 1 : 0;
}
