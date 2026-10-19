// cd hub/host_test && g++ -std=c++17 -Wall -I../../shared -I../src test_session.cpp ../src/session.cpp ../src/ringlog.cpp -o /tmp/t && /tmp/t
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <string>
#include <vector>
#include <ctime>
#include "session.h"
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

static void mk(GridFrame *f, int k, int nodes) {
    memset(f, 0, sizeof(*f));
    f->score = (uint8_t)(k % 101); strcpy(f->vector, k % 3 ? "LOCAL_NODE" : "QUIET"); f->vec_id = (int8_t)(k % 3 ? 1 : -1);
    f->n_nodes = (uint8_t)nodes; f->n_links = (uint8_t)(nodes > 1 ? nodes - 1 : 0);
    for (int i = 0; i < nodes; i++) f->nodes[i] = {(uint8_t)i, (uint16_t)(15000 + i), (uint16_t)(k + i), 30.0f + i * 0.25f, i == 1 ? -1.5f : 0.f, (bool)(i != 2), (int8_t)(-40 - i), (float)(i), (uint16_t)(i == 1 ? 3900 : 0)};
    for (int i = 1; i < nodes; i++) f->links[i - 1] = {(uint8_t)i, 10.0f + i, 0, -50};
}

int main() {
    GridFrame f; uint8_t rec[SESS_MAX_REC]; SessFrame d;
    mk(&f, 7, 4);
    uint16_t n = sess_encode(&f, 123456, rec, sizeof(rec));
    CHECK(n == 9 + 4 * 14 + 3 * 3);
    CHECK(sess_decode(rec, n, &d) && d.t_ms == 123456 && d.score == 7 && d.vec_id == 1 && sess_vec_code("LOCAL_NODE") == d.vec);
    CHECK(d.n_nodes == 4 && d.nodes[1].d_temp_c100 == -150 && d.nodes[1].vbat_mv == 3900 && d.nodes[2].online == 0 && d.nodes[3].rssi == -43);
    CHECK(d.n_links == 3 && d.links[1].id == 2 && d.links[1].csi100 == 1200);
    CHECK(sess_encode(&f, 1, rec, 20) == 0 && !sess_decode(rec, 5, &d));
    CHECK(sess_vec_code("nope") == 255 && !strcmp(sess_vec_name(1), "LOCAL_NODE") && !strcmp(sess_vec_name(99), "UNKNOWN"));
    mk(&f, 1, SESS_MAX_NODES); CHECK(sess_encode(&f, 1, rec, sizeof(rec)) == SESS_MAX_REC);

    // CSV: 300 frames of 3 nodes into a small ring so it wraps; stream with awkward buffer sizes
    static uint8_t buf[6000]; RingLog r; ringlog_init(&r, buf, sizeof(buf));
    for (int k = 0; k < 300; k++) { mk(&f, k, 3); n = sess_encode(&f, 1000 + k * 500, rec, sizeof(rec)); CHECK(ringlog_push(&r, rec, n)); }
    CHECK(r.dropped > 0 && r.count < 300);
    for (size_t bufsz : {1u, 7u, 64u, 1000u}) {
        CsvStream cs; csv_begin(&cs, &r, true, 1700000000000LL - 1000);
        std::string all; uint8_t out[1000]; size_t got;
        while ((got = csv_fill(&cs, out, bufsz)) > 0) all.append((char *)out, got);
        std::vector<std::string> lines; size_t p = 0;
        while (p < all.size()) { size_t q = all.find('\n', p); lines.push_back(all.substr(p, q - p)); p = q + 1; }
        CHECK(lines[0].rfind("t_ms,wall_utc", 0) == 0);
        CHECK(lines.size() == 1 + (size_t)r.count * 3);                                   // header + one row per node per frame, no repeats
        { uint32_t t0 = (uint32_t)atoll(lines[1].c_str()); char want[40]; time_t ss = (time_t)((1700000000000LL - 1000 + t0) / 1000); struct tm tm_; gmtime_r(&ss, &tm_);
          snprintf(want, sizeof(want), "%04d-%02d-%02dT%02d:%02d:%02d.", tm_.tm_year + 1900, tm_.tm_mon + 1, tm_.tm_mday, tm_.tm_hour, tm_.tm_min, tm_.tm_sec);
          CHECK(lines[1].find(want) != std::string::npos); }                              // wall clock = t_ms + offset
        uint32_t last = 0; for (size_t i = 1; i < lines.size(); i += 3) { uint32_t t = (uint32_t)atoll(lines[i].c_str()); CHECK(t >= last); last = t; }
        CHECK(lines[2].find(",1,1,15001,") != std::string::npos && lines[3].find(",2,0,15002,") != std::string::npos);   // node 2 offline row
    }
    // epoch unknown => empty wall column; empty ring => header only
    CsvStream c2; csv_begin(&c2, &r, false, 0); uint8_t o2[400]; size_t g2 = csv_fill(&c2, o2, 400); CHECK(g2 > 0 && !memcmp(o2, "t_ms,", 5));
    std::string s2((char *)o2, g2); CHECK(s2.find(",,") != std::string::npos);
    RingLog e; uint8_t eb[100]; ringlog_init(&e, eb, 100); CsvStream c3; csv_begin(&c3, &e, false, 0);
    std::string s3; uint8_t o3[500]; size_t g3; while ((g3 = csv_fill(&c3, o3, 500)) > 0) s3.append((char *)o3, g3); CHECK(s3.size() > 20 && std::count(s3.begin(), s3.end(), '\n') == 1);
    // seek
    RingIter it; CHECK(sess_seek(&r, &it, 1000 + 299 * 500) && sess_seek(&r, &it, 0) && !sess_seek(&r, &it, 4000000000u));
    { RingIter s2; uint8_t rb[SESS_MAX_REC]; uint16_t ln; uint32_t T = 1000 + 290 * 500;      // lands exactly on the first record at or after T, also on a wrapped ring
      CHECK(sess_seek(&r, &s2, T - 100) && ringlog_next(&r, &s2, rb, sizeof(rb), &ln) && sess_time(rb) == T);
      CHECK(sess_seek(&r, &s2, T) && ringlog_next(&r, &s2, rb, sizeof(rb), &ln) && sess_time(rb) == T);
      CHECK(sess_seek(&r, &s2, T + 1) && ringlog_next(&r, &s2, rb, sizeof(rb), &ln) && sess_time(rb) == T + 500); }
    // ring drops records WHILE streaming: rows are not repeated and time stays monotonic
    CsvStream c4; csv_begin(&c4, &r, false, 0); std::string s4; uint8_t o4[300]; size_t g4; int round = 0; uint32_t kk = 300;
    while ((g4 = csv_fill(&c4, o4, 150)) > 0) { s4.append((char *)o4, g4); if (round++ % 3 == 0) { mk(&f, kk, 3); n = sess_encode(&f, 1000 + kk * 500, rec, sizeof(rec)); ringlog_push(&r, rec, n); kk++; } if (round > 4000) break; }
    uint32_t prev = 0; bool mono = true; size_t p4 = s4.find('\n') + 1;
    while (p4 < s4.size()) { uint32_t t = (uint32_t)atoll(s4.c_str() + p4); if (t < prev) mono = false; prev = t; p4 = s4.find('\n', p4) + 1; }
    CHECK(mono);
    puts("ok"); return 0;
}
