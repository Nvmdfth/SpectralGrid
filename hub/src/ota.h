// Over-the-air updates (SpectralGrid.md §6/§7, backlog item "OTA"). The hub accepts firmware uploads only while an
// "OTA window" is open; the window opens by holding BOOT for 8 s or by typing `ota` on the serial console. Both need
// physical or host access, so a stranger on the open access point cannot flash the hub.
#pragma once
#include <Arduino.h>
#include "hub.h"
class AsyncWebServer;

void ota_register(AsyncWebServer &server);      // routes; call before server.begin()
void ota_loop();                                // BOOT-hold detection, deferred reboot, armed probe update
void ota_gate_feed(const GridFrame *f, uint32_t now_ms);   // link-stability tracking for the armed probe update
void ota_open_window(uint32_t ms);
void ota_close_window();
bool ota_window_open();
