// Host test for shared/csi_window.h and shared/presence.h (no board needed).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "csi_window.h"
#include "presence.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static void fill(int8_t *buf, float gain, float jitter) {      // 64 (imag, real) pairs, smooth magnitude ~13 plus optional jitter
    for (int k = 0; k < 64; k++) {
        float a = (13.0f + (k % 5)) * gain + jitter * ((rand() % 2001) / 1000.0f - 1.0f) * gain;
        buf[2 * k] = (int8_t)(a * 0.6f); buf[2 * k + 1] = (int8_t)(a * 0.8f);
    }
}
static float run_window(float gain, float jitter) {            // 50 frames -> one window result
    static CsiWin w; csiw_reset(&w); int8_t b[128]; float t = -1, r = 0;
    for (int i = 0; i < CSIW_FRAMES; i++) { fill(b, gain, jitter); if (csiw_push(&w, b, -40, &t, &r)) return t; }
    return -1;
}
static float jit(float s) { return s * ((rand() % 2001) / 1000.0f - 1.0f); }

int main() {
    srand(1);
    // window statistic: a steady channel reads ~0, a jittering one reads clearly higher, and it ignores overall gain
    float steady = run_window(1.0f, 0.0f), j1 = run_window(1.0f, 3.0f), j2 = run_window(2.0f, 3.0f);
    CHECK(steady >= 0 && steady < 0.02f);
    CHECK(j1 > steady + 0.03f);
    CHECK(j2 > j1 * 0.7f && j2 < j1 * 1.4f);
    { static CsiWin w; csiw_reset(&w); int8_t b[128]; float t, r; int got = 0;
      for (int i = 1; i <= 100; i++) { fill(b, 1, 1); if (csiw_push(&w, b, -40, &t, &r)) got++; }
      CHECK(got == 3); }

    // detector: quiet level 0.025, threshold 0.030, RSSI base -40
    PresLink p; pres_link_set_cal(&p, 0.025f, 0.030f, -40.0f); uint32_t seq = 0, now = 0;
    #define WIN(t, r) do { seq++; now += 500; pres_link_update(&p, (t), (r), seq, now); } while (0)
    for (int i = 0; i < 6; i++) WIN(0.025f, -40.0f);
    CHECK(p.state == PRES_CLEAR && p.score == 0);
    WIN(0.06f, -40.0f); CHECK(p.state == PRES_CLEAR);             // one window is not enough (2-of-3 vote)
    WIN(0.06f, -40.0f); CHECK(p.state == PRES_MOTION && p.score >= 40 && p.score <= 65);
    pres_link_update(&p, 0.06f, -40.0f, seq, now);                // the same window again must not count twice
    for (int i = 0; i < 4; i++) WIN(0.025f, -40.0f);
    CHECK(p.state == PRES_MOTION);                                // held for PRES_HOLD_MS (3 s) after the last active window
    for (int i = 0; i < 6; i++) WIN(0.025f, -40.0f);
    CHECK(p.state == PRES_CLEAR);
    WIN(0.025f, -42.0f); WIN(0.025f, -42.0f); CHECK(p.state == PRES_ON_LINE && !p.moving && p.score >= 60);
    WIN(0.07f, -42.0f); WIN(0.07f, -42.0f); CHECK(p.state == PRES_ON_LINE && p.moving);
    for (int i = 0; i < 12; i++) WIN(0.025f, -40.0f);
    CHECK(p.state == PRES_CLEAR);
    { PresLink q; pres_link_reset(&q); pres_link_update(&q, 1.0f, -90.0f, 1, 1000); CHECK(q.state == PRES_OFF); }

    // calibration: median-based threshold, sensitivity, robustness to a few disturbed windows, warning on movement
    { PresCalAcc a; pres_cal_reset(&a);
      for (uint32_t i = 1; i <= 11; i++) pres_cal_add(&a, 0.025f, -40.0f, i);
      CHECK(!pres_cal_result(&a, PRES_G_NORMAL).valid);          // too few windows
      for (uint32_t i = 12; i <= 60; i++) pres_cal_add(&a, 0.025f + jit(0.0004f), -40.0f + jit(0.3f), i);
      pres_cal_add(&a, 0.025f, -40.0f, 60);                       // duplicate window ignored
      PresCalResult n = pres_cal_result(&a, PRES_G_NORMAL), lo = pres_cal_result(&a, PRES_G_LOW), hi = pres_cal_result(&a, PRES_G_HIGH);
      CHECK(n.valid && n.n == 60 && !n.warn);
      CHECK(fabsf(n.q0 - 0.025f) < 0.0005f);
      CHECK(n.thr > 0.025f * 1.21f && n.thr < 0.025f * 1.25f);   // quiet level x 1.22
      CHECK(lo.thr > n.thr && hi.thr < n.thr);                    // low = fewer alerts, high = more sensitive
      CHECK(fabsf(n.rssi_base + 40.0f) < 0.3f); }
    { PresCalAcc a; pres_cal_reset(&a);                           // 3 disturbed windows of 60: ignored, no warning
      for (uint32_t i = 1; i <= 60; i++) pres_cal_add(&a, (i == 7 || i == 30 || i == 31) ? 0.07f : 0.025f + jit(0.0004f), -40.0f, i);
      PresCalResult r = pres_cal_result(&a, PRES_G_NORMAL);
      CHECK(r.valid && !r.warn && r.thr < 0.025f * 1.26f && r.tmax > 0.06f); }
    { PresCalAcc a; pres_cal_reset(&a);                           // 8 disturbed windows: warning, threshold still not inflated by them
      for (uint32_t i = 1; i <= 60; i++) pres_cal_add(&a, (i % 8 == 0) ? 0.07f : 0.025f + jit(0.0004f), -40.0f, i);
      PresCalResult r = pres_cal_result(&a, PRES_G_NORMAL);
      CHECK(r.valid && r.warn && r.thr < 0.025f * 1.3f); }
    { PresCalAcc a; pres_cal_reset(&a);                           // very noisy but still room: the 8-sigma guard lifts the threshold
      for (uint32_t i = 1; i <= 60; i++) pres_cal_add(&a, 0.03f + jit(0.006f), -40.0f, i);
      PresCalResult r = pres_cal_result(&a, PRES_G_HIGH);
      CHECK(r.valid && r.thr > 0.03f * 1.3f); }

    // learning over time (2 windows per second)
    { PresLink q; pres_link_set_cal(&q, 0.025f, 0.0305f, -40.0f); uint32_t sq = 0, tn = 0; int nonclear = 0;
      for (int i = 0; i < 7200 * 2; i++) {                        // 2 h: the quiet level creeps up 25 %, RSSI down 1.5 dB
          float f = (float)i / (7200 * 2);
          sq++; tn += 500; pres_link_update(&q, 0.025f * (1 + 0.25f * f) + jit(0.0003f), -40.0f - 1.5f * f + jit(0.2f), sq, tn);
          if (q.state > PRES_CLEAR) nonclear++;
      }
      CHECK(nonclear == 0 && !q.drift);
      CHECK(q.q > 0.025f * 1.12f && q.thr > 0.0305f * 1.12f && q.rssi_base < -40.5f); }
    { PresLink q; pres_link_set_cal(&q, 0.025f, 0.0305f, -40.0f); uint32_t sq = 0, tn = 0; bool clear = false;
      for (int i = 0; i < 120; i++) { sq++; tn += 500; pres_link_update(&q, 0.025f + jit(0.0003f), -40.0f, sq, tn); }   // 1 min quiet
      float q0 = q.q;
      for (int i = 0; i < 600 * 2; i++) { sq++; tn += 500; pres_link_update(&q, 0.055f, -40.0f, sq, tn); if (q.state <= PRES_CLEAR && i > 4) clear = true; }   // 10 min of motion
      CHECK(!clear && fabsf(q.q - q0) < 0.0003f);                 // never absorbed
      CHECK(!q.drift); }
    // --- shadow onset classification: a person arrives with motion, a level step does not ---
    #define QUIET(q, n) for (int _i = 0; _i < (n); _i++) { sq++; tn += 500; pres_link_update(&(q), 0.025f + jit(0.0002f), -40.0f + jit(0.15f), sq, tn); }
    { PresLink q; pres_link_set_cal(&q, 0.025f, 0.0305f, -40.0f); uint32_t sq = 0, tn = 0; bool dropped = false;     // a person walks onto the line and stands still
      QUIET(q, 120);
      for (int i = 0; i < 6; i++) { sq++; tn += 500; pres_link_update(&q, 0.06f, -42.0f - 0.3f * i, sq, tn); }       // 3 s of arrival motion, signal falling
      for (int i = 0; i < 600 * 2; i++) { sq++; tn += 500; pres_link_update(&q, 0.025f + jit(0.0002f), -44.0f + jit(0.15f), sq, tn); if (q.state != PRES_ON_LINE && i > 4) dropped = true; }   // 10 min still
      CHECK(!dropped && q.relearn_n == 0 && q.sh_class == 2);
      for (int i = 0; i < 660 * 2; i++) { sq++; tn += 500; pres_link_update(&q, 0.025f + jit(0.0002f), -44.0f + jit(0.15f), sq, tn); }   // past 20 min of perfect stillness: the new normal
      CHECK(q.relearn_n == 1 && q.state == PRES_CLEAR && q.rssi_base < -43.5f); }
    { PresLink q; pres_link_set_cal(&q, 0.030f, 0.0366f, -34.5f); uint32_t sq = 0, tn = 0; int on_line = 0;           // 2026-10-08 anomaly: stale baseline 1.7 dB high, then a 2.8 dB step, no motion
      for (int i = 0; i < 300 * 2; i++) { sq++; tn += 500; pres_link_update(&q, 0.031f + jit(0.001f), -36.2f + jit(0.15f), sq, tn); if (q.state == PRES_ON_LINE) on_line++; }
      CHECK(q.relearn_n == 1 && q.state == PRES_CLEAR && fabsf(q.rssi_base + 36.2f) < 0.3f && on_line > 20 && on_line < 70);   // recovers within ~25 s
      on_line = 0;
      for (int i = 0; i < 300 * 2; i++) { sq++; tn += 500; float lvl = -36.2f - 2.8f * (i < 40 ? i / 40.0f : 1.0f); pres_link_update(&q, 0.029f + jit(0.001f), lvl + jit(0.15f), sq, tn); if (q.state == PRES_ON_LINE) on_line++; }   // the step takes ~20 s
      CHECK(q.relearn_n == 2 && q.state == PRES_CLEAR && fabsf(q.rssi_base + 39.0f) < 0.3f && !q.drift && on_line < 120); }
    { PresLink q; pres_link_set_cal(&q, 0.025f, 0.0305f, -40.0f); uint32_t sq = 0, tn = 0;                           // a level step DURING motion looks like a person: never absorbed quickly
      QUIET(q, 120);
      for (int i = 0; i < 600 * 2; i++) { sq++; tn += 500; pres_link_update(&q, (i % 6 < 3) ? 0.06f : 0.025f, -43.0f, sq, tn); }
      CHECK(q.relearn_n == 0 && q.state == PRES_ON_LINE); }
    { PresLink q; pres_link_set_cal(&q, 0.025f, 0.0305f, -40.0f); uint32_t sq = 0, tn = 0;                           // wandering shadow (a person fidgeting): never absorbed
      QUIET(q, 120);
      for (int i = 0; i < 6; i++) { sq++; tn += 500; pres_link_update(&q, 0.06f, -42.0f, sq, tn); }
      for (int i = 0; i < 1500 * 2; i++) { sq++; tn += 500; pres_link_update(&q, 0.025f, -44.0f + 1.8f * sinf(i * 0.3f), sq, tn); }
      CHECK(q.relearn_n == 0); }
    { PresLink q; pres_link_set_cal(&q, 0.025f, 0.0305f, -40.0f); uint32_t sq = 0, tn = 0;                           // stale baseline restored at boot with nobody moving: replaced after ~20 s
      for (int i = 0; i < 80; i++) { sq++; tn += 500; pres_link_update(&q, 0.025f + jit(0.0002f), -43.0f + jit(0.15f), sq, tn); }
      CHECK(q.relearn_n == 1 && q.state == PRES_CLEAR && fabsf(q.rssi_base + 43.0f) < 0.3f); }
    { PresLink q; pres_link_set_cal(&q, 0.025f, 0.0305f, -40.0f); uint32_t sq = 0, tn = 0;                           // the signal comes back up 2 dB and stays: re-learn upward within about a minute
      QUIET(q, 120);
      for (int i = 0; i < 90 * 2; i++) { sq++; tn += 500; pres_link_update(&q, 0.025f, -38.0f + jit(0.1f), sq, tn); }
      CHECK(q.relearn_n == 1 && q.rssi_base > -38.4f && q.state == PRES_CLEAR); }
    { PresLink q; pres_link_set_cal(&q, 0.025f, 0.0305f, -40.0f); uint32_t sq = 0, tn = 0; int on_line = 0;          // 2026-10-16: a person at the node raises the RSSI 2.6 dB for 90 s (learned up), then leaves
      QUIET(q, 120);
      for (int i = 0; i < 90 * 2; i++) { sq++; tn += 500; pres_link_update(&q, 0.025f, -37.4f + jit(0.1f), sq, tn); }
      CHECK(q.relearn_n == 1 && q.rssi_base > -37.8f);
      for (int i = 0; i < 30 * 2; i++) { sq++; tn += 500; pres_link_update(&q, 0.025f, -40.0f + jit(0.15f), sq, tn); if (q.state == PRES_ON_LINE) on_line++; }
      CHECK(q.relearn_n == 2 && q.state == PRES_CLEAR && fabsf(q.rssi_base + 40.0f) < 0.3f && on_line <= 12);          // back to the old baseline within a few seconds, not 20 min
      QUIET(q, 120); CHECK(q.relearn_n == 2 && q.state == PRES_CLEAR); }
    { PresLink q; pres_link_set_cal(&q, 0.025f, 0.0305f, -40.0f); uint32_t sq = 0, tn = 0;                           // after the upward re-learn the signal falls to a DIFFERENT level (a real shadow): not reverted
      QUIET(q, 120);
      for (int i = 0; i < 90 * 2; i++) { sq++; tn += 500; pres_link_update(&q, 0.025f, -37.4f + jit(0.1f), sq, tn); }
      for (int i = 0; i < 10 * 2; i++) { sq++; tn += 500; pres_link_update(&q, 0.025f, -43.0f + jit(0.15f), sq, tn); }
      CHECK(q.relearn_n == 1 && q.state == PRES_ON_LINE); }
    { PresLink q; pres_link_set_cal(&q, 0.025f, 0.0305f, -40.0f); uint32_t sq = 0, tn = 0;                           // the revert window is 10 min: a raise that stayed longer is the new normal
      QUIET(q, 120);
      for (int i = 0; i < 90 * 2; i++) { sq++; tn += 500; pres_link_update(&q, 0.025f, -37.4f + jit(0.1f), sq, tn); }
      for (int i = 0; i < 660 * 2; i++) { sq++; tn += 500; pres_link_update(&q, 0.025f, -37.4f + jit(0.1f), sq, tn); }
      for (int i = 0; i < 4 * 2; i++) { sq++; tn += 500; pres_link_update(&q, 0.025f, -40.0f + jit(0.15f), sq, tn); }
      CHECK(q.relearn_n == 1 && q.state == PRES_ON_LINE); }
    { PresLink q; pres_link_set_cal(&q, 0.025f, 0.0305f, -40.0f); uint32_t sq = 0, tn = 0;
      for (int i = 0; i < 120; i++) { sq++; tn += 500; pres_link_update(&q, 0.025f, -40.0f, sq, tn); }
      for (int i = 0; i < 700 * 2; i++) { sq++; tn += 500; pres_link_update(&q, 0.04f, -40.0f, sq, tn); }       // permanent +60 %: the room changed
      CHECK(q.drift); }                                           // raises the recalibrate flag instead of learning it
    { PresLink q; pres_link_set_cal(&q, 0.025f, 0.0305f, -40.0f); uint32_t sq = 0, tn = 0;
      for (int i = 0; i < 30000 * 2; i++) { sq++; tn += 500; pres_link_update(&q, 0.025f * 1.5f - 0.0001f * (i % 2), -40.0f, sq, tn); }   // slow creep that the clamp stops
      /* the value 0.0375 is above the 0.0305 threshold, so it is never learned: still flagged by the stuck rule */
      CHECK(q.drift); }

    printf(fails ? "FAILED\n" : "ok\n");
    return fails ? 1 : 0;
}
