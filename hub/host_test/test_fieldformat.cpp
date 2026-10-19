// Host test for shared/field_format.h. With an argument it also writes a sample log (used to check hub/tools/field_decode.py).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include "field_format.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static std::vector<uint8_t> log_;
static void put(uint8_t type, const uint8_t *p, size_t n) { uint8_t fr[FL_MAX_PAYLOAD + 4]; size_t k = fl_frame(fr, type, p, n); log_.insert(log_.end(), fr, fr + k); }

int main(int argc, char **argv) {
    uint8_t p[FL_MAX_PAYLOAD];
    // BOOT
    fl_u32(p, 1000); fl_u32(p + 4, 3); memcpy(p + 8, "build test", 10); put(FR_BOOT, p, 18);
    // FEAT: t=2000 node 1 tstd 0.02512 rssi -40.4 state 2 score 55 moving 1
    fl_u32(p, 2000); p[4] = 1; fl_u16(p + 5, 2512); fl_u16(p + 7, (uint16_t)(int16_t)-404); p[9] = 2; p[10] = 55; p[11] = 1; put(FR_FEAT, p, 12);
    // AMP
    fl_u32(p, 2040); p[4] = 1; p[5] = (uint8_t)(int8_t)-40; for (int i = 0; i < 52; i++) p[6 + i] = (uint8_t)(50 + i); put(FR_AMP, p, 58);
    // LABEL, CAL, CLOCK
    fl_u32(p, 3000); memcpy(p + 4, "cross", 5); put(FR_LABEL, p, 9);
    fl_u32(p, 3100); p[4] = 1; fl_f32(p + 5, 0.0298f); fl_f32(p + 9, -40.0f); put(FR_CAL, p, 13);
    fl_u32(p, 3200); fl_i64(p + 4, 1791500000000LL); put(FR_CLOCK, p, 12);
    size_t good_end = log_.size();

    // all records parse back in order
    { size_t off = 0, cnt = 0; FlRec r; bool ok; uint8_t types[8] = {FR_BOOT, FR_FEAT, FR_AMP, FR_LABEL, FR_CAL, FR_CLOCK};
      while (off < log_.size()) { size_t c = fl_parse(log_.data() + off, log_.size() - off, &r, &ok); if (!ok) break; CHECK(r.type == types[cnt]); off += c; cnt++; }
      CHECK(cnt == 6 && off == log_.size()); }

    // garbage in the middle (power loss / flash fault): the next valid record is found, the garbage is skipped
    { std::vector<uint8_t> b(log_.begin(), log_.begin() + 22);
      b.push_back(0xA5); b.push_back(0x02); b.push_back(0x05); b.push_back(0x11); b.push_back(0x22);          // truncated record then junk
      for (int i = 0; i < 10; i++) b.push_back((uint8_t)(0x30 + i));
      b.insert(b.end(), log_.begin() + 22, log_.end());
      size_t off = 0, cnt = 0; FlRec r; bool ok;
      while (off < b.size()) { size_t c = fl_parse(b.data() + off, b.size() - off, &r, &ok); if (!ok) break; off += c; cnt++; }
      CHECK(cnt == 6); }

    // a corrupted payload byte is rejected (CRC), later records survive
    { std::vector<uint8_t> b = log_; b[30] ^= 0x55;
      size_t off = 0, cnt = 0; FlRec r; bool ok;
      while (off < b.size()) { size_t c = fl_parse(b.data() + off, b.size() - off, &r, &ok); if (!ok) { if (!c) break; off += c; continue; } off += c; cnt++; }
      CHECK(cnt >= 4 && cnt < 6); }

    // a record cut off at the end waits for more data instead of being consumed
    { FlRec r; bool ok; size_t c = fl_parse(log_.data() + good_end - 16, 10, &r, &ok); CHECK(!ok && c == 0); }   // the CLOCK record, first 10 of its 16 bytes

    // too long payload refused
    { uint8_t fr[FL_MAX_PAYLOAD + 8]; CHECK(fl_frame(fr, FR_EVENT, p, FL_MAX_PAYLOAD + 1) == 0); }

    if (argc > 1) { FILE *f = fopen(argv[1], "wb"); fwrite(log_.data(), 1, log_.size(), f); fclose(f); }
    printf(fails ? "FAILED\n" : "ok\n");
    return fails ? 1 : 0;
}
