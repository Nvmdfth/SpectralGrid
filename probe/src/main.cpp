#include <esp_ota_ops.h>
// SpectralGrid probe entry. Spec: SpectralGrid.md §6.
#include <Arduino.h>
#include "protocol.h"
#include "config.h"
#include "tasks.h"

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.printf("SpectralGrid probe | %s rev%d | cores=%d | psram=%u B | proto=%d\n",
                  ESP.getChipModel(), ESP.getChipRevision(), ESP.getChipCores(), (unsigned)ESP.getPsramSize(), PROTO_VERSION);
    Serial.printf("build %s %s | running partition %s\n", __DATE__, __TIME__, esp_ota_get_running_partition() ? esp_ota_get_running_partition()->label : "?");
    led_init();
    mailbox_init();
    xTaskCreatePinnedToCore(sample_task, "sample", 4096, nullptr, 5, nullptr, 0);   // Core 0: deterministic acquisition
    xTaskCreatePinnedToCore(beacon_task, "beacon", 6144, nullptr, 4, nullptr, 1);   // Core 1: radio + LED
}

void loop() { vTaskDelay(portMAX_DELAY); }
