// Double-click BOOT to calibrate without a phone (see fieldcal.h). BOOT holds keep their meaning (3 s pairing, 8 s OTA window);
// a click is a press shorter than FC_CLICK_MAX_MS, so they never overlap.
#include <Arduino.h>
#include "config.h"
#include "hub.h"
#include "calibration.h"
#include "calib.h"
#include "logger.h"
#include "fieldcal.h"

#define FC_CLICK_MAX_MS   800
#define FC_DOUBLE_MS      900
#define FC_DELAY_MS       10000     // time to get away from the devices before the capture starts
#define FC_START_WAIT_MS  3000
#define FC_CAPTURE_MAX_MS 50000   // must exceed the 30 s capture (CAL_CAPTURE_MS) with margin
#define FC_RESULT_MS      3000

typedef enum { FC_IDLE, FC_COUNTDOWN, FC_CAPTURING, FC_RESULT } FcState;
static FcState st = FC_IDLE;
static uint32_t t0 = 0; static bool seen_capture = false;

static void finish(uint32_t now, uint8_t led_mode, const char *msg) {
    st = FC_RESULT; t0 = now; led_gesture(led_mode); logger_event(EV_CAL, 255, 0, msg);
}

static void on_double(uint32_t now) {
    if (st == FC_IDLE) {
        if (calibration_phase() != 0) return;                                  // busy (dashboard capture or pending review)
        st = FC_COUNTDOWN; t0 = now; led_gesture(1);
        logger_event(EV_CAL, 255, 0, "calibration in 10 s (BOOT double-click): move away from the boards");
    } else if (st == FC_COUNTDOWN) {
        st = FC_IDLE; led_gesture(0);
        logger_event(EV_CAL, 255, 0, "calibration cancelled (BOOT double-click)");
    }
}

void fieldcal_loop() {
    uint32_t now = millis();
    bool down = digitalRead(PIN_BOOT) == LOW;
    static bool prev = false; static uint32_t press_ms = 0, last_click = 0; static int clicks = 0;
    if (down && !prev) press_ms = now;
    if (!down && prev) {
        uint32_t d = now - press_ms;
        if (d >= 30 && d < FC_CLICK_MAX_MS) {
            if (clicks == 1 && now - last_click < FC_DOUBLE_MS) { clicks = 0; on_double(now); }
            else { clicks = 1; last_click = now; }
        } else clicks = 0;
    }
    prev = down;
    if (clicks == 1 && now - last_click > FC_DOUBLE_MS) clicks = 0;

    switch (st) {
    case FC_IDLE: break;
    case FC_COUNTDOWN:
        if (now - t0 >= FC_DELAY_MS) {
            if (calibration_request_start(-1, CAL_SENS_NORMAL)) { st = FC_CAPTURING; t0 = now; seen_capture = false; led_gesture(0); }
            else finish(now, 2, "calibration could not start (busy)");
        }
        break;
    case FC_CAPTURING: {
        int ph = calibration_phase();
        if (ph == 1) seen_capture = true;
        if (seen_capture && ph == 2) {
            const char *why = "";
            if (calibration_pending_quiet(&why)) { calibration_request_accept(); finish(now, 3, "calibration accepted (BOOT double-click)"); }
            else {
                calibration_request_discard();
                char m[64]; snprintf(m, sizeof(m), "calibration rejected: %s", why); finish(now, 2, m);
            }
        } else if ((!seen_capture && now - t0 > FC_START_WAIT_MS) || now - t0 > FC_CAPTURE_MAX_MS) finish(now, 2, "calibration aborted (timeout)");
        break; }
    case FC_RESULT:
        if (now - t0 >= FC_RESULT_MS) { led_gesture(0); st = FC_IDLE; }
        break;
    }
}
