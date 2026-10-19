// Core 0: the hub's own sensing (SpectralGrid.md §7.1). Same method as the probe: 200 Hz touch windows, 10 Hz die temp,
// 3 s thermal change. Publishes a NodeSample with node_id 0 for the aggregator on Core 1.
// Note: die temperature tracks chip self-heating, not the room (spec §12 #12).
#include <Arduino.h>
#include <math.h>
#include "driver/temperature_sensor.h"
#include "config.h"
#include "hub.h"
#include "dsp.h"
#include "batt.h"

static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
static NodeSample latest;
static bool have_sample = false;
static volatile uint32_t windows_done = 0;

uint32_t acquisition_windows() { return windows_done; }

bool acquisition_get(NodeSample *out) {
    portENTER_CRITICAL(&mux);
    *out = latest;
    bool ok = have_sample;
    portEXIT_CRITICAL(&mux);
    return ok;
}

static void acquisition_task(void *) {
    temperature_sensor_handle_t ts = nullptr;
    temperature_sensor_config_t tc = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    bool ts_ok = temperature_sensor_install(&tc, &ts) == ESP_OK && temperature_sensor_enable(ts) == ESP_OK;
    if (!ts_ok) Serial.println("acquisition: temperature sensor init FAILED");

    SlopeRing ring;
    slope_ring_init(&ring);
    float temp_c = 0, d_temp = 0;
    bool temp_ok = false;
    uint16_t vbat_mv = 0;
#if HUB_BATT_PIN >= 0
    analogSetPinAttenuation(HUB_BATT_PIN, ADC_11db);
#endif
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
        windows_done = windows;

        if (windows % TEMP_DIV == 0 && ts_ok) {
            float c = NAN;
            if (temperature_sensor_get_celsius(ts, &c) == ESP_OK) { temp_c = c; d_temp = slope_ring_push(&ring, c); temp_ok = true; }
            else temp_ok = false;
        }

#if HUB_BATT_PIN >= 0
        if (windows % SAMPLE_HZ == 1) {         // 1 Hz
            uint32_t acc = 0;
            for (int i = 0; i < BATT_SAMPLES; i++) acc += analogReadMilliVolts(HUB_BATT_PIN);
            vbat_mv = batt_mv_from_pin(acc / BATT_SAMPLES, HUB_BATT_DIV);
        }
#endif
        uint32_t p2p32 = hi - lo;
        NodeSample s;
        memset(&s, 0, sizeof(s));
        s.node_id = NODE_ID_HUB;
        s.emf_p2p = p2p32 > 0xFFFF ? 0xFFFF : (uint16_t)p2p32;
        s.emf_raw = last > 0xFFFF ? 0xFFFF : (uint16_t)last;
        s.temp_c = temp_c;
        s.d_temp = d_temp;
        s.vbat_mv = vbat_mv;
        // A saturated/dead touch read or a failed temp sensor takes node 0 out of scoring rather than feeding it garbage.
        s.online = !(hi >= RAW_SATURATED || hi == 0) && temp_ok;

        portENTER_CRITICAL(&mux);
        latest = s;
        have_sample = true;
        portEXIT_CRITICAL(&mux);

        vTaskDelayUntil(&t, pdMS_TO_TICKS(1000 / SAMPLE_HZ));
    }
}

void acquisition_start() {
    memset(&latest, 0, sizeof(latest));
    xTaskCreatePinnedToCore(acquisition_task, "acq", 4096, nullptr, 5, nullptr, 0);   // Core 0: deterministic acquisition
}
