# Screenshots

Documentation images of the SpectralGrid dashboard. **Not part of any build:** this folder sits at the repository root,
outside both PlatformIO projects (`hub/` and `probe/`), so neither build can compile, embed or upload it. Nothing in
`hub/platformio.ini` or `probe/platformio.ini` references it.

| File | What it shows | Source |
| :--- | :--- | :--- |
| `live_dashboard.jpg` | The full dashboard, idle, hub only | Real hub (flashed ESP32-S3) |
| `live_replay.jpg` | Session replay playing (REPLAY chip, scrubber), event log with real events, tag chip | Real hub |
| `live_square_map.jpg` | Square map during a walk-through: six paths, one red, person dot | Real hub |
| `live_drift.jpg` | "Calibration suggested" banner after 5 min uncalibrated | Real hub |
| `live_mobile_full.jpg` | Phone-width layout, full page | Real hub |
| `live_calibration_review.jpg` | Calibration review with real measurements (earlier layout) | Real hub |
| `demo_calm.jpg` | Two probes, quiet grid | Built-in demo data |
| `demo_alarm.jpg` | Grid-wide anomaly: red state, glitch title, shattered beams | Demo data |
| `demo_drift.jpg` | Drift banner (antenna change suspected) | Demo data |
| `demo_context.jpg` | Cause tag chip, battery column, control-probe buttons | Demo data |
| `demo_barrier_mobile.jpg` | Barrier breach at phone width | Demo data |
| `demo_calibrating.jpg`, `demo_calibration_review.jpg` | Calibration capture and review (with a noisy-node warning) | Demo data |

`live_*` were captured by headless Chromium on the Pi while its `wlan0` was joined to the hub's access point
(`hub/tools/pi_hub_features_test.sh`). The live hub had no probes and no antenna wire, so it shows one node and a flat EMF
trace. `demo_*` use the dashboard's synthetic data (open `hub/web/dashboard.html` from a `file://` URL). The film grain
makes lossless PNGs large, so these are JPEGs (quality ~90).

Take a screenshot in real time (needed for anything involving sockets, timers or the microphone):

```bash
echo '[{"wait":3000},{"shot":"out.jpg"}]' > steps.json
node --experimental-websocket hub/tools/browser_check.mjs file://$PWD/hub/web/dashboard.html steps.json 1100 1500
```
