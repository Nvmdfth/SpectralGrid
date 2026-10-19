// cd hub/host_test && g++ -std=c++17 -Wall -I../../shared -I../src test_events.cpp ../src/events.cpp -o /tmp/t && /tmp/t
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "events.h"
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

static std::vector<Event> got;
static void sink(const Event *e, void *) { got.push_back(*e); }
static void frame(GridFrame *f, const char *vec, int vid, int score, bool on1 = true, uint16_t vbat1 = 0) {
    memset(f, 0, sizeof(*f)); strcpy(f->vector, vec); f->vec_id = (int8_t)vid; f->score = (uint8_t)score;
    f->n_nodes = 2; f->nodes[0] = {0, 0, 0, 30, 0, true, 0, 0, 0}; f->nodes[1] = {1, 0, 0, 30, 0, on1, -50, 0, vbat1};
}
int main() {
    // ---- log: ordering, wraparound, since() ----
    static EventLog l; evlog_init(&l);
    for (int i = 0; i < 700; i++) { char t[16]; snprintf(t, sizeof(t), "e%d", i); evlog_push(&l, i * 10, 0, EV_SYSTEM, 0, 0, t); }
    CHECK(l.count == EV_LOG_N);
    Event out[600]; int n = evlog_since(&l, 0, out, 600);
    CHECK(n == EV_LOG_N && out[0].seq == 700 - EV_LOG_N + 1 && out[n - 1].seq == 700 && !strcmp(out[n - 1].text, "e699"));
    n = evlog_since(&l, 695, out, 600); CHECK(n == 5 && out[0].seq == 696);
    CHECK(evlog_since(&l, 700, out, 600) == 0 && evlog_since(&l, 0, out, 3) == 3);
    n = evlog_tail(&l, 0, out, 5); CHECK(n == 5 && out[0].seq == 696 && out[4].seq == 700);        // newest 5, oldest first
    n = evlog_tail(&l, 698, out, 5); CHECK(n == 2 && out[0].seq == 699);
    n = evlog_tail(&l, 0, out, 600); CHECK(n == EV_LOG_N && out[0].seq == 700 - EV_LOG_N + 1);
    CHECK(evlog_tail(&l, 700, out, 5) == 0);
    CHECK(!strcmp(ev_type_name(EV_TAG), "tag") && !strcmp(ev_type_name(99), "?"));
    { char big[100]; memset(big, 'x', 99); big[99] = 0; const Event *e = evlog_push(&l, 1, 0, 0, 0, 0, big); CHECK(strlen(e->text) == sizeof(e->text) - 1); }

    // ---- detector ----
    EventDetector d; evdet_init(&d, sink, nullptr); GridFrame f; uint32_t t = 1000;
    auto run = [&](const char *v, int vid, int sc, int k, bool pair = false, int cal = 0, bool on1 = true, uint16_t vb = 0) { for (int i = 0; i < k; i++) { frame(&f, v, vid, sc, on1, vb); evdet_observe(&d, &f, pair, cal, t); t += 50; } };
    run("QUIET", -1, 0, 10);                                                       // first observation: the hub itself is silent, an online probe is announced once
    CHECK(got.size() == 1 && got[0].type == EV_NODE_UP && !strcmp(got[0].text, "node 1 online")); got.clear();
    run("LOCAL_NODE", 1, 40, 2); CHECK(got.empty());                               // flicker shorter than 3 frames: ignored
    run("QUIET", -1, 0, 1); run("LOCAL_NODE", 1, 40, 3);
    CHECK(got.size() == 1 && got[0].type == EV_VECTOR && !strcmp(got[0].text, "LOCAL_NODE node 1") && got[0].id == 1);
    got.clear(); run("LOCAL_NODE", 1, 40, 50); CHECK(got.empty());                  // steady state: silent
    run("GRID_ANOMALY", -1, 85, 3);
    CHECK(got.size() == 2 && got[0].type == EV_ALERT && !strcmp(got[0].text, "alert: score 85") && got[1].type == EV_VECTOR && !strcmp(got[1].text, "GRID_ANOMALY"));
    got.clear(); run("GRID_ANOMALY", -1, 60, 5); CHECK(got.empty());                // between thresholds: hysteresis holds the alert
    run("QUIET", -1, 10, 3); CHECK(got.size() == 2 && got[0].type == EV_ALERT && !strcmp(got[0].text, "alert cleared") && got[1].type == EV_VECTOR);
    // node drop / recover, battery with hysteresis
    got.clear(); run("QUIET", -1, 0, 1, false, 0, false); CHECK(got.size() == 1 && got[0].type == EV_NODE_DOWN && !strcmp(got[0].text, "node 1 lost signal"));
    got.clear(); run("QUIET", -1, 0, 1, false, 0, true, 3200); CHECK(got.size() == 2 && got[0].type == EV_NODE_UP && got[1].type == EV_BATT && strstr(got[1].text, "3.20 V"));
    got.clear(); run("QUIET", -1, 0, 5, false, 0, true, 3350); CHECK(got.empty());   // 3.35 V: above low, below recover: no repeat, no re-arm
    run("QUIET", -1, 0, 1, false, 0, true, 3500); run("QUIET", -1, 0, 1, false, 0, true, 3100); CHECK(got.size() == 1 && got[0].type == EV_BATT);
    // pairing + calibration phases
    got.clear(); run("QUIET", -1, 0, 1, true, 0, true, 3500); run("QUIET", -1, 0, 1, true, 1, true, 3500); run("QUIET", -1, 0, 1, false, 2, true, 3500);
    CHECK(got.size() == 4 && got[0].type == EV_PAIRING && strstr(got[0].text, "open") && got[1].type == EV_CAL && strstr(got[1].text, "started") && got[2].type == EV_PAIRING && got[3].type == EV_CAL && strstr(got[3].text, "review"));
    // hub-radio annotation: a hub-local vector right after a client joined/left
    got.clear(); run("QUIET", -1, 0, 4, false, 2, true, 3500); got.clear();
    evdet_radio_activity(&d, t); run("LOCAL_NODE", 0, 40, 3, false, 2, true, 3500);
    CHECK(got.size() == 1 && strstr(got[0].text, "hub radio?"));
    got.clear(); run("QUIET", -1, 0, 3, false, 2, true, 3500); got.clear(); t += 10000; run("LOCAL_NODE", 0, 40, 3, false, 2, true, 3500);
    CHECK(got.size() == 1 && !strstr(got[0].text, "hub radio?"));                   // activity too long ago: no annotation
    // hub sensor offline at boot then recovering: silent once; a later fault/recovery is reported
    { EventDetector h; evdet_init(&h, sink, nullptr); GridFrame g; got.clear(); uint32_t tt = 0;
      auto hub = [&](bool on) { memset(&g, 0, sizeof(g)); strcpy(g.vector, "QUIET"); g.vec_id = -1; g.n_nodes = 1; g.nodes[0] = {0, 0, 0, 30, 0, on, 0, 0, 0}; evdet_observe(&h, &g, false, 0, tt); tt += 50; };
      hub(false); hub(false); hub(true); hub(true); CHECK(got.empty());
      hub(false); CHECK(got.size() == 1 && !strcmp(got[0].text, "hub sensor fault")); hub(true); CHECK(got.size() == 2 && !strcmp(got[1].text, "hub sensor recovered")); }
    // hub sensor fault event
    got.clear(); frame(&f, "QUIET", -1, 0); f.nodes[0].online = false; evdet_observe(&d, &f, false, 2, t); CHECK(got.size() >= 1);
    // ---- CSV export ----
    {
        static EventLog cl; evlog_init(&cl);
        evlog_push(&cl, 5000, 0, EV_VECTOR, 1, 70, "LOCAL_NODE node 1");
        evlog_push(&cl, 6000, 1700000100, EV_TAG, 255, 0, "hvac: furnace, \"east\" side");
        evlog_push(&cl, 7000, 0, EV_SYSTEM, 255, 0, "line1\nline2");
        static Event snap[8]; int sn = evlog_snapshot(&cl, snap, 8); CHECK(sn == 3);
        for (size_t bs : {1u, 5u, 200u}) {
            EvCsv ec; evcsv_begin(&ec, snap, (uint32_t)sn, true, 1700000000000LL - 5000);
            std::string all; uint8_t ob[300]; size_t g; while ((g = evcsv_fill(&ec, ob, bs)) > 0) all.append((char *)ob, g);
            CHECK(all.rfind("seq,t_ms,wall_utc,type,node,score,text\n", 0) == 0);
            CHECK(all.find("1,5000,2023-11-14T22:13:20Z,vector,1,70,\"LOCAL_NODE node 1\"\n") != std::string::npos);      // wall from offset
            CHECK(all.find("2,6000,2023-11-14T22:15:00Z,tag,,0,\"hvac: furnace, \"\"east\"\" side\"\n") != std::string::npos);   // own epoch wins; quotes escaped; no node
            CHECK(all.find("3,7000,2023-11-14T22:13:22Z,system,,0,\"line1 line2\"\n") != std::string::npos);                 // newline flattened
        }
        EvCsv e2; evcsv_begin(&e2, snap, 1, false, 0); uint8_t o2[200]; size_t g2 = evcsv_fill(&e2, o2, 200); std::string s2((char *)o2, g2);
        CHECK(s2.find("1,5000,,vector,1,70,") != std::string::npos);                    // no clock known: empty wall column
    }
    puts("ok"); return 0;
}
