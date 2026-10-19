// WS2812 status LED (§7.2). Runs in its own Core 1 task at 100 Hz: a 10 Hz strobe cannot be driven from the 20 Hz loop.
// Both candidate pins are driven until the board's LED pin is confirmed (DevKitC-1 v1.0 = GPIO 48, v1.1 = GPIO 38).
// Priority: calibrating (cyan breathe) > pairing window open (blue blink) > score meter.
#include <Arduino.h>
#include <FastLED.h>
#include <math.h>
#include "config.h"
#include "hub.h"

#define LED_PIN_A 48
#define LED_PIN_B 38

static CRGB ledA[1], ledB[1];
static volatile uint8_t g_score = 0;
static volatile bool g_pair = false, g_cal = false;
static volatile uint8_t g_pres = 0, g_gest = 0;
static volatile uint32_t g_touch_until = 0;     // EMF-wire touch event: blue/white flashes until this millis()
static bool touch_on(uint32_t now) { return (int32_t)(g_touch_until - now) > 0; }

// IO2 LED: off = clear/not calibrated, solid = motion in the room, 5 Hz blink = something on the hub-probe line, 1 Hz blink = calibrating
static void led2_write(bool on) { digitalWrite(PIN_LED2, (on == (LED2_ACTIVE_HIGH != 0)) ? HIGH : LOW); }
static bool led2_level(uint32_t now) {
    if (g_gest == 1) return (now / 100) & 1;
    if (g_gest >= 2) return true;
    if (g_cal) return (now / 500) & 1;
    if (touch_on(now)) return (now / 125) & 1;          // lit on the white phase
    if (g_pres == 3) return (now / 100) & 1;
    return g_pres == 2;
}

static CRGB color_for(uint32_t now, uint8_t score, bool pair, bool cal) {
    if (g_gest == 1) return ((now / 250) & 1) ? CRGB(0x60, 0x60, 0x60) : CRGB::Black;                      // white blink 2 Hz: walk away
    if (g_gest == 2) return CRGB(0xFF, 0, 0);                                                              // rejected: solid red
    if (g_gest == 3) return ((now / 125) & 1) ? CRGB(0, 0xFF, 0) : CRGB::Black;                            // accepted: green flashes
    if (cal) { uint8_t b = (uint8_t)(24 + 24 * (1.0f + sinf(now / 320.0f))); return CRGB(0, b, b); }       // cyan breathe
    if (touch_on(now)) return ((now / 125) & 1) ? CRGB(0xFF, 0xFF, 0xFF) : CRGB(0, 0, 0xFF);               // EMF-wire touch: alternate white / blue 4 Hz
    if (pair) return ((now / 250) & 1) ? CRGB(0, 0, 0x66) : CRGB::Black;                                    // blue blink 2 Hz
    if (score >= 71) return ((now / 50) & 1) ? CRGB(0xFF, 0, 0) : CRGB::Black;                              // red strobe 10 Hz
    if (score >= 31) { float k = 0.4f + 0.6f * (0.5f + 0.5f * sinf(now / 350.0f)); return CRGB((uint8_t)(0x44 * k), (uint8_t)(0x33 * k), 0); }  // breathing amber
    return CRGB(0, 0x22, 0);                                                                                // dim green
}

static void led_task(void *) {
    for (;;) {
        CRGB c = color_for(millis(), g_score, g_pair, g_cal);
        ledA[0] = c; ledB[0] = c;
        led2_write(led2_level(millis()));
        FastLED.show();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void led_init() {
    pinMode(PIN_LED2, OUTPUT);
    for (int i = 0; i < 3; i++) { led2_write(true); delay(150); led2_write(false); delay(150); }   // boot self-test: shows the IO2 LED works and its polarity
    FastLED.addLeds<WS2812, LED_PIN_A, GRB>(ledA, 1);
    FastLED.addLeds<WS2812, LED_PIN_B, GRB>(ledB, 1);
    FastLED.setBrightness(128);
    xTaskCreatePinnedToCore(led_task, "led", 3072, nullptr, 2, nullptr, 1);
}
void led_show(uint8_t score) { g_score = score; }
void led_presence(uint8_t pres) { g_pres = pres; }
void led_gesture(uint8_t mode) { g_gest = mode; }
void led_touch_pulse() { g_touch_until = millis() + 3000; }
bool led_touch_active() { return touch_on(millis()); }
uint8_t led_gesture_get() { return g_gest; }
void led_state(bool pairing, bool calibrating) { g_pair = pairing; g_cal = calibrating; }
