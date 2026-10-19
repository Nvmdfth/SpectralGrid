// Raw CSI capture over the serial console (research aid, off by default). Firmware-only: hub.h stays Arduino-free.
#pragma once
#include <Arduino.h>
#include <esp_wifi.h>

void csicap_init();                                      // starts the drain task (call once, after the Wi-Fi stack is up)
void csicap_push(const wifi_csi_info_t *info, int node); // Wi-Fi task: queue one frame; node -1 = not a paired probe
bool csicap_on();
void csicap_start();                                     // console only: switches the UART to CSICAP_BAUD
void csicap_stop();                                      // switches the UART back to 115200
bool csicap_label(const char *text);                     // writes a LBL line into the stream; false when capture is off
