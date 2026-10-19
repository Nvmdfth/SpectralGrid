# Technical Specification & Product Requirements Document (PRD)

**Project:** Project SpectralMultiNode — Distributed Dual-Node ESP32-S3 Bistatic RF Barrier & Paranormal Telemetry Grid  
**Target Hardware:** 2× ESP32-S3-DevKitC-1 (16MB Flash, 8MB OPI PSRAM, Dual-Core Xtensa LX7 @ 240MHz, Onboard WS2812 RGB LED on GPIO 48)  
**Toolchain / Runtime:** VS Code / PlatformIO (`espressif32` platform), Arduino Framework with ESP-IDF Hardware Halting Components  
**Architecture:** Asymmetric Distributed Mesh (Node A: Remote Sensor/RF Beacon ──[ESP-NOW 2.4 GHz]──> Node B: CSI Receiver, Aggregator & Web Gateway)  
**Primary UI:** Standalone SoftAP Captive Portal + Real-Time HTML5/Canvas CRT Phosphor Dashboard + Web Audio API Engine  

---

## 1. System Overview & Dual-Node Advantage

Project SpectralMultiNode leverages two paired ESP32-S3 development boards to overcome the fundamental physical limitations of single-node RF and EMF sensing:

1. **Bistatic RF Fresnel Barrier:** Node A transmits high-frequency ESP-NOW sounding frames (50 Hz) on a fixed 2.4 GHz Wi-Fi channel. Node B captures raw Channel State Information (CSI) for every arriving frame. This creates an invisible, volumetric RF "tripwire" across the first Fresnel zone between the two devices. Any physical mass entering the corridor causes instantaneous multipath phase and amplitude disruptions.
2. **RF Self-Interference Elimination:** On Node A (the remote probe), heavy TCP/IP stacks and Wi-Fi beaconing are disabled. Node A operates in pure ESP-NOW transmission mode, allowing its high-impedance capacitive EMF antenna on GPIO 7 to operate without local Wi-Fi transmission bleed.
3. **Differential Anomaly Vectoring:** Both nodes sample localized EMF and thermal shifts independently. By comparing telemetry from Node A and Node B alongside link CSI fluctuations, the system vectors whether an anomaly is localized to Node A, localized to Node B, or a physical entity crossing between them.


[ NODE A: Remote Scout ] [ NODE B: Base Station / Gateway ] ┌──────────────────────────────────────────┐ ┌──────────────────────────────────────────┐ │ • GPIO 7: High-Z Float Antenna (EMF A) │ │ • GPIO 7: Local Float Antenna (EMF B) │ │ • Internal Die Thermal Sensor (Temp A) │ │ • Internal Die Thermal Sensor (Temp B) │ │ • GPIO 48: WS2812 Status LED │ │ • 802.11 CSI Hardware Hook (Link CSI) │ │ • Core 0: 200 Hz ADC/Touch Sampler │ │ • Core 0: CSI Ingest & ESP-NOW RX ISR │ │ • Core 1: ESP-NOW Beacon Engine @ 50 Hz │ │ • Core 1: Aggregator, Anomaly Engine, │ └────────────────────┬─────────────────────┘ │ SoftAP, WebSockets & LittleFS │ │ └────────────────────┬─────────────────────┘ │ 2.4 GHz ESP-NOW Sounding Frames │ └─────────────────────────────────────────────────────┘ │ 802.11 SoftAP ("SPECTRAL_GRID_S3") ▼ [ CLIENT BROWSER (Phone / Tablet) ] ┌──────────────────────────────────────────┐ │ • Phosphor CRT Bistatic Barrier Radar │ │ • Dual-Zone Differential Strip Charts │ │ • Zero-Hardware Geiger Audio Synthesizer │ └──────────────────────────────────────────┘
---

## 2. Hardware Specification & Pin Mapping

Both nodes run on identical ESP32-S3-DevKitC-1 hardware, configured by firmware into asymmetric roles:

| Component | Hardware Resource | Node A (Remote Probe) Function | Node B (Base Gateway) Function |
| :--- | :--- | :--- | :--- |
| **EMF Antenna** | **GPIO 7** (Touch Ch 7) | 10 cm unshielded 22 AWG solid-core wire. Samples Probe A electrostatic field. | 10 cm unshielded 22 AWG solid-core wire. Samples Base B electrostatic field. |
| **Visual Indicator** | **GPIO 48** (WS2812 RGB LED) | Optical status for Probe A anomaly index & ESP-NOW transmission heartbeat. | Optical status for Composite Grid Anomaly Score (Green $\to$ Yellow $\to$ Strobe Red). |
| **Internal Temp** | Internal Silicon Junction | Logs Probe A thermal gradient ($-\frac{dT_A}{dt}$). | Logs Base B thermal gradient ($-\frac{dT_B}{dt}$). |
| **RF Transceiver** | 2.4 GHz PCB Trace Antenna | Emits 50 Hz ESP-NOW packets with custom payload on fixed Channel 6. | Captures 64-subcarrier raw CSI from Node A packets; hosts 802.11 SoftAP. |
| **Power Input** | **VIN (5V) & GND** | Isolated 1S LiPo/18650 or dedicated USB battery bank. | Isolated USB battery bank (or phone USB OTG port with capacitive filtering). |

