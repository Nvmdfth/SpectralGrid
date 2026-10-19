// Host test: g++ -std=c++17 -I.. test_moved.cpp -o /tmp/tm && /tmp/tm
#include <cstdio>
#include "moved.h"
static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
static float jit(int i) { return 0.2f * ((i * 7919) % 11 - 5) / 5.0f; }
int main() {
    // a path 3 dB off its calibrated level: off only after MOVED_HOLD_MS, and never from a short disturbance
    { MovedPath m = MovedPath(); uint32_t t = 1000; bool early = false;
      for (int i = 0; i < 500; i++) { t += 500; moved_feed(&m, -36.0f + jit(i), -36.0f, t); }
      CHECK(!m.off);
      for (int i = 0; i < 2 * 170; i++) { t += 500; moved_feed(&m, -39.0f + jit(i), -36.0f, t); if (m.off) early = true; }   // 170 s
      CHECK(!early && !m.off);
      for (int i = 0; i < 2 * 40; i++) { t += 500; moved_feed(&m, -39.0f + jit(i), -36.0f, t); }                            // past 180 s (+ the smoothing lag)
      CHECK(m.off);
      for (int i = 0; i < 2 * 120; i++) { t += 500; moved_feed(&m, -36.0f + jit(i), -36.0f, t); }                           // back to the calibrated level
      CHECK(!m.off); }
    // a rise counts too (the hub path of the bumped node went UP)
    { MovedPath m = MovedPath(); uint32_t t = 1000;
      for (int i = 0; i < 2 * 400; i++) { t += 500; moved_feed(&m, -33.0f + jit(i), -36.0f, t); } CHECK(m.off); }
    // 1 dB is normal wobble: never off
    { MovedPath m = MovedPath(); uint32_t t = 1000;
      for (int i = 0; i < 2 * 2000; i++) { t += 500; moved_feed(&m, -37.0f + jit(i), -36.0f, t); } CHECK(!m.off); }
    // a person stands at a path for 60 s and leaves: not off
    { MovedPath m = MovedPath(); uint32_t t = 1000;
      for (int i = 0; i < 2 * 100; i++) { t += 500; moved_feed(&m, -36.0f + jit(i), -36.0f, t); }
      for (int i = 0; i < 2 * 60; i++) { t += 500; moved_feed(&m, -42.0f + jit(i), -36.0f, t); }
      for (int i = 0; i < 2 * 300; i++) { t += 500; moved_feed(&m, -36.0f + jit(i), -36.0f, t); }
      CHECK(!m.off); }
    // a new calibration (r0 changes) starts over: off is cleared at once and re-arms for the new level
    { MovedPath m = MovedPath(); uint32_t t = 1000;
      for (int i = 0; i < 2 * 400; i++) { t += 500; moved_feed(&m, -39.0f + jit(i), -36.0f, t); } CHECK(m.off);
      t += 500; moved_feed(&m, -39.0f, -39.0f, t); CHECK(!m.off);
      for (int i = 0; i < 2 * 400; i++) { t += 500; moved_feed(&m, -39.0f + jit(i), -39.0f, t); } CHECK(!m.off); }
    // which node: node 3 bumped = every path of node 3 off (hub-3, 1-3, 3-4), others have an unaffected path
    { PathAct p[6] = {{0,1,1,0,0},{0,3,1,0,1},{0,4,1,0,0},{1,3,1,0,1},{1,4,1,0,0},{3,4,1,0,1}};
      uint32_t k = moved_nodes(p, 6, 5);
      CHECK(k == (1u << 3));
      // a person at node 3 would also shadow its paths, but also other paths would stay normal: same result (time decides, see MOVED_HOLD_MS)
      p[1].off = 0; CHECK(moved_nodes(p, 6, 5) == 0);                    // one of node 3's paths is fine: not moved
      // a single dubious path never flags a node (needs >= 2 paths)
      PathAct q[1] = {{0,3,1,0,1}}; CHECK(moved_nodes(q, 1, 5) == 0);
      // everything off (the room changed): every node is flagged
      PathAct r[6] = {{0,1,1,0,1},{0,3,1,0,1},{0,4,1,0,1},{1,3,1,0,1},{1,4,1,0,1},{3,4,1,0,1}};
      CHECK(moved_nodes(r, 6, 5) == 0x1B); }
    printf(fails ? "FAILED\n" : "ok\n"); return fails ? 1 : 0;
}
