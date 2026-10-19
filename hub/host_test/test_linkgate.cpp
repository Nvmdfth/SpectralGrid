// Host test for shared/linkgate.h.
#include <stdio.h>
#include "linkgate.h"
static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static void run(LinkGate *g, const LinkGateCfg *c, uint32_t *now, bool on, float rssi, float loss, int secs) {
    for (int i = 0; i < secs * 10; i++) { *now += 100; lg_update(g, c, on, rssi, loss, *now); }   // 10 Hz feed
}
int main() {
    LinkGateCfg c = lg_default_cfg(); LinkGate g; lg_reset(&g); uint32_t now = 1000;
    CHECK(c.gate_dbm == -50.0f && c.stable_s == 300.0f);
    run(&g, &c, &now, true, -45, 0, 290);  CHECK(!lg_ready(&g, &c));
    run(&g, &c, &now, true, -45, 0, 15);   CHECK(lg_ready(&g, &c));
    // one second below the gate breaks it
    run(&g, &c, &now, true, -55, 0, 1);    CHECK(!lg_ready(&g, &c) && g.ok_s == 0);
    run(&g, &c, &now, true, -45, 0, 299);  CHECK(!lg_ready(&g, &c));
    run(&g, &c, &now, true, -45, 0, 5);    CHECK(lg_ready(&g, &c));
    // offline breaks it; loss above 2 % breaks it
    run(&g, &c, &now, false, -45, 0, 1);   CHECK(!lg_ready(&g, &c));
    run(&g, &c, &now, true, -45, 3.0f, 5); CHECK(g.ok_s == 0);
    // a wandering link (blocks of 10 s alternating -42 / -48: spread 6 dB > 4) never becomes ready
    lg_reset(&g); now = 1000; int bad = 0;
    for (int blk = 0; blk < 60; blk++) { run(&g, &c, &now, true, blk % 2 ? -42 : -48, 0, 10); if (lg_ready(&g, &c)) bad++; }
    CHECK(bad == 0);
    // a slow wander inside 4 dB is fine
    lg_reset(&g); now = 1000;
    for (int blk = 0; blk < 40; blk++) run(&g, &c, &now, true, blk % 2 ? -44 : -46, 0, 10);
    CHECK(lg_ready(&g, &c) && g.spread <= 4.0f);
    // custom config: gate -40 rejects a -45 link
    LinkGateCfg c2 = c; c2.gate_dbm = -40; lg_reset(&g); now = 1000; run(&g, &c2, &now, true, -45, 0, 400); CHECK(!lg_ready(&g, &c2));
    printf(fails ? "FAILED\n" : "ok\n");
    return fails ? 1 : 0;
}
