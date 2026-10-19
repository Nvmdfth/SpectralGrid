// WS2812 status (§6.2). Both candidate pins are driven until the board's LED pin is confirmed (DevKitC-1 v1.0=48, v1.1=38).
#include <Arduino.h>
#include <FastLED.h>
#include <math.h>
#include "config.h"
#include "tasks.h"

static CRGB ledA[1], ledB[1];
static LedMode mode = LED_PAIRING;
static uint8_t score = 0;
static uint32_t pulse_until_ms = 0, last_show_ms = 0;
static volatile uint8_t hub_score = 0, hub_state = 0; static volatile uint32_t hub_state_ms = 0;   // last HubSync light state (millis)
#define HUB_LIGHT_FRESH_MS 3000

static void led2_write(bool on) { digitalWrite(PIN_LED2, (on == (LED2_ACTIVE_HIGH != 0)) ? HIGH : LOW); }
static bool hub_fresh(uint32_t now) { return hub_state_ms && now - hub_state_ms < HUB_LIGHT_FRESH_MS; }
// IO2 as on the hub: off = clear, solid = motion, 5 Hz blink = on the link, 1 Hz blink = calibrating, 5 Hz blink during the countdown, solid = result
static bool led2_level(uint32_t now) {
    if (!hub_fresh(now)) return false;
    uint8_t st = hub_state, gest = (st >> 3) & 3, pres = st & 3;
    if (gest == 1) return (now / 100) & 1;
    if (gest >= 2) return true;
    if (st & 4) return (now / 500) & 1;
    if (st & 64) return (now / 125) & 1;               // EMF-wire touch: lit on the white phase
    if (pres == 3) return (now / 100) & 1;
    return pres == 2;
}

void led_init() {
    FastLED.addLeds<WS2812, LED_PIN_A, GRB>(ledA, 1);
    FastLED.addLeds<WS2812, LED_PIN_B, GRB>(ledB, 1);
    FastLED.setBrightness(40);
    pinMode(PIN_LED2, OUTPUT);
    for (int i = 0; i < 3; i++) { led2_write(true); delay(150); led2_write(false); delay(150); }   // boot self-test of the IO2 LED
}
static const CRGB PAL[8] = { CRGB::Black, CRGB(255, 0, 0), CRGB(0, 255, 0), CRGB(0, 0, 255), CRGB(255, 190, 0), CRGB(255, 0, 255), CRGB(0, 255, 255), CRGB(255, 255, 255) };
static uint8_t id_color = 0;          // permanent identity colour from the node id (1 red, 2 green, 3 blue, 4 yellow, 5 magenta, 6 cyan, 7 white, then repeats); 0 = unpaired
void led_set_identity(uint8_t node_id) { id_color = (node_id == 0 || node_id == 0xFF) ? 0 : (uint8_t)((node_id - 1) % 7 + 1); }
static CRGB idle_color(uint32_t now) {   // the quiet light: the node's own colour, brighter on each transmit (slow white blink when unpaired: node 2 is green)
    if (!id_color) return ((now / 1000) & 1) ? CRGB(40, 40, 40) : CRGB::Black;
    CRGB c = PAL[id_color];
    if (now >= pulse_until_ms) c.nscale8(110);
    return c;
}
static uint8_t ident_color = 0; static uint32_t ident_until_ms = 0;
void led_ident(uint8_t color, uint32_t ms) { ident_color = color > 7 ? 0 : color; ident_until_ms = millis() + ms; }
void led_mode(LedMode m) { mode = m; }
void led_local_score(uint8_t s) { score = s; }
void led_hub_state(uint8_t sc, uint8_t st) { hub_score = sc; hub_state = st; hub_state_ms = millis(); }
void led_tx_pulse() { pulse_until_ms = millis() + 80; }

void led_update(uint32_t now) {
    if (now - last_show_ms < 20) return;     // <= 50 Hz
    last_show_ms = now;
    CRGB c;
    switch (mode) {
        case LED_PAIRING:  c = ((now / 250) & 1) ? CRGB(0, 0, 255) : CRGB::Black; break;                 // blue blink 2 Hz
        case LED_OTA:      c = ((now / 125) & 1) ? CRGB(0, 0xC0, 0xC0) : CRGB::Black; break;                  // cyan fast blink while updating
        case LED_HUB_LOST: c = ((now / 125) & 1) ? CRGB(255, 255, 255) : CRGB::Black; break;             // white strobe 4 Hz
        default:
            if (hub_fresh(now)) {          // mirror the hub (same colours as hub/src/led.cpp), keeping the probe's own green heartbeat when idle
                uint8_t st = hub_state, gest = (st >> 3) & 3, hs = hub_score;
                if (gest == 1)      c = ((now / 250) & 1) ? CRGB(0x60, 0x60, 0x60) : CRGB::Black;
                else if (gest == 2) c = CRGB(255, 0, 0);
                else if (gest == 3) c = ((now / 125) & 1) ? CRGB(0, 255, 0) : CRGB::Black;
                else if (st & 4)    { uint8_t b = (uint8_t)(24 + 24 * (1.0f + sinf(now / 320.0f))); c = CRGB(0, b, b); }
                else if (st & 64)   c = ((now / 125) & 1) ? CRGB(255, 255, 255) : CRGB(0, 0, 255);   // EMF-wire touch
                else if (hs >= 71)  c = ((now / 50) & 1) ? CRGB(255, 0, 0) : CRGB::Black;
                else if (hs >= 31)  { uint8_t b = 64 + (uint8_t)(63.0f * (1.0f + sinf(now / 300.0f))); c = CRGB(b, (uint8_t)(b * 0.75f), 0); }
                else                c = idle_color(now);
            }
            else if (score >= 71)      c = ((now / 50) & 1) ? CRGB(255, 0, 0) : CRGB::Black;                  // red strobe 10 Hz
            else if (score >= 31) { uint8_t b = 64 + (uint8_t)(63.0f * (1.0f + sinf(now / 300.0f))); c = CRGB(b, (uint8_t)(b * 0.75f), 0); } // breathing amber
            else                  c = idle_color(now);         // node-colour heartbeat on TX
    }
    if (ident_color && (int32_t)(ident_until_ms - now) > 0) {      // "which board is this": a solid colour per node, overrides everything
        c = PAL[ident_color];
    }
    ledA[0] = c; ledB[0] = c;
    led2_write(mode == LED_BEACON && led2_level(now));
    FastLED.show();
}
