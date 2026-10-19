// Core 0 acquisition (§6.1): touch windows at 200 Hz, die temp at 10 Hz with a 3 s slope, probe-local EMF score.
// Note: die temperature tracks chip self-heating, not the room (spec §12 #12). Reported anyway for the protocol.
#include <Arduino.h>
#include <math.h>
#include "driver/temperature_sensor.h"
#include "config.h"
#include "tasks.h"
#include "dsp.h"
#include "batt.h"

static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
static ProbeSample latest;
static bool have_sample = false;

void mailbox_init() { memset(&latest, 0, sizeof(latest)); have_sample = false; }

bool mailbox_read(ProbeSample *out) {
    portENTER_CRITICAL(&mux);
    *out = latest;
    bool ok = have_sample;
    portEXIT_CRITICAL(&mux);
    return ok;
}

static int16_t clamp_i16(float v) { return v > 32767.f ? 32767 : (v < -32768.f ? -32768 : (int16_t)lroundf(v)); }

void sample_task(void *) {
    temperature_sensor_handle_t ts = nullptr;
    temperature_sensor_config_t tc = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    bool ts_ok = temperature_sensor_install(&tc, &ts) == ESP_OK && temperature_sensor_enable(ts) == ESP_OK;

    SlopeRing ring;
    slope_ring_init(&ring);
    int16_t temp_centi = 0, slope_centi = 0;
    bool temp_fault = !ts_ok;
    uint16_t vbat_mv = 0;
#if BATT_PIN >= 0
    analogSetPinAttenuation(BATT_PIN, ADC_11db);
#endif

    Baseline base;
    baseline_init(&base, LOCAL_ALPHA);
    uint32_t windows = 0;
    bool first = true;
    TickType_t t = xTaskGetTickCount();

    for (;;) {
        uint32_t lo = 0xFFFFFFFFu, hi = 0, last = 0;
        for (int i = 0; i < EMF_WINDOW_SAMPLES; i++) {
            last = touchRead(PIN_EMF);
            if (last < lo) lo = last;
            if (last > hi) hi = last;
        }
        if (first) {            // first window after boot saturates; discard it
            first = false;
            vTaskDelayUntil(&t, pdMS_TO_TICKS(1000 / SAMPLE_HZ));
            continue;
        }
        windows++;

        if (windows % TEMP_DIV == 0 && ts_ok) {
            float c = NAN;
            if (temperature_sensor_get_celsius(ts, &c) == ESP_OK) {
                slope_centi = clamp_i16(slope_ring_push(&ring, c) * 100.0f);
                temp_centi = clamp_i16(c * 100.0f);
                temp_fault = false;
            } else temp_fault = true;
        }

#if BATT_PIN >= 0
        if (windows % SAMPLE_HZ == 1) {         // 1 Hz
            uint32_t acc = 0;
            for (int i = 0; i < BATT_SAMPLES; i++) acc += analogReadMilliVolts(BATT_PIN);
            vbat_mv = batt_mv_from_pin(acc / BATT_SAMPLES, BATT_DIV);
        }
#endif
        uint32_t p2p32 = hi - lo;
        uint16_t p2p = p2p32 > 0xFFFF ? 0xFFFF : (uint16_t)p2p32;
        uint16_t raw = last > 0xFFFF ? 0xFFFF : (uint16_t)last;
        bool emf_fault = hi >= RAW_SATURATED || hi == 0;

        float score = emf_fault ? 0.f : baseline_score(&base, (float)p2p, 2.5f, LOCAL_EMF_SCALE, LOCAL_WARMUP);
        if (!emf_fault) baseline_update(&base, (float)p2p);

        ProbeSample s;
        memset(&s, 0, sizeof(s));
        s.t.emf_raw = raw;
        s.t.emf_p2p = p2p;
        s.t.temp_centi_c = temp_centi;
        s.t.temp_slope_centi = slope_centi;
        s.t.vbat_mv = vbat_mv;
        s.t.flags = (emf_fault || temp_fault ? FLAG_SENSOR_FAULT : 0) | (score >= LOCAL_ALARM_SCORE ? FLAG_LOCAL_ALARM : 0) | (batt_low(vbat_mv) ? FLAG_LOW_BATT : 0);
        s.local_score = (uint8_t)score;
        s.windows = windows;

        portENTER_CRITICAL(&mux);
        latest = s;
        have_sample = true;
        portEXIT_CRITICAL(&mux);

        vTaskDelayUntil(&t, pdMS_TO_TICKS(1000 / SAMPLE_HZ));
    }
}
