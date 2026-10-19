#pragma once
#define PIN_EMF   7
#define PIN_BOOT  0
#define PIN_LED2  2             // second onboard LED (silkscreen IO2): mirrors the hub's detection indicator
#define LED2_ACTIVE_HIGH 1
#define LED_PIN_A 48    // DevKitC-1 v1.0; v1.1 uses 38. Both are driven until the board's pin is confirmed (SpectralGrid.md #18).
#define LED_PIN_B 38
#define EMF_WINDOW_SAMPLES 64   // measured ~3.3 us/read => ~210 us per window (bringup, 2026-10-07)
#define SAMPLE_HZ 200
#define TEMP_DIV  20            // temp every 20th window = 10 Hz
#define LOCAL_ALPHA 0.001f      // ~5 s horizon at 200 Hz
#define LOCAL_WARMUP 2000       // 10 s before the local score is trusted
#define LOCAL_EMF_SCALE 50.0f   // TODO(open item #5/#11): placeholder until calibration
#define LOCAL_ALARM_SCORE 40
#define RAW_SATURATED 0x3FFFFF  // first window after boot reads 4194303 (bringup); also treated as a fault
#define PAIR_REQ_MS 500
#define HUBLOST_REQ_MS 1000
#define PAIR_HOLD_MS 3000
#define BATT_PIN  -1            // ADC1 pin on the battery divider; -1 = not wired (vbat reported as 0). Suggested: GPIO 1
#define BATT_DIV  2.0f          // divider ratio (100k/100k)
#define BATT_SAMPLES 16
