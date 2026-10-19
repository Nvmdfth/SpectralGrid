// Raw CSI capture (see csicap.h). Frames are queued from the Wi-Fi callback and printed by a low-priority task so the
// acquisition loop never blocks on the UART. Line format, one frame per line (the host ignores every other line):
//   CSI,<hub ms>,<node or -1>,<src mac>,<rssi>,<noise floor>,<sig_mode>,<rate>,<256 hex chars = 128 raw int8 bytes>
//   LBL,<hub ms>,<label>      CAPEND,<hub ms>,drops=<n>
#include "csicap.h"
#include <string.h>

#define CAP_RING     64
#define CSICAP_BAUD  921600
#define CAP_MAX_MS   (20UL * 60UL * 1000UL)   // safety stop so a crashed host cannot leave the UART at 921600 for good

typedef struct { uint32_t t; int8_t node; uint8_t mac[6]; int8_t rssi, noise; uint8_t sig, rate; int8_t buf[128]; } CapFrame;
static CapFrame ring[CAP_RING];
static volatile uint32_t head, tail, drops, started_ms;   // head/tail count up forever; slot = n % CAP_RING
static volatile bool on;
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

void csicap_push(const wifi_csi_info_t *info, int node) {
    if (!on) return;
    portENTER_CRITICAL(&mux);
    if (head - tail >= CAP_RING) { drops = drops + 1; portEXIT_CRITICAL(&mux); return; }
    CapFrame &f = ring[head % CAP_RING];
    f.t = millis(); f.node = (int8_t)node; memcpy(f.mac, info->mac, 6);
    f.rssi = info->rx_ctrl.rssi; f.noise = info->rx_ctrl.noise_floor; f.sig = info->rx_ctrl.sig_mode; f.rate = info->rx_ctrl.rate;
    memcpy(f.buf, info->buf, 128);
    head = head + 1;
    portEXIT_CRITICAL(&mux);
}

bool csicap_on() { return on; }

void csicap_start() {
    if (on) return;
    portENTER_CRITICAL(&mux); head = 0; tail = 0; drops = 0; portEXIT_CRITICAL(&mux);
    started_ms = millis();
    Serial.printf("console: csicap on, UART now %d baud\n", CSICAP_BAUD);
    Serial.flush(); delay(150);
    Serial.updateBaudRate(CSICAP_BAUD);
    on = true;
}

void csicap_stop() {
    if (!on) return;
    on = false;
    delay(60);                                           // let the task finish the line it is printing
    Serial.printf("CAPEND,%lu,drops=%lu\n", (unsigned long)millis(), (unsigned long)drops);
    Serial.flush(); delay(100);
    Serial.updateBaudRate(115200);
    Serial.println("console: csicap off, UART back to 115200");
}

bool csicap_label(const char *text) {
    if (!on) return false;
    char s[28]; int n = 0;
    for (; text && *text && n < 24; text++) if (isalnum((unsigned char)*text) || *text == '_' || *text == '-') s[n++] = *text;
    s[n] = 0;
    if (!n) return false;
    Serial.printf("LBL,%lu,%s\n", (unsigned long)millis(), s);
    return true;
}

static void cap_task(void *) {
    static char line[400];
    static const char hx[] = "0123456789abcdef";
    for (;;) {
        if (!on) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }
        if (millis() - started_ms > CAP_MAX_MS) { csicap_stop(); continue; }
        CapFrame f; bool got = false;
        portENTER_CRITICAL(&mux);
        if (tail != head) { f = ring[tail % CAP_RING]; tail = tail + 1; got = true; }
        portEXIT_CRITICAL(&mux);
        if (!got) { vTaskDelay(pdMS_TO_TICKS(2)); continue; }
        int n = snprintf(line, sizeof line, "CSI,%lu,%d,%02x%02x%02x%02x%02x%02x,%d,%d,%u,%u,", (unsigned long)f.t, (int)f.node,
                         f.mac[0], f.mac[1], f.mac[2], f.mac[3], f.mac[4], f.mac[5], (int)f.rssi, (int)f.noise, (unsigned)f.sig, (unsigned)f.rate);
        for (int i = 0; i < 128; i++) { uint8_t b = (uint8_t)f.buf[i]; line[n++] = hx[b >> 4]; line[n++] = hx[b & 15]; }
        line[n++] = '\n';
        Serial.write((const uint8_t *)line, n);
    }
}

void csicap_init() { xTaskCreatePinnedToCore(cap_task, "csicap", 4096, nullptr, 1, nullptr, 1); }
