// NVS-backed pairing state (§5.4). One probe binary serves every board; identity lives here, not in the build.
#include <Arduino.h>
#include <Preferences.h>
#include <string.h>
#include "tasks.h"

static const char *NS = "spectral";

void pairing_load(PairingState *ps) {
    memset(ps, 0, sizeof(*ps));
    ps->node_id = NODE_ID_UNASSIGNED;
    ps->tx_hz = PROBE_DEFAULT_TX_HZ;
    Preferences p;
    if (!p.begin(NS, false)) return;      // read-write: creates the namespace on first boot (read-only logs an NVS error)
    if (p.getBool("paired", false) && p.getBytesLength("hub_mac") == 6) {
        p.getBytes("hub_mac", ps->hub_mac, 6);
        ps->node_id = p.getUChar("node_id", NODE_ID_UNASSIGNED);
        ps->slot = p.getUChar("slot", 0);
        ps->tx_hz = proto_clamp_tx_hz(p.getUChar("tx_hz", PROBE_DEFAULT_TX_HZ));
        ps->paired = ps->node_id != NODE_ID_UNASSIGNED && ps->node_id != NODE_ID_HUB;
    }
    p.end();
}

void pairing_save(const PairingState *ps) {
    Preferences p;
    if (!p.begin(NS, false)) return;
    p.putBool("paired", ps->paired);
    p.putBytes("hub_mac", ps->hub_mac, 6);
    p.putUChar("node_id", ps->node_id);
    p.putUChar("slot", ps->slot);
    p.putUChar("tx_hz", ps->tx_hz);
    p.end();
}

void pairing_clear(PairingState *ps) {
    Preferences p;
    if (p.begin(NS, false)) { p.clear(); p.end(); }
    memset(ps, 0, sizeof(*ps));
    ps->node_id = NODE_ID_UNASSIGNED;
    ps->tx_hz = PROBE_DEFAULT_TX_HZ;
}
