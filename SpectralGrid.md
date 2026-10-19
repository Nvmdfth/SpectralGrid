# Technical Specification & Product Requirements Document (PRD)

**Project:** SpectralGrid — Unified ESP32-S3 Paranormal Telemetry Hub + Expandable Probe Mesh
**Supersedes:** `SpectralNode.md` (single node) and `SpectralMultiNode.md` (dual node). Both are the degenerate cases of this design: *Hub only* and *Hub + 1 probe*.
**Target Hardware:** ESP32-S3-DevKitC-1 (N16R8: 16MB Quad-SPI Flash, 8MB Octal PSRAM, Dual-Core LX7 @ 240MHz, WS2812 on GPIO 48)
**Toolchain:** VS Code / PlatformIO, Arduino framework on ESP-IDF 5.x (see §4.3)
**Topology:** 1 × **Hub** (SoftAP, web gateway, CSI receiver, scoring) + 0…N × **Probe** (ESP-NOW sounding beacon + remote EMF/thermal sensor)
**Primary UI:** SoftAP captive portal, HTML5 Canvas CRT dashboard, Web Audio API

---

## 1. System Overview

The Hub is a complete standalone SpectralNode (local EMF, die temperature, WS2812 meter, dashboard). Each additional Probe adds three things:

1. **A bistatic RF link.** Probe → Hub ESP-NOW sounding frames let the Hub capture per-frame CSI for that link, creating a Fresnel-zone tripwire between the two boards.
2. **A remote EMF/thermal sensor** free of Wi-Fi self-interference (no AP, no TCP/IP, TX only).
3. **Spatial vectoring.** Per-node EMF compared against per-link CSI tells whether an event is local to one node, or something crossing a specific link.

```text
PROBE 1..N (flash: probe/)                 HUB (flash: hub/)
┌───────────────────────────────┐          ┌──────────────────────────────────────────┐
│ C0: 200 Hz touch GPIO7, 10 Hz │          │ C0: CSI ingest (per-MAC), ESP-NOW RX,    │
│     die temp                  │ ESP-NOW  │     local touch GPIO7 @ 200 Hz, die temp│
│ C1: ESP-NOW TX (staggered     │ ───────▶ │ C1: aggregator, anomaly engine, SoftAP,  │
│     slot), WS2812 status      │ ch 6     │     DNS, WebSocket, static assets        │
└───────────────────────────────┘          └─────────────────┬────────────────────────┘
                                                             │ SoftAP "SPECTRAL_GRID"
                                                             ▼
                                           CLIENT BROWSER: floor-plan canvas (hub + probes + links),
                                           per-node strip charts, directional Geiger / drone audio
```

