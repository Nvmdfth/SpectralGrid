#pragma once
#define PIN_EMF   7
#define PIN_LED   48
#define PIN_BOOT  0
#define PIN_LED2  2             // second onboard LED (silkscreen IO2, next to the RGB LED on 48): detection indicator
#define LED2_ACTIVE_HIGH 1      // set 0 if the IO2 LED turns out to be wired active-low (it would then light when idle)
#define EMF_WINDOW_SAMPLES 64   // measured ~3.3 us/read => ~210 us per window (bringup, 2026-10-07)
#define TEMP_DIV  20            // temp every 20th window = 10 Hz
#define RAW_SATURATED 0x3FFFFF  // first window after boot reads 4194303 (bringup); also treated as a fault
#define SAMPLE_HZ 200
#define SCORE_HZ  20
#define AP_SSID   "SPECTRAL_GRID"
#define AP_PASS   nullptr       // open; or "paranormal2026"
#define W_CSI 0.40f
#define W_EMF 0.45f
#define W_T   0.15f
#define MESH_DEFAULT_TX_HZ  PROBE_DEFAULT_TX_HZ
#define PAIR_WINDOW_MS      60000
#define PAIR_HOLD_MS        3000
#define PEER_ONLINE_MS      1000    // spec §5.5
#define PEER_ACK_MIN_MS     400     // rate-limit PairAck replies per probe
#define CSI_STALE_MS        500
#define HUB_BATT_PIN -1         // optional ADC1 pin on a battery divider for the hub itself; -1 = not wired
#define HUB_BATT_DIV 2.0f
#define BATT_SAMPLES 16
