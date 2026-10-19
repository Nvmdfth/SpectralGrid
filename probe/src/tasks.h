#pragma once
#include <stdint.h>
#include "protocol.h"

// Latest sample handed from Core 0 (sample_task) to Core 1 (beacon_task). Header and seq are filled at TX time.
typedef struct {
    ProbeTelemetry t;
    uint8_t local_score;   // 0..100, probe-local EMF score for the LED
    uint32_t windows;      // windows published since boot
} ProbeSample;

typedef struct {
    bool paired;
    uint8_t hub_mac[6];
    uint8_t node_id, slot, tx_hz;
} PairingState;

typedef enum { LED_PAIRING, LED_BEACON, LED_HUB_LOST, LED_OTA } LedMode;

void mailbox_init();
bool mailbox_read(ProbeSample *out);
void sample_task(void *);   // Core 0: touch + die temp -> mailbox
void beacon_task(void *);   // Core 1: pairing, slotted ESP-NOW TX, LED

void pairing_load(PairingState *ps);
void pairing_save(const PairingState *ps);
void pairing_clear(PairingState *ps);

void led_init();
void led_mode(LedMode m);
void led_tx_pulse();
void led_local_score(uint8_t score);
void led_hub_state(uint8_t score, uint8_t state);   // from HubSync (protocol v3): the probe mirrors the hub's lights while this is fresh
void led_set_identity(uint8_t node_id);               // permanent per-node colour for the idle light
void led_ident(uint8_t color, uint32_t ms);          // solid colour 1-7 (red green blue yellow magenta cyan white) for ms; 0 = off
void led_update(uint32_t now_ms);
