// Paired-probe table: MAC <-> node_id/slot assignment and capacity rules. Arduino-free, host-tested.
// slot == array index, node_id == slot + 1 (node 0 is the hub).
#pragma once
#include <stdint.h>
#include "protocol.h"

typedef struct { bool used; uint8_t mac[6]; uint8_t node_id, slot; } PeerRec;
typedef struct { PeerRec rec[MESH_MAX_PROBES]; uint8_t tx_hz; } PeerTable;

void peers_init(PeerTable *t, uint8_t tx_hz);
int peers_capacity(const PeerTable *t);          // min(MESH_MAX_PROBES, slots per frame at t->tx_hz)
int peers_count(const PeerTable *t);
PeerRec *peers_find_mac(PeerTable *t, const uint8_t *mac);
PeerRec *peers_find_id(PeerTable *t, uint8_t node_id);
PeerRec *peers_assign(PeerTable *t, const uint8_t *mac);   // existing MAC => same record; NULL when full
bool peers_remove(PeerTable *t, uint8_t node_id);
bool peers_set_tx_hz(PeerTable *t, uint8_t hz);            // false if an occupied slot would not fit the new frame
