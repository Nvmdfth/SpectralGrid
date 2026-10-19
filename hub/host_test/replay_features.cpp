// Feeds a decoded field log's detector windows (features.csv: tstd, rssi_mean) through shared/presence.h from a chosen start time, with the
// calibration given on the command line, and prints the state per minute plus every baseline re-learn. Used on the 2026-10-08 anomaly log.
// Build: g++ -std=c++17 -Ishared hub/host_test/replay_features.cpp -o /tmp/replay_features
// Usage: /tmp/replay_features DECODED_DIR START_S Q0 THR0 R0 [labels]   (with "labels": also a per-label table from labels.csv, 5 s / 2 s trims, 15 s for empty)
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <vector>
#include <map>
#include "presence.h"
int main(int argc, char **argv) {
    if (argc < 6) return 2;
    std::string f = std::string(argv[1]) + "/features.csv"; double t0 = atof(argv[2]);
    PresLink p; pres_link_set_cal(&p, (float)atof(argv[3]), (float)atof(argv[4]), (float)atof(argv[5]));
    FILE *fp = fopen(f.c_str(), "r"); char line[256]; fgets(line, sizeof line, fp);
    std::vector<std::pair<double, std::string>> L;
    if (argc > 6) { FILE *lf = fopen((std::string(argv[1]) + "/labels.csv").c_str(), "r"); char ln[200]; fgets(ln, sizeof ln, lf); while (fgets(ln, sizeof ln, lf)) { unsigned tt; char nm[64]; if (sscanf(ln, "%u,%63[^\r\n]", &tt, nm) == 2) L.push_back({tt / 1000.0, nm}); } fclose(lf); }
    std::map<std::string, std::vector<int>> tab; std::map<std::string, int> tot;
    uint32_t seq = 0; double minute = -1; int cnt[4] = {0}, n = 0; uint16_t seen = 0;
    while (fgets(line, sizeof line, fp)) {
        unsigned t, node, st, sc, mv; float ts, rs;
        if (sscanf(line, "%u,%u,%f,%f,%u,%u,%u", &t, &node, &ts, &rs, &st, &sc, &mv) < 7 || node != 1 || t / 1000.0 < t0) continue;
        pres_link_update(&p, ts, rs, ++seq, t);
        double m = (t / 1000.0 - t0) / 60.0; if (m >= 0 && (int)m != (int)minute) { if (n) printf("min %2d: CLEAR %3.0f%% MOTION %3.0f%% ON_LINE %3.0f%%  baseline %.1f dB  thr %.4f\n", (int)minute, 100.0 * cnt[1] / n, 100.0 * cnt[2] / n, 100.0 * cnt[3] / n, p.rssi_base, p.thr); cnt[1] = cnt[2] = cnt[3] = n = 0; minute = m; }
        cnt[p.state]++; n++;
        for (size_t i = 0; i + 1 <= L.size(); i++) { double a = L[i].first + (L[i].second == "empty" ? 15 : 5), b = (i + 1 < L.size() ? L[i + 1].first : 1e12) - 2; double ts_ = t / 1000.0; if (ts_ >= a && ts_ < b) { auto &v = tab[L[i].second]; if (v.empty()) v.assign(4, 0); v[p.state]++; tot[L[i].second]++; } }
        if (p.relearn_n != seen) { seen = p.relearn_n; printf("   t=%.0fs: baseline re-learned %.1f -> %.1f dB\n", t / 1000.0, p.relearn_from, p.relearn_to); }
    }
    if (!L.empty()) { printf("\nper activity (new logic): CLEAR / MOTION / ON_LINE\n"); for (auto &kv : tab) printf("  %-9s %4d win  %3.0f%% %3.0f%% %3.0f%%\n", kv.first.c_str(), tot[kv.first], 100.0 * kv.second[1] / tot[kv.first], 100.0 * kv.second[2] / tot[kv.first], 100.0 * kv.second[3] / tot[kv.first]); }
    printf("last minute: CLEAR %3.0f%% MOTION %3.0f%% ON_LINE %3.0f%%  baseline %.1f dB\n", 100.0 * cnt[1] / (n ? n : 1), 100.0 * cnt[2] / (n ? n : 1), 100.0 * cnt[3] / (n ? n : 1), p.rssi_base);
    return 0;
}
