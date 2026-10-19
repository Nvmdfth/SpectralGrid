// Arduino-free DSP helpers. See SpectralGrid.md §8.1.
#pragma once
#include <stdint.h>
#include <math.h>

// EMA mean + variance baseline.
typedef struct {
    float mean, var, alpha;
    uint32_t n;
} Baseline;

static inline void baseline_init(Baseline *b, float alpha) { b->mean = 0; b->var = 0; b->alpha = alpha; b->n = 0; }

static inline void baseline_update(Baseline *b, float x) {
    if (b->n == 0) { b->mean = x; b->var = 0; }
    else {
        float d = x - b->mean;
        b->mean += b->alpha * d;
        b->var = (1.0f - b->alpha) * (b->var + b->alpha * d * d);
    }
    b->n++;
}

// 0..100 score for positive deviation beyond k*sigma; `scale` = deviation mapping to 100.
static inline float baseline_score(const Baseline *b, float x, float k_sigma, float scale, uint32_t warmup_n) {
    if (b->n < warmup_n) return 0;
    float over = x - b->mean - k_sigma * sqrtf(b->var);
    if (over <= 0) return 0;
    float s = 100.0f * over / scale;
    return s > 100.0f ? 100.0f : s;
}

static inline uint16_t p2p_u16(const uint16_t *v, int n) {
    uint16_t lo = v[0], hi = v[0];
    for (int i = 1; i < n; i++) { if (v[i] < lo) lo = v[i]; if (v[i] > hi) hi = v[i]; }
    return (uint16_t)(hi - lo);
}

// Variance of subcarrier amplitudes from interleaved int8 (Q,I) pairs, k in [k_lo..k_hi].
static inline float csi_amp_variance(const int8_t *buf, int k_lo, int k_hi) {
    float sum = 0, sq = 0; int n = 0;
    for (int k = k_lo; k <= k_hi; k++) {
        float q = buf[2 * k], i = buf[2 * k + 1];
        float a = sqrtf(i * i + q * q);
        sum += a; sq += a * a; n++;
    }
    if (n == 0) return 0;
    float mu = sum / n;
    return sq / n - mu * mu;
}

// Least-squares slope (units/sample) over n evenly spaced samples.
static inline float slope_ls(const float *y, int n) {
    if (n < 2) return 0;
    float sx = 0, sy = 0, sxy = 0, sxx = 0;
    for (int i = 0; i < n; i++) { sx += i; sy += y[i]; sxy += i * y[i]; sxx += (float)i * i; }
    float den = n * sxx - sx * sx;
    return den == 0 ? 0 : (n * sxy - sx * sy) / den;
}

// Variance of subcarrier amplitudes over the usable part of a 64-point LLTF buffer (128 bytes, (imag, real) int8 pairs).
// Usable: indices 1..26 and 38..63 (+/-1..26). Skipped: DC (0) and the guard band (27..37).
// UNVERIFIED subcarrier ordering on S3 hardware (SpectralGrid.md §12): validate against live frames. Spec's original
// k in [6..58] included guard carriers; those are near-constant so they only offset the variance.
static inline float csi_lltf_variance(const int8_t *buf) {
    float sum = 0, sq = 0; int n = 0;
    for (int k = 1; k < 64; k++) {
        if (k >= 27 && k <= 37) continue;
        float im = buf[2 * k], re = buf[2 * k + 1];
        float a = sqrtf(re * re + im * im);
        sum += a; sq += a * a; n++;
    }
    float mu = sum / n;
    float v = sq / n - mu * mu;
    return v < 0 ? 0 : v;
}

// Rolling temperature change: push() returns (new - oldest in ring). With one push per 100 ms and 30 slots the oldest
// sample is 3.0 s old, matching the spec's "-0.25 degC over 3 s" thermal rule. First push returns 0.
#define SLOPE_RING_N 30
typedef struct { float v[SLOPE_RING_N]; int n, i; } SlopeRing;
static inline void slope_ring_init(SlopeRing *r) { r->n = 0; r->i = 0; }
static inline float slope_ring_push(SlopeRing *r, float x) {
    float oldest = r->n == 0 ? x : (r->n == SLOPE_RING_N ? r->v[r->i] : r->v[0]);
    r->v[r->i] = x;
    r->i = (r->i + 1) % SLOPE_RING_N;
    if (r->n < SLOPE_RING_N) r->n++;
    return x - oldest;
}
