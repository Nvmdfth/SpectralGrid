// Replays a decoded field log's EMF raw level (nodes.csv with emf_raw, labels.csv) through shared/touch.h and lists the events
// next to the label active at that moment. Build: g++ -std=c++17 -Ishared hub/host_test/replay_touch.cpp -o /tmp/replay_touch
// Usage: /tmp/replay_touch DECODED_DIR
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include "touch.h"

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    std::string d = argv[1];
    std::vector<std::pair<uint32_t, std::string>> L;
    { FILE *f = fopen((d + "/labels.csv").c_str(), "r"); char line[256]; fgets(line, sizeof line, f);
      while (fgets(line, sizeof line, f)) { unsigned t; char n[64]; if (sscanf(line, "%u,%63[^\r\n]", &t, n) == 2) L.push_back({t, n}); } fclose(f); }
    TouchDet det[2]; touch_reset(&det[0], 80.0f); touch_reset(&det[1]);   // hub 80 counts, probes 50 (as in main.cpp)
    FILE *f = fopen((d + "/nodes.csv").c_str(), "r"); char line[512]; fgets(line, sizeof line, f);
    int quiet_events = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned t, node, on, p2p, vb; float tc, ls; int rs; char raw[16] = "";
        if (sscanf(line, "%u,%u,%u,%u,%f,%u,%d,%f,%15[^\r\n]", &t, &node, &on, &p2p, &tc, &vb, &rs, &ls, raw) < 9 || !raw[0] || node > 1) continue;
        float r = (float)atoi(raw);
        if (touch_update(&det[node], r, t)) {
            std::string lbl = "(before first label)"; for (auto &l : L) if (l.first <= t) lbl = l.second;
            printf("EVENT t=%6.1fs  %-5s  shift %+8.0f  during '%s'\n", t / 1000.0, node ? "probe" : "hub", det[node].peak, lbl.c_str());
            if (lbl == "emf_quiet") quiet_events++;
        }
    }
    printf("events during 'emf_quiet' (should be 0): %d\n", quiet_events);
    return 0;
}
