// Phone-free calibration for field runs: double-click BOOT -> 10 s countdown (walk away) -> 30 s capture -> auto-accept if
// the area stayed still, otherwise discard. LED feedback via led_gesture(); outcomes are logged as events. Firmware-only.
#pragma once
void fieldcal_loop();       // Core 1 loop, every frame
