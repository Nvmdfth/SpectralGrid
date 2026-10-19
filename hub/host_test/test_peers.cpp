// cd hub/host_test && g++ -std=c++17 -Wall -I../../shared -I../src test_peers.cpp ../src/peers.cpp -o /tmp/t && /tmp/t
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "peers.h"
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)
static void mac(uint8_t *m, int n) { memset(m, 0xA0, 6); m[5] = (uint8_t)n; }
int main() {
    PeerTable t; peers_init(&t, 50);
    uint8_t m[6];
    CHECK(peers_capacity(&t) == 13);
    for (int i = 0; i < 13; i++) { mac(m, i); PeerRec *r = peers_assign(&t, m); CHECK(r && r->slot == i && r->node_id == i + 1); }
    mac(m, 99); CHECK(peers_assign(&t, m) == nullptr);                 // 14th refused at 50 Hz
    mac(m, 3); PeerRec *again = peers_assign(&t, m); CHECK(again && again->node_id == 4 && peers_count(&t) == 13);  // idempotent
    CHECK(peers_set_tx_hz(&t, 25) && t.tx_hz == 25 && peers_capacity(&t) == 16);
    for (int i = 13; i < 16; i++) { mac(m, i); CHECK(peers_assign(&t, m) != nullptr); }
    mac(m, 77); CHECK(peers_assign(&t, m) == nullptr);                 // 16 cap
    CHECK(!peers_set_tx_hz(&t, 50) && t.tx_hz == 25);                  // slots 13..15 do not fit 50 Hz
    CHECK(peers_remove(&t, 14) && peers_remove(&t, 15) && peers_remove(&t, 16));
    CHECK(peers_set_tx_hz(&t, 50) && t.tx_hz == 50);
    CHECK(peers_remove(&t, 5) && !peers_remove(&t, 5));
    mac(m, 55); PeerRec *re = peers_assign(&t, m); CHECK(re && re->slot == 4 && re->node_id == 5);   // lowest free slot reused
    CHECK(peers_find_id(&t, 5) == re && peers_find_mac(&t, m) == re && peers_find_id(&t, 200) == nullptr);
    puts("ok"); return 0;
}
