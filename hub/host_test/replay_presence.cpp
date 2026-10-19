// Replays a labelled capture (hub/tools/csi_capture.py output) through the SAME C++ the firmware runs (csi_window.h +
// presence.h) and prints what the detector said per labelled activity. Not part of run_all.sh (needs a capture file).
// Build: g++ -std=c++17 -O2 -Ishared -Ihub/src hub/host_test/replay_presence.cpp -o /tmp/replay
// Usage: /tmp/replay CAL.csv TEST.csv [TEST.csv ...]   (first 15 s of the first 'empty' segment of CAL.csv calibrate;
//        the last segment of each file is dropped, trim: first 5 s and last 2 s of every segment)
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>
#include <map>
#include "csi_window.h"
#include "presence.h"

struct Frame { uint32_t t; int8_t rssi; int8_t buf[128]; };
struct Lbl { uint32_t t; std::string name; };
static bool load(const char *path, std::vector<Frame> &F, std::vector<Lbl> &L) {
    FILE *fp = fopen(path, "r"); if (!fp) return false;
    static char line[1024];
    while (fgets(line, sizeof line, fp)) {
        if (!strncmp(line, "LBL,", 4)) { unsigned t; char n[40]; if (sscanf(line, "LBL,%u,%39[^\n\r]", &t, n) == 2) L.push_back({t, n}); }
        else if (!strncmp(line, "CSI,", 4)) {
            unsigned t; int node, rssi, noise, sig, rate; char mac[16]; int off = 0;
            if (sscanf(line, "CSI,%u,%d,%12[0-9a-f],%d,%d,%d,%d,%n", &t, &node, mac, &rssi, &noise, &sig, &rate, &off) < 7 || node != 1) continue;
            Frame f; f.t = t; f.rssi = (int8_t)rssi; bool ok = true;
            for (int i = 0; i < 128; i++) { unsigned v; if (sscanf(line + off + 2 * i, "%2x", &v) != 1) { ok = false; break; } f.buf[i] = (int8_t)v; }
            if (ok) F.push_back(f);
        }
    }
    fclose(fp); return true;
}
struct Win { uint32_t t; float tstd, rssi; uint32_t seq; };
static std::vector<Win> windows(const std::vector<Frame> &F) {
    static CsiWin w; csiw_reset(&w); std::vector<Win> out; uint32_t seq = 0;
    for (auto &f : F) { float t, r; if (csiw_push(&w, f.buf, f.rssi, &t, &r)) out.push_back({f.t, t, r, ++seq}); }
    return out;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s CAL.csv TEST.csv...\n", argv[0]); return 2; }
    std::vector<Frame> F; std::vector<Lbl> L; load(argv[1], F, L);
    auto W = windows(F);
    uint32_t e0 = 0; for (auto &l : L) if (l.name == "empty") { e0 = l.t; break; }
    PresCalAcc acc; pres_cal_reset(&acc);
    for (auto &w : W) if (w.t >= e0 + 5000 && w.t < e0 + 20000) pres_cal_add(&acc, w.tstd, w.rssi, w.seq);
    PresCalResult cr = pres_cal_result(&acc, PRES_G_NORMAL);
    printf("calibration: valid=%d n=%u tmean=%.4f tmax=%.4f -> thr=%.4f rssi_base=%.2f warn=%d\n", cr.valid, cr.n, cr.tmean, cr.tmax, cr.thr, cr.rssi_base, cr.warn);
    if (!cr.valid) return 1;
    const char *names[] = {"OFF", "CLEAR", "MOTION", "ON_LINE"};
    struct Seg { std::string name; int w = 0, act = 0, onsets = 0; bool prev = false; std::vector<int> at; }; std::map<std::string, Seg> segs;   // per file+segment, for the false-alarm report
    std::map<std::string, int> cnt[4]; std::map<std::string, int> tot; std::map<std::string, int> mov;
    for (int a = 2; a < argc; a++) {
        std::vector<Frame> TF; std::vector<Lbl> TL; if (!load(argv[a], TF, TL) || TL.size() < 2) continue;
        auto TW = windows(TF); PresLink p; pres_link_set_cal(&p, cr.q0, cr.thr, cr.rssi_base);
        for (auto &w : TW) {
            pres_link_update(&p, w.tstd, w.rssi, w.seq, w.t);
            for (size_t i = 0; i + 1 < TL.size(); i++) {            // last segment (no closing label) is dropped
                uint32_t s = TL[i].t + (TL[i].name == "empty" ? 15000 : 5000), e = TL[i + 1].t - 2000;   // 'empty': you are still walking out of the room for the first seconds
                if (a == 2 && argv[a] == std::string(argv[1]) && TL[i].name == "empty" && TL[i].t == e0 && w.t < e0 + 20000 + 1000) continue;   // calibration window
                if (w.t >= s && w.t < e) { const std::string &n = TL[i].name; { Seg &g = segs[std::string(argv[a]) + "#" + std::to_string(i) + " " + n]; g.name = n; g.w++; bool act = p.state > PRES_CLEAR; if (act) g.act++; if (act && !g.prev) { g.onsets++; g.at.push_back((int)((w.t - TL[i].t) / 1000)); } g.prev = act; } tot[n]++; cnt[p.state][n]++; if (p.state == PRES_ON_LINE && p.moving) mov[n]++; }
            }
        }
    }
    printf("%-10s %8s", "activity", "windows"); for (int s = 1; s < 4; s++) printf("%10s", names[s]); printf("   (on-line & moving)\n");
    for (auto &kv : tot) { printf("%-10s %8d", kv.first.c_str(), kv.second); for (int s = 1; s < 4; s++) printf("%9.0f%%", 100.0 * cnt[s][kv.first] / kv.second); printf("   %5.0f%%\n", 100.0 * mov[kv.first] / kv.second); }
    printf("\nempty segments (false alarms): windows, % non-clear, onsets, onsets per minute\n");
    for (auto &kv : segs) if (kv.second.name == "empty") { printf("  %-34s %4d  %5.1f%%  %2d  %.1f/min  onsets at (s into segment):", kv.first.c_str(), kv.second.w, 100.0 * kv.second.act / kv.second.w, kv.second.onsets, kv.second.onsets / (kv.second.w * 0.5 / 60.0)); for (int t : kv.second.at) printf(" %d", t); printf("\n"); }
    return 0;
}