---

## 3. PlatformIO Project Configuration (`platformio.ini`)

Both targets can be maintained in a single PlatformIO project using environments:

```ini
[platformio]
default_envs = node_b_gateway

[env]
platform = espressif32 @ ~6.5.0
board = esp32-s3-devkitc-1
framework = arduino
board_build.mcu = esp32s3
board_build.f_cpu = 240000000L
board_build.flash_mode = qio
board_build.flash_size = 16MB
board_build.psram_type = opi
board_build.partitions = default_16MB.csv

build_flags = 
   -DBOARD_HAS_PSRAM
   -DARDUINO_USB_CDC_ON_BOOT=1
   -DARDUINO_USB_MODE=1
   -DCORE_DEBUG_LEVEL=1

[env:node_a_probe]
build_flags = 
   ${env.build_flags}
   -DNODE_ROLE_PROBE=1
lib_deps =
   fastled/FastLED @ ^3.6.0

[env:node_b_gateway]
build_flags = 
   ${env.build_flags}
   -DNODE_ROLE_GATEWAY=1
lib_deps =
   fastled/FastLED @ ^3.6.0
   [https://github.com/me-no-dev/ESPAsyncWebServer.git](https://github.com/me-no-dev/ESPAsyncWebServer.git)
   [https://github.com/me-no-dev/AsyncTCP.git](https://github.com/me-no-dev/AsyncTCP.git)
   bblanchon/ArduinoJson @ ^6.21.3

4. Node A (Remote Probe) Firmware Specification
Node A is a low-latency, deterministic sampling beacon.
4.1 Acquisition & Transmission Architecture
* Core 0 Loop (ProbeSampleTask):
   * Reads touchRead(7) across a 64-sample burst every 5 ms (200 Hz effective rate).
   * Calculates peak-to-peak delta: \Delta V_{\text{EMF\_A}} = V_{\max} - V_{\min}.
   * Samples internal die temperature at 10 Hz; calculates negative thermal drop.
   * Populates the inter-node telemetry frame.
* Core 1 Loop (BeaconTask):
   * Broadcasts telemetry frames via ESP-NOW at 50 Hz (20 ms interval) on Channel 6.
   * Drives onboard WS2812 LED on GPIO 48: pulse green on packet transmit; flash yellow/red if local EMF exceeds baseline.
4.2 Inter-Node Telemetry Packet Struct
typedef struct __attribute__((packed)) {
 uint32_t sequence_id;      // Monotonically increasing frame counter
 uint16_t emf_raw;          // Instantaneous touch reading
 uint16_t emf_p2p;          // Peak-to-peak amplitude delta
 int16_t  temp_centi_c;     // Temperature in Celsius * 100
 int16_t  temp_delta;       // Rolling 3-second temperature slope * 100
} ProbeTelemetryPacket;

5. Node B (Gateway & Aggregator) Firmware Specification
Node B acts as the network coordinator, CSI demodulator, anomaly scoring engine, and web server.
5.1 Radio Concurrency (ESP-NOW + SoftAP)
* Wi-Fi Mode: WIFI_AP_STA locked to Channel 6.
* ESP-NOW is initialized on the STA interface to receive frames from Node A's MAC address.
* SoftAP runs on the AP interface (SSID: SPECTRAL_GRID_S3, Channel: 6, Auth: Open or WPA2).
* Wi-Fi TX Power throttled to 13 dBm to prevent swamping the local GPIO 7 probe.
5.2 CSI Extraction Pipeline
* CSI callback registered via esp_wifi_set_csi_rx_cb().
* Filter: Discard all packets whose source MAC does not match Node A.
* For each arriving 802.11n HT20 frame (64 subcarriers, 128 bytes of interleaved Q/I):
   1. Extract signed 8-bit pairs (Q_k, I_k) for usable subcarriers (k \in [6..58]).
   2. Compute magnitude: A_k = \sqrt{I_k^2 + Q_k^2}.
   3. Calculate instantaneous subcarrier variance: \sigma^2_{\text{CSI}} = \frac{1}{N}\sum_{k=1}^N (A_k - \mu)^2
   4. Push combined tuple {ProbeTelemetryPacket, Local_EMF_B, Local_Temp_B, CSI_Variance} into a FreeRTOS ring buffer.
5.3 Composite Grid Anomaly Engine
The aggregation engine computes a centralized Anomaly Score (S_{\text{grid}} \in [0, 100]):
S_{\text{grid}} = \min\left(100, \; w_{\text{csi}} \cdot \widehat{\sigma}^2_{\text{CSI}} + w_{\text{emfA}} \cdot \widehat{\Delta V}_{\text{A}} + w_{\text{emfB}} \cdot \widehat{\Delta V}_{\text{B}} + w_{\text{temp}} \cdot \widehat{\Delta T}_{\max}\right)
* Weights: w_{\text{csi}} = 0.40 (Physical RF barrier breach), w_{\text{emfA}} = 0.25, w_{\text{emfB}} = 0.25, w_{\text{temp}} = 0.10.
* Differential Classification Logic:
   * Barrier Breach (Motion/Displacement): \sigma^2_{\text{CSI}} > \text{Threshold} while \Delta V_A and \Delta V_B remain low.
   * Localized Node A Anomaly: \Delta V_A spikes; \Delta V_B quiet; CSI variance mild.
   * Localized Node B Anomaly: \Delta V_B spikes; \Delta V_A quiet; CSI variance mild.
   * Full-Spectrum Grid Anomaly: Simultaneous spikes on both EMF probes and CSI link (triggers high-priority alarm).
5.4 Async Web Server & WebSocket Engine
* WebSocket Path: /ws
* Broadcast Decimation: 20 Hz transmission rate to connected clients.
* Payload Format: Minified JSON string:
{
 "seq": 10423,
 "csi_var": 14.82,
 "node_a": {"emf": 32, "temp": 22.4, "d_temp": -0.01},
 "node_b": {"emf": 12, "temp": 23.1, "d_temp": 0.00},
 "score": 68,
 "vector": "BARRIER_BREACH"
}

6. Single-Page Application (SPA) Web Dashboard
The frontend is served from Node B's flash memory via ESPAsyncWebServer and rendered on a smartphone or tablet browser.
6.1 UI Design Tokens
   * Aesthetic: Dual-Beam Phosphor Military Vector Display.
   * Colors: Void Black (#050805), Phosphor Terminal Green (#00FF66), Cyan Vector (#00E5FF), Alert Amber (#FFB000), Redline (#FF2222).
   * Performance: Fixed viewport, hardware-accelerated 2D Canvas contexts, zero external CDN dependencies.
6.2 Display Modules
   1. Bistatic Barrier Visualizer (Top Canvas):
   * Displays Node A and Node B as circular anchor nodes on opposite sides of a 2D floor grid.
   * Renders the connecting RF line-of-sight and 1st Fresnel zone ellipse.
   * Baseline state: Clean, coherent harmonic wave ripples along the link beam.
   * Perturbation state: High CSI variance causes the beam to distort, shatter, and scatter particle sparks into the room grid based on interference intensity.
   2. Dual-Zone Differential Strip Chart (Middle Module):
   * Two stacked rolling line plots comparing Node A EMF (Cyan) vs. Node B EMF (Green) over a rolling 10-second window.
   * Visual differential indicator highlights whether electromagnetic activity is migrating from A to B or vice-versa.
   3. Web Audio Soundscape (Bottom Module):
   * Zero-hardware client-side synthesis via HTML5 AudioContext.
   * Differential Geiger Audio: Generates clicks whose pitch differs depending on origin (high-pitched click for Node A spikes, low-pitched click for Node B spikes).
   * Bistatic Sub-Bass Resonator: A 45 Hz sine oscillator whose phase distortion and harmonic overtones track the inter-node CSI variance in real time.
7. Verification & Calibration Protocol
   1. MAC Pairing: Note the STA MAC address of Node A; hardcode or dynamically pair it in Node B's CSI MAC filter to prevent logging packets from third-party routers.
   2. Channel Alignment: Verify that both Node A (ESP-NOW) and Node B (SoftAP + ESP-NOW) are locked strictly to Wi-Fi Channel 6; channel mismatches will drop both ESP-NOW frames and CSI callbacks.
   3. RF Barrier Sensitivity Test:
   * Place Node A and Node B 4 meters apart at waist height.
   * Confirm the web dashboard displays a calm, coherent RF beam.
   * Walk across the physical threshold between the two nodes; verify that CSI variance spikes immediately and triggers the BARRIER_BREACH state while both local EMF probes stay below threshold.
   4. EMF Differential Isolation Test:
   * Bring an unshielded AC power cable near Node A's antenna; confirm that Node A EMF spikes to >80\%, Node B EMF remains <10\%, and the system vectors the event as LOCAL_PROBE_A.