#include <string.h>
#include "peers.h"

void peers_init(PeerTable *t, uint8_t tx_hz) { memset(t, 0, sizeof(*t)); t->tx_hz = proto_clamp_tx_hz(tx_hz); }

int peers_capacity(const PeerTable *t) {
    int s = (int)proto_slots_per_frame(t->tx_hz);
    return s < MESH_MAX_PROBES ? s : MESH_MAX_PROBES;
}

int peers_count(const PeerTable *t) {
    int n = 0;
    for (int i = 0; i < MESH_MAX_PROBES; i++) n += t->rec[i].used;
    return n;
}

PeerRec *peers_find_mac(PeerTable *t, const uint8_t *mac) {
    for (int i = 0; i < MESH_MAX_PROBES; i++)
        if (t->rec[i].used && memcmp(t->rec[i].mac, mac, 6) == 0) return &t->rec[i];
    return nullptr;
}

PeerRec *peers_find_id(PeerTable *t, uint8_t node_id) {
    for (int i = 0; i < MESH_MAX_PROBES; i++)
        if (t->rec[i].used && t->rec[i].node_id == node_id) return &t->rec[i];
    return nullptr;
}

PeerRec *peers_assign(PeerTable *t, const uint8_t *mac) {
    PeerRec *r = peers_find_mac(t, mac);
    if (r) return r;
    int cap = peers_capacity(t);
    for (int i = 0; i < cap; i++) {
        if (!t->rec[i].used) {
            t->rec[i].used = true;
            memcpy(t->rec[i].mac, mac, 6);
            t->rec[i].slot = (uint8_t)i;
            t->rec[i].node_id = (uint8_t)(i + 1);
            return &t->rec[i];
        }
    }
    return nullptr;
}

bool peers_remove(PeerTable *t, uint8_t node_id) {
    PeerRec *r = peers_find_id(t, node_id);
    if (!r) return false;
    memset(r, 0, sizeof(*r));
    return true;
}

bool peers_set_tx_hz(PeerTable *t, uint8_t hz) {
    hz = proto_clamp_tx_hz(hz);
    uint32_t slots = proto_slots_per_frame(hz);
    for (int i = 0; i < MESH_MAX_PROBES; i++)
        if (t->rec[i].used && t->rec[i].slot >= slots) return false;
    t->tx_hz = hz;
    return true;
}
