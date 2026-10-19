// Host test for shared/touch.h.
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "touch.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
static float gauss(float s) { float u = (rand() + 1.0f) / (RAND_MAX + 2.0f), v = (rand() + 1.0f) / (RAND_MAX + 2.0f); return s * sqrtf(-2 * logf(u)) * cosf(6.2831853f * v); }

// Drive the detector for `secs` seconds at `hz` with a constant offset; returns the number of events fired.
static int run(TouchDet *t, uint32_t *now, float base, float sigma, float offset, float secs, int hz = 20) {
    int fires = 0; int n = (int)(secs * hz);
    for (int i = 0; i < n; i++) { *now += 1000 / hz; if (touch_update(t, base + offset + gauss(sigma), *now)) fires++; }
    return fires;
}

int main() {
    srand(7);
    // probe-like noise (sigma 5): quiet for 5 minutes fires nothing; a +100 hand fires once and clears; again fires again
    { TouchDet t; touch_reset(&t); uint32_t now = 0;
      CHECK(run(&t, &now, 18580, 5, 0, 300) == 0);
      CHECK(run(&t, &now, 18580, 5, 100, 4) == 1 && t.active && t.peak > 80);
      CHECK(run(&t, &now, 18580, 5, 0, 4) == 0 && !t.active);
      CHECK(run(&t, &now, 18580, 5, 0, 2) == 0);                       // lockout passed
      CHECK(run(&t, &now, 18580, 5, 120, 3) == 1); }
    // hub-like noise (sigma 28): +50 (hand at 30 cm) must not fire, +182 (hand at 10 cm) must
    { TouchDet t; touch_reset(&t); uint32_t now = 0;
      CHECK(run(&t, &now, 20090, 28, 0, 120) == 0);
      CHECK(run(&t, &now, 20090, 28, 50, 5) == 0);
      CHECK(run(&t, &now, 20090, 28, 0, 3) == 0);
      CHECK(run(&t, &now, 20090, 28, 182, 5) == 1); }
    // saturating touch fires once
    { TouchDet t; touch_reset(&t); uint32_t now = 0;
      CHECK(run(&t, &now, 20090, 28, 0, 30) == 0);
      CHECK(run(&t, &now, 20090, 28, 45000, 6) == 1); }
    // slow drift (+40 counts over 60 s) is absorbed by the baseline
    { TouchDet t; touch_reset(&t); uint32_t now = 0; int fires = 0;
      for (int i = 0; i < 1200; i++) { now += 50; fires += touch_update(&t, 19000 + 40.0f * i / 1200 + gauss(6), now); }
      CHECK(fires == 0); }
    // a permanent shift (wire moved) fires once, then is re-baselined and stays quiet
    { TouchDet t; touch_reset(&t); uint32_t now = 0;
      CHECK(run(&t, &now, 18580, 5, 0, 30) == 0);
      int f = run(&t, &now, 18580, 5, 400, 40);
      CHECK(f == 1 && !t.active); }
    // rate independence: the same hand at 2 Hz sampling also fires once
    { TouchDet t; touch_reset(&t); uint32_t now = 0;
      CHECK(run(&t, &now, 18580, 5, 0, 120, 2) == 0);
      CHECK(run(&t, &now, 18580, 5, 100, 8, 2) == 1); }
    printf(fails ? "FAILED\n" : "ok\n");
    return fails ? 1 : 0;
}