Core split rule (both firmwares): **Core 0 = deterministic acquisition only; Core 1 = radio TX / aggregation / network.** (`SpectralMultiNode.md` put Node B's local EMF sampling on Core 1; it moves to Core 0 here.)

---

## 2. Repository Layout (two separately flashable codebases)

```text
SpectralGrid/
├── shared/                     # header-only, included by both firmwares
│   ├── protocol.h              # packet structs, magic/version, channel, slot timing
│   └── dsp.h                   # EMA baseline, p2p, slope helpers (no Arduino deps)
├── hub/                        # PlatformIO project #1 -> flash ONCE to the gateway board
│   ├── platformio.ini
│   ├── src/                    # main.cpp, acquisition.cpp, csi.cpp, mesh.cpp, score.cpp, web.cpp, led.cpp
│   └── data/ or web/           # dashboard.html -> gzipped PROGMEM header at build
└── probe/                      # PlatformIO project #2 -> flash once PER extra board
    ├── platformio.ini
    └── src/                    # main.cpp, sample.cpp, beacon.cpp, led.cpp, pairing.cpp
```

- Each project builds and uploads independently (`pio run -d hub -t upload`, `pio run -d probe -t upload`). Neither pulls in the other's libraries; the probe has no web stack.
- Both set `build_flags = -I ../shared`. `protocol.h` is the single source of truth for the wire format; a `PROTO_VERSION` mismatch makes the Hub ignore the probe and show it as `INCOMPATIBLE` in the dashboard.
- Probe identity is **not** compiled in, so one probe binary serves all extra boards (see §5.3).

---

## 3. Hardware & Pin Assignments

Identical board for every role; role is determined by which firmware is flashed.

| Subsystem | Pin / Periph | Requirement | Hub | Probe |
| :--- | :--- | :--- | :--- | :--- |
| **EMF Antenna** | GPIO 7 (Touch Ch 7) | 10 cm 22 AWG solid-core wire, floating, unshielded | Local EMF | Remote EMF |
| **Status LED** | GPIO 48 (WS2812) | Onboard | Grid composite score | Local score + TX heartbeat |
| **Die Temp** | Internal sensor | Cold-spot tracking (−dT/dt) | Local | Remote |
| **2.4 GHz RF** | PCB trace antenna | Fixed channel 6 | AP + ESP-NOW RX + CSI | ESP-NOW TX only |
| **Pairing button** | GPIO 0 (BOOT) | Onboard | Hold 3 s = open pairing window | Hold 3 s = request pairing |
| **Power** | VIN/5V, GND | Battery bank or 1S LiPo/18650 + TP4056; no AC-derived supply | USB battery bank | USB battery bank / LiPo |

Isolated DC power for every node is required (no 60 Hz ground loops, no phone-charger switching noise).

---

## 4. Build Configuration

### 4.1 `hub/platformio.ini`

```ini
[env:hub]
platform = https://github.com/pioarduino/platform-espressif32/releases/download/stable/platform-espressif32.zip
board = esp32-s3-devkitc-1
framework = arduino
board_build.mcu = esp32s3
board_build.f_cpu = 240000000L
board_build.flash_size = 16MB
board_build.arduino.memory_type = qio_opi
board_build.partitions = default_16MB.csv
build_flags =
    -I ../shared
    -DROLE_HUB=1
    -DBOARD_HAS_PSRAM
    -DCORE_DEBUG_LEVEL=1
lib_deps =
    esp32async/ESPAsyncWebServer
    esp32async/AsyncTCP
    bblanchon/ArduinoJson @ ^7
    fastled/FastLED @ ^3.6.0
```

### 4.2 `probe/platformio.ini`

```ini
[env:probe]
; platform / board / memory_type lines identical to hub
build_flags =
    -I ../shared
    -DROLE_PROBE=1
    -DBOARD_HAS_PSRAM
    -DCORE_DEBUG_LEVEL=1
lib_deps =
    fastled/FastLED @ ^3.6.0
```

Note: `ARDUINO_USB_CDC_ON_BOOT` is deliberately **not** set. The dev board is used over its UART (CH343) port; with that flag `Serial` would go to the native USB port and print nothing. Re-add it only if switching to the native USB port.

### 4.3 Corrections carried over from the source plans

- `platform = espressif32 @ ~6.5.0` ships Arduino-ESP32 2.x / IDF 4.4, which has **no** `driver/temperature_sensor.h` (that is IDF 5.x; IDF 4.4 uses `driver/temp_sensor.h`). The merged plan uses pioarduino (Arduino core 3.x / IDF 5.x) so the documented API is correct. Verify `esp_wifi_set_csi_rx_cb` signatures against the IDF version actually resolved.
- `me-no-dev/ESPAsyncWebServer` is unmaintained and has known issues on core 3.x; use the `esp32async` forks.
- `psram_type = opi` is not the reliable selector for N16R8; use `board_build.arduino.memory_type = qio_opi`. Confirm with a PSRAM size print at boot.
- `SpectralMultiNode.md` wrapped the library URLs in Markdown links inside the `.ini` block (invalid). Fixed above.

---

## 5. Mesh Protocol (`shared/protocol.h`)

### 5.1 Radio plan

- Channel **6**, fixed on every node. Hub runs `WIFI_AP_STA` locked to channel 6; probes run `WIFI_STA` (no association, no beacons, no TCP/IP use) with `esp_wifi_set_channel(6, ...)`. A channel mismatch silently drops both ESP-NOW and CSI.
- TX power throttled to 13 dBm on all nodes so the radio does not couple into the high-impedance GPIO 7 antenna.
- ESP-NOW peers: Hub holds a paired-probe table (max 16 probes; ESP-NOW hard limit is 20 peers, 6 of which may also be used for broadcast/other).

### 5.2 Airtime and slotting

N probes at 50 Hz each would collide. Hub assigns each probe a `slot` and the probe transmits at `t = slot * SLOT_US` within a 20 ms frame (default `SLOT_US = 1500` → 13 slots/frame). Probes align to the Hub's 1 Hz `HubSync` broadcast (±1 ms is sufficient). Per-probe rate may be lowered (`tx_hz` 10/25/50) to fit larger meshes; CSI quality per link scales with `tx_hz`.

### 5.3 Packets (all `__attribute__((packed))`, little-endian, header on every type)

```c
typedef struct { uint8_t magic; uint8_t proto_ver; uint8_t type; uint8_t node_id; } PktHdr;
// type: 1=Telemetry 2=PairReq 3=PairAck 4=HubSync

typedef struct {            // probe -> hub, 50 Hz (also the CSI sounding frame)
    PktHdr   h;
    uint32_t seq;           // gap detection per link
    uint16_t emf_raw;
    uint16_t emf_p2p;
    int16_t  temp_centi_c;
    int16_t  temp_slope_centi;  // rolling 3 s slope * 100
    uint8_t  flags;         // bit0 low-battery, bit1 sensor-fault, bit2 local-alarm
} ProbeTelemetry;           // 17 bytes

typedef struct {            // hub -> probe (unicast reply to PairReq)
    PktHdr   h;
    uint8_t  slot;
    uint8_t  tx_hz;
    uint32_t hub_time_ms;
} PairAck;
```

### 5.4 Pairing (replaces hardcoded MAC)

1. Hub: hold BOOT 3 s (or dashboard button) → 60 s pairing window, LED blue.
2. Probe (unpaired, or BOOT held 3 s): broadcasts `PairReq` on ch 6 every 500 ms, LED blue.
3. Hub accepts the first `PairReq` in the window, assigns `node_id` + `slot`, replies `PairAck`, stores MAC in NVS.
4. Probe stores `{hub_mac, node_id, slot, tx_hz}` in NVS and boots straight into beaconing thereafter.

Optional hardening: ESP-NOW CCMP encryption with a key derived during pairing. Not required for v1.

### 5.5 Liveness

Hub marks a probe `OFFLINE` after 1 s without a frame; its links drop out of scoring (not scored as zero). Dashboard greys the node. Packet `seq` gaps are counted as per-link loss %.

---

## 6. Probe Firmware (`probe/`)

### 6.1 Core 0 — `SampleTask` (priority high, pinned)

1. **EMF:** `touchRead(7)` windowed to a 200 Hz output rate. Windows are 16–32 samples (the source plans conflicted: 128 samples every 250 µs ≈ 32 ms vs. 64 samples every 5 ms; neither fits a 200 Hz cadence). **Measure the real `touchRead` conversion time on the S3 first**, then size the window so one window ≤ 4 ms. Output `emf_raw`, `emf_p2p = max − min`.
2. **Die temp:** 10 Hz via `temperature_sensor_handle_t`; 3 s rolling slope and 30 s baseline.
3. Write a `ProbeTelemetry` to a single-slot mailbox (latest wins) for Core 1.

### 6.2 Core 1 — `BeaconTask`

- Transmit the latest sample at the assigned slot each frame via `esp_now_send` (unicast to Hub MAC so the Hub's CSI callback sees a deterministic frame source).
- WS2812: pulse green on each TX burst; amber/red when local `emf_p2p` exceeds the local baseline (local-only EMA, §8). Blue = unpaired/pairing. Rapid white = Hub lost.
- Hub-lost behaviour: no `HubSync` for 5 s → keep sampling, fall back to broadcast `PairReq` probing at 1 Hz (so a rebooted Hub with an empty table still re-adopts known probes only if its MAC is in the probe's NVS; otherwise requires manual pairing).

---

## 7. Hub Firmware (`hub/`)

### 7.1 Core 0 — acquisition

- **Local EMF** (`touchRead(7)`, same windowing as probe) and **die temp** → local `NodeSample` with `node_id = 0`.
- **ESP-NOW RX callback** → copy `ProbeTelemetry` + `rx_ctrl.rssi` into a ring buffer keyed by `node_id`.
- **CSI callback** (`esp_wifi_set_csi_rx_cb`): look up the source MAC in the paired table, discard unknowns. For HT20 frames take the 128-byte buffer as (Q,I) int8 pairs, usable subcarriers `k ∈ [6..58]`, `A_k = √(I² + Q²)`, `σ²_CSI = (1/N) Σ (A_k − μ)²`. Result is stored per link: `{node_id, seq, csi_var, rssi}`. Do no float-heavy work beyond this in the callback; hand off through the ring buffer.
- **Standalone fallback (zero probes paired):** CSI is taken from client frames hitting the Hub's own AP (the phone). That link is weak and scored with the legacy single-node weights (§8.3). It also self-reports as `LINK_QUALITY=LOW` in the dashboard.

### 7.2 Core 1 — network and supervision

- **Mesh manager:** paired-probe table (NVS), slot assignment, 1 Hz `HubSync`, pairing window.
- **SoftAP:** SSID `SPECTRAL_GRID`, channel 6, open or WPA2 `paranormal2026`.
- **DNSServer:** `*` → `192.168.4.1` (captive portal).
- **WebSocket `/ws`:** 20 Hz decimated broadcast. REST: `GET /api/nodes`, `POST /api/pair`, `POST /api/node/{id}` (label, floor-plan x/y, tx_hz), `POST /api/calibrate`.
- **Aggregator and anomaly engine** (§8).
- **WS2812 (GPIO 48 or 38, see §12 #18):** own Core 1 task at 100 Hz. Overrides: cyan breathe while calibrating, blue blink 2 Hz while the pairing window is open. Otherwise the score meter: 0–30 green `#002200`; 31–70 breathing amber `#443300`; 71–100 red strobe 10 Hz.

### 7.3 Telemetry payload (one frame, N nodes)

```json
{
  "t": 1042300, "score": 68, "vector": "BARRIER_BREACH", "vec_ids": [1],
  "nodes": [
    {"id":0,"emf":12,"temp":23.1,"d_temp":0.00,"on":1},
    {"id":1,"emf":32,"temp":22.4,"d_temp":-0.01,"on":1,"rssi":-48,"loss":0.4}
  ],
  "links": [
    {"id":1,"csi":14.82,"q":"OK"}
  ]
}
```

Node 0 is always the Hub. With zero probes, `links` is the single AP-side pseudo-link and `nodes` has one entry; the frontend renders identically.

---

## 8. Composite Anomaly Engine

### 8.1 Normalisation (per node, per link, per channel)

Each channel keeps a running EMA baseline and variance: `μ_t = α·X_t + (1−α)·μ_{t−1}`, `α = 0.02` (~10 s horizon at the 20 Hz scoring rate; recompute α if the scoring rate changes). `X̂ = clamp((X − μ − 2.5σ) / scale, 0, 1)·100`. Only positive deviations beyond 2.5σ contribute; a channel is not scored until its baseline has had 10 s of warm-up.

### 8.2 Aggregate score (probes present)

`S = min(100, w_csi·max_l(CSÎ_l) + w_emf·max_n(EMF̂_n) + w_t·max_n(ΔT̂_neg,n))`

| Weight | Value | Note |
| :--- | :--- | :--- |
| `w_csi` | 0.40 | worst link |
| `w_emf` | 0.45 | worst node (sum of the two source plans' EMF weights was 0.50; the 0.05 moves to thermal) |
| `w_t` | 0.15 | worst node |

### 8.3 Degraded mode (no probes paired)

Original SpectralNode weights: EMF 0.45, thermal 0.25, CSI 0.30 on the Hub's own channels. Mode switches automatically as probes appear or go offline, with a 3 s hysteresis to avoid score steps.

Thermal is the exception to §8.1 normalisation: it is an absolute 3 s drop with a fixed gate (`temp_gate_c` = 1.0 °C, tolerance 0.05, `temp_scale` = 1.0 °C above the gate maps to 100), because the die sensor quantises in ~1 °C steps. Die temperature stays in the score by decision (2026-10-08), with this coarser gate in place of the original 0.25 °C.

All weights are tunable starting points, exposed through `/api/calibrate`; not validated on hardware.

### 8.4 Vector classification (N-node generalisation)

| Vector | Condition |
| :--- | :--- |
| `QUIET` | `S` below 20 |
| `LOCAL_NODE_<id>` | `EMF̂_id` high; all other nodes quiet; its link CSI mild |
| `BARRIER_BREACH_<id>` | `CSÎ_id` high while `EMF̂` of both link endpoints (Hub and probe `id`) is low |
| `MULTI_LINK_BREACH` | ≥2 links high, EMF quiet (motion in the Hub's neighbourhood or large body) |
| `GRID_ANOMALY` | ≥2 nodes EMF high *and* ≥1 link CSI high → high-priority alarm |
| `THERMAL_<id>` | 3 s temperature drop on one node exceeding the thermal gate (default 1 °C, i.e. more than one die-sensor step; 2 °C scores 100) with EMF/CSI quiet |

---

## 9. Dashboard SPA (served from Hub PROGMEM, gzipped)

### 9.1 Design tokens

Void black `#050805`, phosphor green `#00FF66`, cyan `#00E5FF`, amber `#FFB000`, red `#FF2222`. Mobile viewport locked (`width=device-width, initial-scale=1, user-scalable=no`). No CDN dependencies; everything inline.

### 9.2 Modules

1. **Floor-plan / radar canvas.** Hub at the origin with a rotating CRT sweep (phosphor persistence via `rgba(0,8,0,0.08)` clears); each probe is an anchor at user-placed x/y (drag to set, persisted via `POST /api/node/{id}`). Each link draws its line of sight and 1st Fresnel ellipse; baseline = coherent ripple, high CSI = beam distortion + sparks scaling with `csi`. Single-node mode shows only the sweep with CSI blips.
2. **Strip charts.** One trace per node (colour-coded), 100-point / 10 s window, EMF and ΔT toggle; a differential band highlights migration of activity between nodes.
3. **Node list.** Label, online state, RSSI, packet loss, battery flag; Pair button.
4. **Audio (Web Audio API, client-synthesised).**
   - *Geiger:* white-noise bursts through a 1.2 kHz high-pass; rate ∝ anomaly; **pitch per `node_id`** (hub lowest, probes stepping upward).
   - *Sub-bass / drone:* 40–45 Hz carrier through a resonant bandpass whose centre frequency tracks max link CSI variance. Needs a user gesture to start `AudioContext` (browser policy).
5. **Calibration panel** (implemented). Per-node or all-node "Calibrate", needed whenever an antenna wire is added, swapped or moved, a node is relocated, or readings drift.
   - Flow: choose node(s) and sensitivity (low/normal/high) -> **capture** 30 s (15 s until 2026-10-08) while the area is still (progress bar; LED breathes cyan) -> **review** a table of before/after noise floor (mean, sigma, max) and the new scale per node and channel (EMF, RF link) -> **Accept** (applies at once and stores in flash) or **Discard**. Nothing changes until Accept; a channel the capture could not measure (node offline, fewer than 100 samples) keeps its old calibration. "Reset to defaults" per node or all.
   - **No new packet and no probe change:** the hub captures statistics from the normal telemetry stream (probe `emf_p2p`, per-link CSI variance, and its own EMF). This replaces the earlier idea of the probe streaming raw windows (`CalReq`); probes store nothing.
   - Result per channel: baseline mean/variance seed (skips the 10 s warm-up) and a scale = max(floor, k x sigma) with k = 20 / 12 / 6 for low / normal / high sensitivity (floors: EMF 10 counts, RF 6). Heuristic starting points, not validated.
   - Guard rails: a capture whose largest excursion exceeds 12 sigma (and the floor) is flagged "disturbance during capture"; Accept then asks for confirmation.
   - REST: `POST /api/calibrate?node=all|N&sens=0|1|2` (202), `POST /api/calibrate/accept`, `/discard`, `/reset?node=all|N`, `GET /api/calibration`. WebSocket frames carry a small `cal` object (state, seconds left) only while a capture or review is active.
   - Optional later: auto-prompt when a node's raw touch level shifts by more than a set amount (antenna changed).

---

## 10. Build order

1. **Shared protocol and DSP** (§5, §8.1): packed structs, slot timing, EMA/variance helpers, host unit tests.
2. **Probe** (§6): touch + temperature sampling, slotted ESP-NOW TX, NVS pairing, WS2812 status.
3. **Hub acquisition and mesh** (§7.1, §7.2, §5.4): local EMF and temperature, ESP-NOW RX, per-MAC CSI, paired-probe table, slot assignment.
4. **Hub scoring and network** (§8, §7.2, §7.3): aggregator, SoftAP/DNS/WebSocket/REST, LED meter, embedded gzipped dashboard.
5. **Dashboard** (§9): one self-contained `dashboard.html` for 1…N nodes.

---

## 11. Verification and Calibration

1. **Build isolation:** `pio run -d hub` and `pio run -d probe` each succeed from a clean checkout; probe binary contains no ESPAsyncWebServer symbols.
2. **Power isolation:** all nodes on battery banks; no 60 Hz harmonic in touch readings.
3. **Pairing:** flash two blank probes with the *same* binary; pair both; confirm distinct `node_id`/`slot`, and persistence across reboot of both Hub and probes.
4. **Channel check:** all nodes on ch 6 (print `esp_wifi_get_channel`); CSI callback fires only for paired MACs.
5. **Slot integrity:** with 3 probes at 50 Hz, per-link packet loss < 2 % over 5 min; CSI frame rate per link ≥ 45 Hz.
6. **Core separation:** Core 0 sampling ≥ 195 Hz with zero dropped windows while WebSocket streams at 20 Hz to two clients.
7. **Barrier test:** Hub and probe 4 m apart, waist height; walk through the link → `BARRIER_BREACH_<id>`, both EMF below threshold.
8. **EMF isolation test:** energised AC cord near probe 1 antenna → `LOCAL_NODE_1`; Hub EMF stays < 10 %.
9. **Degradation:** power off all probes → Hub switches to §8.3 weights within ~3 s, dashboard shows probes offline, no score discontinuity > 15 points.
10. **Capacitive Pin Test (carried over):** energised cord at the Hub's GPIO 7 wire → immediate `emf_p2p` spike and Geiger clicks.

---

## 12. Open Items and Status

Last updated 2026-10-08 (late). **Item numbers (#N) are referenced from code comments and other docs: do not renumber.**
"Verified" means observed on a real ESP32-S3. As of this update a hub and one probe run together (pairing, telemetry, CSI per probe frame); see 12.9 for what that session verified and the new items #30-#36.

### 12.1 Settled (verified on hardware)

| # | Item | Result |
| :--- | :--- | :--- |
| 1 | `touchRead` timing vs. the 200 Hz window | ~3.3 µs/read; a 64-sample window takes ~210 µs (max ~450 µs with the radio on); 200 windows/s steady, loop gap 5.0-5.4 ms. The first window after boot saturates (4194303): discarded and clamped in code. Measured with no antenna wire. |
| 2 | pioarduino platform / IDF 5.x APIs | Builds and runs: `temperature_sensor`, ESP-NOW (`esp_now_recv_info_t` callback, per-peer rate config), CSI callback, AsyncWebServer. |
| 3 | `esp32async` AsyncWebServer / AsyncTCP | Serves HTTP, WebSocket (19.4-19.6 Hz) and captive-portal redirects. |
| 4 | PSRAM selector `qio_opi` | 8 MB seen at boot. |
| 9 | Standalone CSI from client frames | CSI is delivered for client frames to the hub AP (2-16 frames/s tracking ~10 Hz traffic, RSSI -27 dBm at close range). |

### 12.2 Decided / design corrections

| # | Item | Decision |
| :--- | :--- | :--- |
| 12 | Die temperature is not an ambient sensor | Confirmed: +5 °C in ~6 s once the radio starts; quantises in ~1 °C steps. **Decision 2026-10-08: keep it in the score, with the thermal gate moved from 0.25 °C to 1 °C** (a drop must exceed one sensor step; 2 °C scores 100). An external sensor is the real fix (README roadmap). |
| 15 | Slot capacity vs. the 16-probe cap | 13 slots fit a 20 ms frame at 50 Hz. The hub must drop `tx_hz` to 25 (26 slots) beyond 13 probes. Enforced and host-tested in `peers.cpp`/`protocol.h`; not yet exercised with real probes. |
| 11 | Calibration flow | Implemented as capture -> review -> accept/discard/reset with no new packet (the hub captures from existing telemetry). Verified on hardware, hub only (REST, flash persistence). See "partly verified" below for what remains. |

### 12.3 Partly verified

| # | Item | Verified | Still open |
| :--- | :--- | :--- | :--- |
| 16 | Hub mesh/CSI design choices | (a) LLTF-only config gives uniform 128-byte frames for HT and legacy client frames. | (b) `csi_lltf_variance` assumes LLTF order 0..31 then -32..-1 and masks DC + guards 27..37 (differs from the original k in [6..58]); (c) hub sends ESP-NOW on the STA interface and probes learn its MAC from the PairAck source; (d) re-ack of probes beaconing under an unknown/mismatched id (only while pairing is open for unknown MACs); (e) a beaconing probe accepts a PairAck from its own hub (rate change / renumber). All need a real probe. |
| 17 | Core separation under load | Hub acquisition ran 194-202 windows/s (target >= 195) with AP + one WebSocket client + CSI. | With probes sending 50 Hz telemetry and two clients. |
| 11 | Calibration (see 12.2) | Capture, review, accept, reset, reload after reboot. | The scale heuristic (k = 20/12/6 sigma; floors EMF 10, RF 6) on a real antenna; calibrating a probe node; CSI sample count when the link goes quiet (a stale link gave 129 of ~300 samples; still valid above the 100 minimum). |

### 12.4 Open: needs a second board

| # | Item | What to check |
| :--- | :--- | :--- |
| 19 | **Probe firmware** | **Update 2026-10-08: paired with the hub, sends telemetry (0 % loss, RSSI -13 at 1 ft, about -40 at a few metres, -80 across a floor/obstructions), runs on battery, re-pairs automatically after a hub reboot.** Still untested: BOOT-hold re-pair, hub-lost in anger, the blue blink; probe OTA install (#31). *Earlier note:* **Smoke test passed 2026-10-08 (one board, no hub):** clean boot into `PAIRING`, sane sampling (`emf raw` ~15100, `p2p` 0), HT MCS0 peer-rate call OK, 31 `PairReq` broadcasts at 500 ms with 0 send errors (accepted by the radio, reception unproven), no NVS error on a fresh board. **Still untested:** pairing, slotted telemetry, hub-lost, BOOT-hold re-pair, persistence of a pairing, the blue blink (needs eyes). |
| 14 | ESP-NOW PHY rate must allow CSI | **VERIFIED 2026-10-08:** the hub gets a CSI frame per probe frame (~50 frames/s on the probe link), `len` = 128, `sig_mode` = 1 (HT). See 12.9. |
| 7 | Slot timing (`SLOT_US` 1500, ±1 ms HubSync alignment) | **Partly verified (1 probe):** 0.0 % loss, ~50 CSI frames/s. Still open: 3+ probes over 5 min. |
| 8 | ESP-NOW peer limits (16 probes) | Confirm against the real peer table limit, and any broadcast/encrypted peers. |
| 10 | Probe hub-lost / re-adoption | **Partly verified:** the probe re-paired by itself within ~6 s after each hub reflash/reboot (pairing persisted on both sides). Still open: empty-table reboot, probe in HUB_LOST and PAIRING. |

### 12.5 Open: needs a person at the hardware

| # | Item | What to check |
| :--- | :--- | :--- |
| 18 | Onboard LED pin and colours | **Partly settled 2026-10-08:** the RGB LED is on GPIO 48 (seen green); the board is a Freenove ESP32-S3-WROOM with a user LED on **IO2** (GPIO 2) next to it, plus power and UART RX/TX LEDs (the latter two are hardware-driven). Still to do: drop the GPIO 38 drive in hub and probe, fix §3/§7.2, check strobe timing and colour mapping, and confirm the IO2 polarity (#33). |
| 13 | Noise environment + antenna wire | **Update:** a jumper wire is now attached to GPIO 7 on both boards (see #32). Original note: board next to `devpi` and a server, **no antenna wire**. Attach a wire, run `bringup` on the bench, then on battery a few metres away, compare `p2p`. Also the radio-bleed premise (radio on vs. off with a wire attached). |
| 20 | Phone/browser behaviour | Captive portal pop-up on iOS/Android, WebSocket stability, Web Audio (Geiger/drone) on a real phone, drag-to-place on touch. Only headless Chromium has been used. |

### 12.6 Open: needs tuning data

| # | Item | Notes |
| :--- | :--- | :--- |
| 5 | Scoring weights (CSI 0.40 / EMF 0.45 / thermal 0.15; degraded 0.45/0.25/0.30) and scales | Unvalidated starting points. Calibration (#11) replaces the guessed scales once a real antenna exists. Single-channel events cap below the red strobe by design (EMF 45, CSI 40, thermal 15). |
| 6 | EMA α = 0.02 | ~10 s horizon only at 20 Hz scoring; recompute if the scoring rate changes. |

### 12.7 Decisions needed (documented, not blocking)

Each was assumed as stated so work could continue. Change the assumption by telling me; none of them blocks other items.

| # | Decision | Assumption taken | Why it needs you |
| :--- | :--- | :--- | :--- |
| 21 | **Secure context for phone mic and system notifications** | Both features are built and degrade with a clear on-page message. Browsers refuse `getUserMedia` and `Notification` on plain `http://192.168.4.1`. Vibration and in-page alerts work regardless. | Options: (a) Android Chrome: add the hub URL to `chrome://flags/#unsafely-treat-insecure-origin-as-secure` (works, per device, no iOS equivalent); (b) HTTPS on the hub with a self-signed certificate (browser warning on every device, the async web server has no TLS, and it conflicts with the captive portal); (c) wrap the dashboard in an app/PWA served from a secure origin. Needs a choice on how much to invest. |
| 22 | **Probe OTA: enable it, and how to provision the secret** | Code is written but **inactive unless `shared/ota_secret.h` exists** (gitignored; example provided). Commands are HMAC-SHA256 authenticated and the firmware MD5 is inside the authenticated part. | A shared secret compiled into every firmware means rotating it needs a cable reflash of all nodes. The Arduino core's bootloader rollback is not confirmed enabled, so a bad image could boot-loop a remote probe (recovery: serial). Alternatives: signed images (ed25519), custom sdkconfig for rollback, or hub-only OTA. Also **untested on hardware** (needs a second board). |
| 23 | **Who may flash the hub over OTA** | Only while an OTA window is open: hold BOOT 8 s, or type `ota` on the serial console (the Pi can do that). No remote token. | Remote OTA with nobody at the board is only possible through the host's serial console. A token/signature would allow true remote OTA but adds a secret to manage. |
| 24 | **Wall-clock source** | The dashboard posts the phone's clock when it connects; no RTC or NTP (the AP has no internet). Events before the first connection after a boot carry uptime only. | A DS3231 RTC (hardware) or an NTP bridge would give timestamps from boot. |
| 25 | **Battery sensing wiring** | Disabled by default (`BATT_PIN = -1`). Assumed wiring: 1S LiPo/18650, 100k/100k divider (ratio 2.0) to an ADC1 pin (suggest GPIO 1), 11 dB attenuation, linear 3.3-4.2 V map, low = under 3.3 V. | Depends on your actual battery and wiring; needs a calibrated check with a multimeter. |
| 26 | **Session recording is volatile** | A 2 MB PSRAM ring (~5 h at 2 Hz with 3 nodes) is lost on reboot; the events log persists (48 KB file). | Persisting frames needs periodic flash writes (wear, 2.5 MB partition) or an SD card. |
| 27 | **Tags annotate, never suppress** | A tag labels the log and the score as "possibly explained"; the score and LED are unchanged. | You may prefer tagged windows to dampen or silence alerts. |
| 28 | **Control-probe semantics** | The control probe is excluded from score and vectors; if it is disturbed at the same time as another node the vector becomes `COMMON_MODE` (score unchanged). | The classification rule and thresholds are my choice and unvalidated; real placement will tell. |
| 29 | **Open access point** | Still open (no password). Pairing, tags, control, calibration, clock and reset endpoints are reachable by anyone in range; OTA is not (see #23). Tag text from the API is rendered as plain text in the dashboard. | A WPA2 password is a one-line config change (`AP_PASS`) but every device must then enter it, and the captive portal pop-up becomes less seamless. |

### 12.8 Software backlog status (2026-10-08)

| Item | Verified | Not verified |
| :--- | :--- | :--- |
| Event log, session recording, replay, CSV export | On the real hub: events with wall clock, history + live push, flash persistence across an OTA reboot, session info/frames/seek, both CSV exports (94 session rows, wall clock), replay of 152 frames in a real browser. Host tests cover the ring, encoding, CSV streaming (including mid-export drops) and the detector. | Session ring is volatile (#26); a long soak (hours) and the ring wrapping on real hardware. |
| False-positive controls | Tags API + WebSocket + UI live (chips, clear, invalid kind 400, markup stored and rendered as plain text); control set/clear/range validation and its event on the hub. Host tests cover `COMMON_MODE`, control exclusion and tag expiry. | `COMMON_MODE` and control exclusion with real probes (needs a second board, #28). |
| Phone-mic EVP recorder | Real-time headless Chromium with a fake microphone: arm, record, save to IndexedDB, list, playable blob. | A real phone; real browsers block it on plain http (#21). |
| Vibration and notifications | The page correctly detects and disables unsupported features on the live (http) hub. | Vibration and notifications on a phone; in-page toast on real alerts. |
| OTA | **Hub, on hardware:** window closed -> 403; window cannot be opened over Wi-Fi; opened from the serial console; a truncated image is rejected and the old firmware keeps running; a probe image is stored and downloads byte-identical; a real update flips app0 -> app1, reboots and keeps events and stored data. Probe OTA code compiles with a throwaway secret (+146 KB) and is compiled out otherwise. | Probe-side OTA end to end (needs a probe and a provisioned secret, #22); remote OTA without the serial path (#23). |
| Probe battery voltage | Protocol v2 runs on both boards (probe boots `proto=2`, sends `PairReq`; hub carries `vbat`); host tests cover the maths. | Reading a real battery through a divider (#25). |
| Calibration drift prompt | Live: the uncalibrated prompt fired after 5 min uptime (event, WebSocket field, dashboard banner) and cleared ~35 s after a calibration was accepted. Host tests cover the antenna and noise kinds. | Antenna and noise kinds on real hardware (needs a wire to attach and remove). |

### 12.9 Two-board session and CSI presence (2026-10-08)

Verified on hardware this session: hub + probe pairing and telemetry; CSI from probe frames (#14); loss and RSSI at three distances; battery operation; the dashboard fixes on a real phone; a raw CSI capture of two labelled sessions (`captures/`, gitignored; tools in `hub/tools/csi_*.py`).

| # | Item | Status |
| :--- | :--- | :--- |
| 30 | **CSI presence detector** (motion anywhere in the room + shadow on the link) | Designed from the captured data; built (`shared/csi_window.h`, `shared/presence.h`, wired into `score.cpp`/`calibration.cpp`); host-tested; replay of the firmware's own code over both sessions with a 15 s empty-room calibration: empty room 100 % clear (288 windows, transitions excluded), walking the room 100 % motion, standing on the line 94 % on-line, crossing 90 % detected (**caveat: the session-2 "cross" blocks were shadowed 56-76 % of the time with unbroken stretches of 12-19 s, i.e. mostly on or near the line, not quick crossings; true quick crossings are untested**), **sitting still off the line only ~25 %**. **Flashed 2026-10-08; not yet calibrated or walk-tested on the board.** Details in 13.7 |
| 31 | **Probe OTA** | **Works, verified twice on 2026-10-08** with both boards on the Pi (`hub/tools/probe_ota.sh`): hub accepts (202), probe logs `ota: success, rebooting`, boots the other partition (app0 -> app1 -> app0), rejoins in ~10 s. The earlier "never returned" case coincided with a serial monitor open on a PC (probable cause: DTR/RTS toggling the reset lines; not proven). A separate hub bug found during the work: a client joining while the 1.2 MB image uploaded once made the Wi-Fi task starve the IDLE0 watchdog (hub reboot, `ppTask` in the backtrace); fixed by not watching idle tasks and a 20 s timeout (`main.cpp`). Remaining: rollback is still not enabled (#22), the update path from a PC-attached probe is unverified |
| 32 | **GPIO 7 antenna wires (EMF channel)** | Wires attached to both boards (insulated jumper, ~1 inch bare tip, left floating). EMF raw rose to ~19,000-21,000 and is steady (p2p 0 at idle). **Response to a nearby body or a touch is unmeasured; EMF is not part of the capture tool yet.** Needs a hands-on test and EMF calibration with the wire fixed in place |
| 33 | **IO2 LED as a detection indicator** | Added to the hub (`led.cpp`, `LED2_ACTIVE_HIGH`): off = clear, solid = motion, 5 Hz blink = on the link, 1 Hz = calibrating, 3 blinks at boot. Polarity and wiring unverified |
| 34 | **`esp_cache_msync(113)` boot message** | Printed at every probe boot, including a clean USB flash. No functional effect observed; cause not investigated |
| 35 | **Position tracking needs more nodes** | One link cannot localise (no angle of arrival on a single antenna). Plan in `MESH_SCOPING.md`: probe-to-probe links and tomography; hub + 4 probes recommended |
| 38 | **Probe mirrors the hub's detection lights (requested 2026-10-08)** | **Built (protocol v3), host-tested, both firmwares build; not flashed or seen on hardware.** `HubSync` (now 10 bytes, sent at 4 Hz instead of 1 Hz) carries the hub's headline score and a state byte (worst presence state, calibrating, BOOT-gesture feedback, pairing open). The probe renders the same RGB colours as the hub (its green heartbeat when idle) and drives its own IO2 LED like the hub while the state is fresh (under 3 s); its pairing, hub-lost and OTA states keep priority. **Hub and probe must both be flashed: a v2 probe ignores v3 packets (shows hub-lost) and vice versa.** Probe IO2 gets a 3-blink boot test |
| 37 | **Battery-powered field test with on-flash data** | Decisions 2026-10-08: 1 h run, open garage, both boards on the floor, raw amplitudes, repartition, BOOT double-click calibration with auto-accept. Built and host-tested: field log (`fieldlog.cpp`, format `shared/field_format.h`, ~5.6 MB/h), download/decoder, `fieldcal.cpp`, `partitions_field.csv`. **Not run on hardware.** Open: battery monitoring (#25), dry run. See `FIELD_TEST.md` |
| 36 | **Probe placement and distance** | RSSI -13 dBm at 1 ft (near field), about -40 at a few metres on a clear line, -80 with the first placement (flapping link). Detector thresholds are specific to the calibrated placement: recalibrate after moving a node |

Findings behind #30 (from `hub/tools/csi_analyze.py`; one room, one person, probe at about -40 dBm): the old CSI feature (`csi_lltf_variance`) fell *below* the empty-room level for motion off the line and for sitting still, so the one-sided score missed them; gain-normalised temporal std of subcarrier amplitude separated moving people from an empty room (AUC 0.94-1.0); a mean RSSI drop of 2-5 dB marked a person standing on the line; the empty room was extremely stable (temporal std 0.025/0.026/0.025 over three rounds; RSSI exactly -40).

### 12.10 Garage field run (2026-10-08)

First logged run away from the basement: closed garage (~30 x 25 ft), boards on the floor 16-20 ft apart, hub on mains, probe on a 20,000 mAh bank (100 % before and after). **Shortened to 24.0 minutes** (10.1 min quiet baseline, 2 rounds of cross / near / far / stand / sit with 30-60 s blocks, a 3.5 min final empty). Data: `captures/garage1/` (gitignored), analysed with `hub/tools/field_decode.py` and `hub/tools/field_analyze.py`.

| Check | Result |
| :--- | :--- |
| Log integrity | 2.37 MB (1.65 KB/s as predicted), 1 boot record, 0 damaged bytes, 2,861 detector windows (99 % of expected), 35,749 amplitude records, 5,468 node records, 13 labels, 5 clock records, 212 events, no `node_down` |
| Quiet baseline (temporal std) | **0.0334 / 0.0334 / 0.0331** in the three empty blocks (basement: 0.0253): very stable, but higher than the basement. RSSI -40.2 dBm (sd 0.4) at 16-20 ft |
| False alarms | 1,783 empty windows 100 % CLEAR (1.4 % non-clear in the last block); 0 onsets in 10.1 min, 1 in 3.5 min |
| Detector (live firmware, thr 0.0392 = quiet max x 1.15) | cross 91 % detected (MOTION 61 %, ON_LINE 30 %); near 100 % (ON_LINE 43 %); **far (8 ft+ from the line) 95 %**; stand on line 100 % (ON_LINE 97 %, RSSI -43.7); **sit off line 36 %** |
| Brief crossings | Cross blocks were genuine quick crossings this time: shadowed 30-39 % of the time, bursts of median 1.5-2 s, longest 5 s. They are still detected (91 %), but mostly classified MOTION, not ON_LINE |
| EMF (GPIO 7 wires) | Hub p2p median 0 with spikes above 100 in ~8-10 % of samples at a steady rate in every condition (consistent with mains-adapter noise, unproven). Probe p2p 0-4. **No response to any activity at these distances; a touch/proximity test is still missing** (#32) |

| # | Item | Status |
| :--- | :--- | :--- |
| 39 | **Mid-run calibration not visible in the log** | **Resolved:** only the *hub* was selected in the dashboard's calibration dropdown, so the probe link was never recalibrated (the log was right). Safeguards added: the accept event now lists the nodes covered and says "probe link detector NOT recalibrated" when probes are paired but none was; the dashboard warns when a single node is chosen |
| 41 | **EMF wire near-field test** | **Run 2026-10-08:** hand at 30 cm no response; hand at 10 cm +100-200 (hub) / +95-145 (probe) counts; touch saturates the hub (65,535) and adds ~17,000 on the probe; repeatable. Hub raw level noisier (sigma 27.7 vs 5.4). The old p2p-based EMF score is blind to these slow shifts. See `EMF_TEST.md` |
| 45 | **Detector stuck in ON_LINE (anomaly log, 2026-10-08)** | `captures/anomaly1/`: the detector reported ON_LINE (score 70-85) for all 432 windows with no motion (tstd max 0.0372 vs threshold 0.03782): the baseline RSSI in force (-34.46 dB) was 1.7 dB above the link (-36.2), which stepped to -39.0 at t=2145 s. The hub was not power-cycled during the session (a BOOT record in the field log marks *logging started*, not a reboot); it ran an older build without any re-learning. The step is a gain-state switch, not a person (per-frame RSSI flips between -36 and -39 while the amplitude shape follows each frame's own RSSI; no motion). Fix on `calibration-v2`: onset classification (no motion = level step, re-based after 20 s steady; person = 20 min), upward re-learn after 60 s, baseline changes logged. Replay of the real windows: recovered 26 s after the start and 29 s after the step; the four recorded sessions replay unchanged (garage empty 100 % clear, stand_on 97 % on-line, near 100 %, cross 91 %). Not flashed to the field devices yet; to verify on hardware: quiet overnight log (count level steps per hour), slow approach onto the line, standing still 6 min (must stay detected), someone on the line at boot |
| 44 | **Calibration v2: robust threshold, real sensitivity, learned baseline** | Branch `calibration-v2`. Fixes the review findings: the Sensitivity dropdown did nothing for the presence detector (now low/normal/high = quiet level x 1.32 / 1.22 / 1.16); the threshold came from the single largest window (one spike inflated it 1.4x; now median + MAD, ignores a few disturbed windows); the RSSI baseline was fixed (now learned slowly); no post-accept check (now a 30 s check event); review table cluttered with EMF rows (now a plain summary, EMF rows collapsed). Details 13.10. Host-tested; replayed on the recordings (same detection, empty room still 100 % clear). Hub flashed from the branch; merged to master 2026-10-11 after independent testing |
| 43 | **Link-gated probe OTA ("arm and wait")** | **Built and verified on hardware 2026-10-08** (`shared/linkgate.h`, `hub/src/ota.cpp`, Settings "Probe updates" card, host test `test_linkgate.cpp`). Upload + arm need the OTA window (BOOT 8 s or serial `ota`). An armed update is sent only after the probe's link has held for the stable time: online, loss <= 2 %, RSSI >= **-50 dBm** (user choice), spread of 10 s means <= 4 dB; blockers: field log running, calibration in progress, no image, probe unpaired. Defaults 5 min, 2 h expiry; thresholds persisted (`og_*`). Live test: refused without the window; blocked at a gate above the link; counted 5..25 s after the gate was lowered; sent at 30 s; probe flashed and rejoined. Manual immediate send (`/api/ota/probe`) unchanged |
| 42 | **EMF touch events (separate from presence)** | Built (`shared/touch.h`, `hub/src/main.cpp`, host test `test_touch.cpp`, replay `replay_touch.cpp`): level shift of at least 5 sigma and 50 counts (hub 80), held 150 ms, self-adapting baseline frozen while active, 20 s warm-up after boot, clears after 1 s, 10 s cap. Raises event type `touch`, a toast, a TOUCH badge on the node card, and blue/white flashes for 3 s on the hub (and on the probe, once reflashed; sync state bit 6). Replay of the EMF test: both hands at 10 cm and both touches fire, nothing at 30 cm. Not on hardware yet |
| 40 | **Quiet-baseline vs basement** | Garage temporal std is ~32 % higher than the basement's, with the same RSSI (-40). Cause unknown (path length, multipath, gain/AGC, geometry). Thresholds therefore must be calibrated in place, as designed |

---

## 13. Backlog Features as Built

### 13.1 Event log, session recording and replay

- **Events:** the hub's `EventDetector` (`events.cpp`, host-tested) watches each frame and emits: vector changes (debounced over 3 frames, annotated "(hub radio?)" when a hub-local vector follows a client joining/leaving within 5 s), alert start/clear (score >= 71, hysteresis to 55), nodes joining/dropping, low battery, pairing window, calibration phases, drift prompts, tags, control changes and boot. A 512-event ring holds them; every event is also appended to `/events.tsv` in LittleFS (capped at 48 KB, newest half kept) and reloaded at boot. Events are pushed to dashboards immediately over `/ws` as `{"ev":{...}}`.
- **Session recording:** every 10th aggregation frame (2 Hz) is encoded (`session.cpp`, 9 + 14/node + 3/link bytes) into a 2 MB PSRAM ring (`ringlog.cpp`). It is volatile (#26).
- **Clock:** no RTC; the dashboard posts the phone's clock (`POST /api/time?ms=`). CSV wall times use it; rows recorded before it was set have an empty `wall_utc` (#24).
- **Replay:** the dashboard's *Session replay* panel loads `/api/session/info`, seeks with a slider and plays frames (1x/4x/16x) through the normal rendering path, with a REPLAY chip and a *Back to live* button. Audio follows the replayed score.
- **Export:** `GET /api/session.csv` (one row per node per frame: `t_ms,wall_utc,score,vector,vec_id,node,online,emf_raw,p2p,temp_c,d_temp_c,rssi,loss_pct,csi_var,vbat_v`) and `GET /api/events.csv` (`seq,t_ms,wall_utc,type,node,score,text`), both streamed in chunks.

### 13.2 False-positive controls

- **Tags:** `POST /api/tag?kind=hvac|phone|appliance|person|pet|other|note&dur=<s>&note=<text>` (dur 0 = a note only; `POST /api/tag/clear?kind=`). Up to 6 active, 4 h max, same kind replaces. Active tags ride along in every WebSocket frame and show on the score panel. They annotate only (#27).
- **Control probe:** `POST /api/control?node=N|none` (persisted). The control node is excluded from the score and vector counts. If it is disturbed (EMF or RF at the high threshold) while another node is, the vector is `COMMON_MODE` (#28). Place it away from the area of interest.
- **Hub radio:** a hub-local vector right after a WebSocket client joins/leaves is annotated in the log.

### 13.3 Phone features (dashboard only)

- **EVP recorder:** arms the phone microphone (raw audio: echo cancel, noise suppression and auto-gain off), records a 20 s clip manually or automatically when the score crosses the chosen level (60 s cooldown), stores up to 20 clips in the browser's IndexedDB with score/vector/time, and lists them with playback and save. Clips never leave the phone. No pre-roll yet. Needs a secure context (#21).
- **Alerts:** on rising crossings of STIRRING/MANIFESTING (20 s cooldown per level): an in-page toast, optional vibration (`navigator.vibrate`), optional system notification (secure context, #21) and optional tab-title flash when hidden.

### 13.4 OTA

- **Hub:** `POST /api/ota` (multipart `file` field; optional `md5` query) writes the inactive app slot and reboots after a full verified write. Accepted only while an OTA window is open (BOOT held 8 s, or `ota` on the serial console; 5 min) (#23). `GET /api/ota/status` reports the window, running partition (app0/app1), sketch MD5 and stored probe firmware. Example: `curl -F file=@.pio/build/hub/firmware.bin http://192.168.4.1/api/ota`.
- **Probes (inactive without `shared/ota_secret.h`, #22):** `POST /api/fw/probe` stores the probe image on the hub (LittleFS, MD5 recorded); `POST /api/ota/probe?node=N` sends an HMAC-authenticated `PKT_OTA` (size + MD5) to that probe. The probe verifies the tag and its hub's MAC, leaves ESP-NOW, joins the hub AP as a station, downloads `/fw/probe.bin`, checks size and MD5, writes the inactive slot, and reboots; any failure reboots into the current firmware.
- **Serial console:** `help | status | pair | ota | ota-close | time <epoch_ms>` on the hub's UART (the host can drive privileged actions without touching the open Wi-Fi).

### 13.5 Probe battery

`PROTO_VERSION` is now **3** (v3: `HubSync` gained `score` and `state` so probes mirror the hub lights, #38; v2: `ProbeTelemetry` gained `vbat_mv` (19 bytes) and `PKT_OTA` (40-byte `OtaCmd`) was added. Probe (`BATT_PIN`) and hub (`HUB_BATT_PIN`) read a divider on an ADC1 pin once per second (16-sample mean). The dashboard shows volts and a coarse percentage; below 3.3 V raises `FLAG_LOW_BATT` and a battery event. Disabled by default until wired (#25).

### 13.6 Calibration drift prompt

`drift.cpp` (host-tested) compares each calibrated node with its calibration: **antenna** (smoothed raw touch level off by more than max(5 %, 600 counts) for 10 s: calibration now also records the raw level), **noise** (live baseline above calibration + max(5 sigma, 5 counts) for 60 s), and **uncalibrated** (online 5 min with none). A one-shot drift event is logged and the dashboard shows a banner with *Calibrate this node* and *Dismiss*. Conditions clear after 30 s of agreement and re-arm. The control node is exempt; nothing fires during a capture. Calibration storage version is now 2 (a v1 store is discarded).

### 13.7 CSI presence detector (#30)

Per link (probe-to-hub, node id 1..N; node 0 is the AP-client pseudo-link and normally unused):
- `shared/csi_window.h`: every frame's 52 usable LLTF subcarrier amplitudes are divided by the frame's mean (cancels gain), kept in a 50-frame ring; every 25 frames (about 0.5 s at 50 fps) the per-subcarrier standard deviation over the window is averaged: **temporal std** (an empty room reads ~0.025). The window's mean RSSI is returned with it. Allocated per link in PSRAM (5.3 KB).
- `shared/presence.h`: **motion** = temporal std above the calibrated threshold; **shadow** = mean RSSI at least 1.5 dB under the calibrated RSSI. Each is voted 2-of-3 windows. State: `ON_LINE` (shadow; `moving` if motion too) > `MOTION` > `CLEAR`; `OFF` until calibrated. A state is held 3 s after the last active window (4 s until 2026-10-08). Score: MOTION 40-65 (by how far over the threshold), ON_LINE 60-85 (by dB below baseline).
- **Calibration** reuses the existing Calibrate flow (30 s, quiet area). **Superseded by 13.10 (branch `calibration-v2`): median-based threshold, sensitivity setting, slow learning, drift flag.** The original rule was threshold = (largest window in the capture) x 1.15 and a fixed RSSI baseline.
- **Scoring:** a calibrated link's CSI channel is the presence score instead of the old baseline score; the headline score is at least the best presence score; new vectors `ROOM_MOTION_<id>` and `LINE_BREAK_<id>` when nothing else (EMF, thermal) fired. The WebSocket carries `pr`, `ps`, `mv` per link. The dashboard's red glow uses `ps`.
- **Limits (from the data):** still people off the line are mostly invisible; standing on the line reads as ON_LINE moving about half the time (sway); thresholds come from one placement; position cannot be inferred from one link (#35).

### 13.8 Capture tooling

`csicap.cpp`/`csicap.h` (hub): `csicap on` over the serial console switches the UART to 921600 baud and streams `CSI,<ms>,<node|-1>,<mac>,<rssi>,<noise>,<sig>,<rate>,<256 hex>` per frame (every frame the radio delivers, including other devices at node -1) and `LBL,<ms>,<name>` lines; `csicap off` (or a 20 min safety stop) restores 115200. `POST /api/csi/label?l=<name>` and `GET /api/csi/status` back the dashboard's label panel (shown only while a capture runs). Frames are queued from the Wi-Fi callback and printed by a Core 1 task, so acquisition is not slowed. Host side: `hub/tools/csi_capture.py`, `csi_analyze.py`, `csi_replay.py`; the firmware's own code is replayed by `hub/host_test/replay_presence.cpp`.

### 13.9 EMF touch events (#42)

`shared/touch.h` watches each node's raw touch level (`emf_raw`) against a baseline that adapts with an 8 s time constant only while quiet. A shift of at least 4 sigma (sigma from a running mean absolute deviation) and at least 50 counts (80 for the hub), accumulated over 500 ms with a leaky counter, starts an event (it was 5 sigma / 150 ms until the Pi's own Wi-Fi bursts, 1 ft from the hub wire, caused 3 false events in 150 s; with this change 0 in 3 min of the same traffic); it ends after 1 s back within half the threshold, then a 1 s lockout; a shift that never goes away is re-baselined after 10 s; the first 20 s after boot only learn. The hub writes an `EV_TOUCH` event ("touch hub wire +182"), sets `touch` on the node (WebSocket `tc`, dashboard badge and toast, field-log event) and flashes the RGB LED alternately blue and white at 4 Hz with IO2 lit on the white phase for 3 s; the state byte of `HubSync` (bit 6) makes the probe do the same. It is not part of the presence score, the headline score or the vectors. Measured basis: `EMF_TEST.md`.

### 13.10 Calibration v2 (#44, branch `calibration-v2`)

- **Capture (30 s):** about 60 windows of temporal std plus the mean RSSI. **Quiet level q0 = median**; sigma = 1.4826 x MAD. **Threshold = q0 x (1 + g)**, g by sensitivity (low 0.32, normal 0.22, high 0.16), but never closer than 8 sigma to q0 and never below 0.015. **Baseline RSSI = capture mean.** A warning (the BOOT gesture then refuses to accept) when more than 5 % of the windows exceed 1.35 x median.
- **Why median:** with one disturbed window in 60 the old "largest x 1.15" threshold rose 1.44x; the median-based one stays put and tolerates up to about 20 % disturbed windows. On the recordings the threshold is similar (normal 1.22 x q0 vs 1.20) and detection is unchanged.
- **Learning:** from CLEAR windows only, after 30 s of continuous quiet, with a 30 min time constant: the quiet level q follows slow changes (HVAC, temperature, furniture) and the baseline RSSI r likewise; threshold = (calibrated ratio) x q; shadow at r - 1.5 dB. Limits q in [0.7, 1.6] x q0, r within 4 dB of the calibration: hitting a limit, or being active for 10 min straight without a shadow (the room changed), raises `drift` (event, dashboard line "recalibrate"). Nothing is learned while anything is detected, so a person standing on the line is never absorbed. The learned values are saved to flash at most every 10 min and only when they moved (store `hubpres` v2: **older calibrations are discarded, recalibrate once**).
- **Self-recovery from a wrong baseline (12.10b; reworked after an independent Opus review):** the learner only runs in CLEAR, so a baseline RSSI that was wrong or went stale kept the detector in ON_LINE for ever. The link level also steps 1-3 dB on its own on this hardware (a receiver/transmitter gain-state switch; seen in the garage, in the basement and in the anomaly log). The discriminator is that **a person can only reach the line by moving**: each shadow onset is classified by whether motion (2-of-3 motion votes) accompanies it, within 15 s before or 5 s after it began. **No motion = a level step:** once the shadow is steady (mean absolute deviation <= 0.8 dB) for 20 s the baseline RSSI jumps to the new level. **Motion = a person:** kept as long as they stay, absorbed only after 20 min perfectly still. RSSI steadily above the baseline for 60 s re-learns upward at once. Each change logs "link N baseline signal re-learned: A to B dB" and is written to the field log; the limit is +-6 dB from the calibration. Blind spots: a level step while someone is moving looks like a person (20 min or recalibrate); a person already standing still on the line when the hub boots is taken for a level step after 20 s. A "shape of the amplitude profile" shadow feature was tested and rejected: a gain step changes the shape (0.003 to 0.033) about as much as a person (0.02-0.10) and quiet-room drift is person-sized (85-100 % false alarms).
- **Check:** 30 s after accepting, an event says whether the detector stayed quiet ("ignore if you were moving").
- **UI:** the review shows one line per probe (quiet level, threshold and its ratio, baseline, windows, warning); the EMF table is in a collapsed "advanced" section; the calibration panel shows the values in force and whether they drifted.
