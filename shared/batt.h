// Battery helpers (SpectralGrid.md §6, item "probe battery voltage"). Arduino-free, host-tested.
// Wiring assumption: a 1S LiPo/18650 through a divider (default 100k/100k, ratio 2.0) into an ADC1 pin. 4.2 V -> 2.1 V,
// inside the ESP32-S3 ADC range at 11 dB attenuation. The percentage is a coarse linear map, not a fuel gauge.
#pragma once
#include <stdint.h>

#define BATT_EMPTY_MV 3300
#define BATT_FULL_MV  4200

static inline uint16_t batt_mv_from_pin(uint32_t pin_mv, float divider) {
    float v = pin_mv * divider + 0.5f;
    return v > 65535.0f ? 65535 : (uint16_t)v;
}
static inline uint8_t batt_pct(uint16_t mv) {
    if (mv == 0) return 0;
    if (mv <= BATT_EMPTY_MV) return 0;
    if (mv >= BATT_FULL_MV) return 100;
    return (uint8_t)((mv - BATT_EMPTY_MV) * 100u / (BATT_FULL_MV - BATT_EMPTY_MV));
}
static inline bool batt_low(uint16_t mv) { return mv != 0 && mv < BATT_EMPTY_MV; }
