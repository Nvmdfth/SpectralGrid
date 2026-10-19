// Sliding-window CSI motion statistic (Arduino-free, host-tested). Per link: 50 frames, a result every 25 frames.
// Each frame's 52 usable LLTF subcarrier amplitudes are divided by that frame's mean amplitude (cancels AGC / TX gain),
// then the standard deviation of every subcarrier over the window is averaged: "temporal std". An empty room reads ~0.025
// on the real boards (SpectralGrid.md section 13 / hub/tools/csi_analyze.py); people moving anywhere in the room raise it.
// Also returns the window's mean RSSI, used for the on-line "shadow" test. UNVERIFIED beyond one room and one placement.
#pragma once
#include <stdint.h>
#include <math.h>

#define CSIW_FRAMES 50
#define CSIW_STEP   25
#define CSIW_SC     52

typedef struct { uint16_t a[CSIW_FRAMES][CSIW_SC]; int8_t rssi[CSIW_FRAMES]; uint32_t n; } CsiWin;   // ~5.3 KB

static inline void csiw_reset(CsiWin *w) { w->n = 0; }

// buf = 128 raw (imag, real) int8 pairs of a 64-point LLTF. Usable subcarriers: 1..26 and 38..63 (as csi_lltf_variance).
// Returns true when a window result is ready (every CSIW_STEP frames once CSIW_FRAMES frames are held).
// amp_out (optional, may be NULL): the frame's 52 raw amplitudes as bytes, amplitude x 4 clamped to 255 (field log "raw-lite").
static inline bool csiw_push_amp(CsiWin *w, const int8_t *buf, int8_t rssi, float *tstd, float *rssi_mean, uint8_t *amp_out) {
    float amp[CSIW_SC], sum = 0; int n = 0;
    for (int k = 1; k < 64; k++) {
        if (k >= 27 && k <= 37) continue;
        float im = buf[2 * k], re = buf[2 * k + 1], a = sqrtf(re * re + im * im);
        amp[n++] = a; sum += a;
    }
    if (amp_out) for (int i = 0; i < n; i++) { float q = amp[i] * 4.0f + 0.5f; amp_out[i] = q > 255.0f ? 255 : (uint8_t)q; }
    float mean = sum / n; if (mean < 1e-6f) mean = 1e-6f;
    uint32_t slot = w->n % CSIW_FRAMES;
    for (int i = 0; i < n; i++) { float v = amp[i] / mean * 4096.0f; w->a[slot][i] = v > 65535.0f ? 65535 : (uint16_t)(v + 0.5f); }
    w->rssi[slot] = rssi; w->n++;
    if (w->n < CSIW_FRAMES || (w->n - CSIW_FRAMES) % CSIW_STEP != 0) return false;
    float tot = 0;
    for (int i = 0; i < n; i++) {
        float m = 0; for (int f = 0; f < CSIW_FRAMES; f++) m += w->a[f][i];
        m /= CSIW_FRAMES;
        float v = 0; for (int f = 0; f < CSIW_FRAMES; f++) { float d = w->a[f][i] - m; v += d * d; }
        tot += sqrtf(v / CSIW_FRAMES);
    }
    float r = 0; for (int f = 0; f < CSIW_FRAMES; f++) r += w->rssi[f];
    *tstd = tot / n / 4096.0f; *rssi_mean = r / CSIW_FRAMES;
    return true;
}

static inline bool csiw_push(CsiWin *w, const int8_t *buf, int8_t rssi, float *tstd, float *rssi_mean) {
    return csiw_push_amp(w, buf, rssi, tstd, rssi_mean, 0);
}
