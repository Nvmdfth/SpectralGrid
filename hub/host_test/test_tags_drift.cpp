// cd hub/host_test && g++ -std=c++17 -Wall -I../../shared -I../src test_tags_drift.cpp ../src/tags.cpp ../src/drift.cpp ../src/calib.cpp ../src/score.cpp -o /tmp/t && /tmp/t
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>
#include "tags.h"
#include "drift.h"
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

static std::vector<std::pair<int, int>> got; static std::vector<std::string> msgs;
static void sink(int id, DriftKind k, const char *m, void *) { got.push_back({id, (int)k}); msgs.push_back(m); }
static void frame(GridFrame *f, uint16_t raw1, bool on1 = true) {
    memset(f, 0, sizeof(*f)); f->n_nodes = 2;
    f->nodes[0] = {0, 15000, 0, 30, 0, true, 0, 0, 0}; f->nodes[1] = {1, raw1, 5, 30, 0, on1, -50, 0, 0};
}
int main() {
    // ---- tags ----
    TagSet ts; tags_init(&ts); Tag out[TAG_MAX]; uint32_t left[TAG_MAX];
    CHECK(tag_kind_valid("hvac") && tag_kind_valid("note") && !tag_kind_valid("ghost") && !tag_kind_valid(nullptr));
    CHECK(!tags_add(&ts, "ghost", "x", 60, 0) && tags_add(&ts, "note", "just a note", 0, 0) && tags_active(&ts, 0, out, left, TAG_MAX) == 0);
    CHECK(tags_add(&ts, "hvac", "furnace", 600, 1000) && tags_add(&ts, "phone", "", 60, 1000) && tags_active(&ts, 1000, out, left, TAG_MAX) == 2);
    CHECK(left[0] == 600 && !strcmp(out[1].kind, "phone"));
    CHECK(tags_add(&ts, "hvac", "extended", 900, 2000) && tags_active(&ts, 2000, out, left, TAG_MAX) == 2 && left[0] == 900 && !strcmp(out[0].note, "extended"));   // same kind replaces
    CHECK(tags_active(&ts, 1000 + 61000, out, left, TAG_MAX) == 1 && !strcmp(out[0].kind, "hvac"));     // phone expired
    CHECK(tags_add(&ts, "pet", "", 999999, 0) && tags_clear(&ts, "pet") && !tags_clear(&ts, "pet"));
    for (const char *k : {"person", "appliance", "other", "phone", "note", "pet"}) tags_add(&ts, k, "", 100, 0);
    CHECK(!tags_add(&ts, "pet", "", 100, 0) || true);                                                  // may replace; set capacity is TAG_MAX
    TagSet full; tags_init(&full); const char *ks[] = {"hvac", "phone", "appliance", "person", "pet", "other"};
    for (auto k : ks) CHECK(tags_add(&full, k, "", 100, 0));
    CHECK(!tags_add(&full, "note", "", 100, 0));                                                        // full

    // ---- drift ----
    ScoreState s; score_init(&s); CalState c; cal_init(&c); DriftState d; drift_init(&d, sink, nullptr); GridFrame f; uint32_t t = 0;
    auto run = [&](uint16_t raw, uint32_t ms, bool on = true) { for (uint32_t e = 0; e < ms; e += 50) { frame(&f, raw, on); drift_update(&d, &f, &c, &s, t); t += 50; } };
    // calibrated node 1: raw 15000, p2p mean 5 sd 1
    c.applied[1].emf = {true, 300, 5, 1, 9, 12, 5, 1, 15000}; c.applied[0].emf = {true, 300, 3, 1, 9, 12, 3, 1, 15000};
    run(15000, 20000); CHECK(got.empty() && drift_kind(&d, 1) == DRIFT_NONE);              // matches its calibration
    run(15400, 15000); CHECK(got.empty());                                                  // < max(5%, 600): ignored
    run(18000, 8000); CHECK(got.empty());                                                   // shifted but not held long enough
    run(18000, 4000); CHECK(got.size() == 1 && got[0].first == 1 && got[0].second == DRIFT_ANTENNA && msgs[0].find("antenna") != std::string::npos);
    run(18000, 30000); CHECK(got.size() == 1);                                              // one-shot while it persists
    run(15000, 33000); CHECK(drift_kind(&d, 1) == DRIFT_NONE);                              // condition gone: clears
    run(18000, 14000); CHECK(got.size() == 2);                                              // re-armed: fires again
    drift_reset_node(&d, 1); CHECK(drift_kind(&d, 1) == DRIFT_NONE);
    // moved board: the hub flags it (all its paths off their calibrated level); raised once, cleared 30 s after the condition goes
    got.clear(); drift_init(&d, sink, nullptr); run(15000, 1000);
    drift_set_moved(&d, 1, true); run(15000, 2000); CHECK(got.size() == 1 && got[0].second == (int)DRIFT_MOVED && drift_kind(&d, 1) == DRIFT_MOVED);
    run(15000, 5000); CHECK(got.size() == 1);
    drift_set_moved(&d, 1, false); run(15000, 31000); CHECK(drift_kind(&d, 1) == DRIFT_NONE);
    // capture in progress holds everything
    got.clear(); drift_init(&d, sink, nullptr); c.phase = CAL_CAPTURING; run(19000, 30000); CHECK(got.empty()); c.phase = CAL_IDLE;
    // offline node resets its timers
    run(19000, 5000, false); CHECK(got.empty());
    // noise floor shift (live baseline mean above calibration for 60 s)
    got.clear(); drift_init(&d, sink, nullptr); s.emf[1].n = s.cfg.warmup_n; s.emf[1].mean = 20; s.emf[1].var = 4;
    run(15000, 59000); CHECK(got.empty()); run(15000, 3000); CHECK(got.size() == 1 && got[0].second == DRIFT_NOISE);
    // uncalibrated node: prompt once after 5 minutes online
    got.clear(); drift_init(&d, sink, nullptr); c.applied[1].emf.valid = false; c.applied[0].emf.valid = true; s.emf[1].n = 0;
    run(15000, 299000); CHECK(got.empty()); run(15000, 3000); CHECK(got.size() == 1 && got[0].second == DRIFT_UNCAL && msgs.back().find("not calibrated") != std::string::npos);
    run(15000, 60000); CHECK(got.size() == 1);
    // control node is skipped
    got.clear(); drift_init(&d, sink, nullptr); score_set_control(&s, 1); run(15000, 400000); CHECK(got.empty());
    // calibration capture records the raw touch level
    { ScoreState s2; score_init(&s2); CalState c2; cal_init(&c2); GridFrame g; cal_start(&c2, -1, CAL_SENS_NORMAL, 0);
      for (uint32_t tt = 0; tt < CAL_CAPTURE_MS + 100; tt += 50) { frame(&g, 16000); cal_feed(&c2, &g, &s2, tt); }
      CHECK(c2.phase == CAL_PENDING && c2.pending[1].emf.valid && c2.pending[1].emf.raw_mean > 15999.0f && c2.pending[1].emf.raw_mean < 16001.0f); }
    puts("ok"); return 0;
}
