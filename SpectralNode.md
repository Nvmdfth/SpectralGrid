# Technical Specification & Product Requirements Document (PRD)

**Project:** Project SpectralNode — Autonomous ESP32-S3 Multi-Channel Paranormal Telemetry Unit
**Target Hardware:** ESP32-S3-DevKitC-1 (16MB Quad-SPI Flash, 8MB Octal PSRAM, Dual-Core Xtensa LX7 @ 240MHz)
**Toolchain / Runtime:** VS Code / PlatformIO (`espressif32` platform), Arduino Framework / ESP-IDF Components
**Primary Interface:** Asynchronous Web Server + WebSocket + Web Audio API (Standalone SoftAP Captive Portal)

---

## 1. System Overview & Core Architecture

SpectralNode turns an unadorned ESP32-S3 development board into an isolated, battery-powered paranormal sensing station. It exploits the physical characteristics of the onboard radio frequency (RF) subsystem, capacitive charge/discharge peripherals, and internal die telemetry to track localized environmental anomalies without requiring specialized external sensor daughterboards.

The firmware segregates real-time deterministic physical sampling and network communications across the two Xtensa LX7 cores to eliminate jitter, prevent Wi-Fi RF beacon bursts from corrupting analog acquisition, and ensure responsive WebSockets.

```text
CORE 0: Real-Time Hardware Acquisition Loop
┌─────────────────────────────────────────────────────────┐
│ • Touch/EMF High-Z Float Antenna (GPIO 7) @ 200 Hz      │
│ • Internal Die Thermal Derivative (dT/dt) @ 10 Hz       │
│ • Wi-Fi PHY CCA Noise Floor / RSSI Telemetry            │
│ • 802.11 CSI Subcarrier Frame Capture (esp-csi hook)    │
└────────────────────────────┬────────────────────────────┘
                             │ FreeRTOS Ring Buffer / Queue
                             ▼
CORE 1: DSP, Anomaly Scoring & Web Services
┌─────────────────────────────────────────────────────────┐
│ • Peak-to-Peak Variance & Rolling Noise Floor Baseline  │
│ • Composite Anomaly Index Calculation (0–100%)          │
│ • LittleFS Static Asset Delivery (HTML5/Canvas SPA)     │
│ • ESPAsyncWebServer + Asynchronous WebSocket Broadcast  │
│ • Captive Portal DNS Redirection (DNSServer)            │
└────────────────────────────┬────────────────────────────┘
                             │ 802.11 SoftAP ("SPECTRAL_NODE_S3")
                             ▼
CLIENT BROWSER (Mobile Smartphone / Tablet)
┌─────────────────────────────────────────────────────────┐
│ • Real-Time CRT Phosphor Radar Sweep (HTML5 Canvas)     │
│ • Tri-Channel Rolling Strip-Chart (EMF, Temp, CSI Var)  │
│ • Dynamic Synthetic Geiger Counter Audio (Web Audio API)│
└─────────────────────────────────────────────────────────┘
```

---

## 2. Hardware Specification & Pin Assignments

| Subsystem | Hardware Pin / Periph | Circuit Topology / Physical Requirement | Operational Purpose |
| :--- | :--- | :--- | :--- |
| **EMF Antenna** | **GPIO 7** (Touch Channel 7) | 10 cm (4-inch) 22 AWG solid-core jumper wire soldered/inserted into header. Free floating; unshielded. | Electrostatic field / AC leakage sensor. Measures charge/discharge cycle shifts induced by stray EM fields. |
| **Status Indication** | **GPIO 48** (Onboard WS2812 RGB LED) | Hardwired onboard addressable RGB LED. | Real-time optical anomaly meter (Green = Baseline Quiet, Amber = Moderate Drift, Strobe Red = Critical Anomaly). |
| **Thermal Sensing** | Internal Die Sensor | On-chip silicon junction. API: `temperature_sensor_handle_t` (ESP-IDF 5.x). | Cold spot tracking. Tracks negative thermal slopes (-dT/dt). |
| **RF / CSI Sensing** | Internal 2.4 GHz RF Front-End | PCB trace antenna on ESP32-S3 module. | Tracks physical Fresnel zone crossing and multipath signal scattering via `esp_wifi_set_csi_rx_cb`. |
| **Power Source** | **5V / VIN** & **GND** pins | Single-cell LiPo/18650 through TP4056 OR regulated 5V USB battery bank. | Decoupled DC power. Eliminates 60 Hz ground loops and smartphone switching regulator noise. |

---

## 3. Firmware Architecture & Component Requirements

### 3.1 PlatformIO Environment Definition (`platformio.ini`)

```ini
[env:esp32-s3-devkitc-1]
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

lib_deps =
    https://github.com/me-no-dev/ESPAsyncWebServer.git
    https://github.com/me-no-dev/AsyncTCP.git
    bblanchon/ArduinoJson @ ^6.21.3
    fastled/FastLED @ ^3.6.0
```

### 3.2 Core 0: Acquisition Engine (`SensorTask`)

1. **EMF Sampling Loop:**
   - Pin: GPIO 7 configured via `touchRead(7)`.
   - Sample Window: 128 samples per batch sampled every 250 microseconds.
   - Calculations: Compute rolling minimum, maximum, and peak-to-peak delta ($V_{\text{max}} - V_{\text{min}}$).
2. **Die Temperature Slope:**
   - Initialize using modern driver API (`driver/temperature_sensor.h`).
   - Poll at 10 Hz. Calculate 30-second moving average baseline.
   - Flag thermal anomalies if temperature drop exceeds -0.25°C over a 3-second rolling window.
3. **CSI & RF Disturbance Metric:**
   - Enable CSI reception using `esp_wifi_set_csi(true)`.
   - In `wifi_csi_rx_cb`, compute subcarrier amplitude variance across usable data subcarriers ($k \in [6..58]$).
4. **Inter-Core Telemetry Pipe:**
   - Encapsulate sampled values in a C-struct:

     ```c
     typedef struct {
         uint16_t emf_raw;
         uint16_t emf_p2p;
         float temp_c;
         float temp_derivative;
         float csi_variance;
         uint8_t composite_score;
     } TelemetryFrame;
     ```

   - Push frames to a FreeRTOS `QueueHandle_t` configured with a depth of 32 frames (drop oldest on overflow).

### 3.3 Core 1: Network & Supervisory Stack

1. **Wi-Fi SoftAP Configuration:**
   - SSID: `SPECTRAL_NODE_S3` (Open or WPA2 `paranormal2026`).
   - Channel: 6 (Fixed).
   - TX Power: Throttled to `WIFI_POWER_13dBm` via `WiFi.setTxPower()` to minimize local self-induced RF bleed into the high-impedance antenna pin.
2. **Captive Portal DNS Service:**
   - Run a lightweight `DNSServer` on port 53 mapping `*` to `192.168.4.1` to force auto-prompt navigation upon mobile client connection.
3. **ESPAsyncWebServer & WebSockets:**
   - Path: `/ws`
   - WebSocket Broadcast Rate: Decimated to 20 Hz (every 50 ms) to avoid browser DOM thrashing.
   - Serialization: Minified JSON format:
     `{"emf":142,"p2p":48,"temp":24.3,"d_temp":-0.02,"csi":12.8,"score":78}`
4. **Visual Indicator Engine (GPIO 48 WS2812B):**
   - Map `composite_score` (0–100%) to a color gradient:
     - 0% to 30%: Solid Low-Intensity Green (`#002200`).
     - 31% to 70%: Breathing Yellow/Amber (`#443300`).
     - 71% to 100%: Rapid Red Strobe (`#FF0000` at 10 Hz).

---

## 4. Single-Page Application (SPA) Web Dashboard

The web dashboard is delivered directly from ESP32-S3 Flash/PROGMEM as an inline GZIP-compressed single-page application.

### 4.1 UI Layout & Design Tokens

- **Theme:** CRT Vector Phosphor Aesthetic. Pitch black background (`#050805`), high-contrast phosphor green accents (`#00FF66`), critical alerts in warning amber (`#FFB000`) and hazard red (`#FF3333`).
- **Viewport:** Responsive layout locked to mobile viewport (`width=device-width, initial-scale=1.0, user-scalable=no`).

### 4.2 Frontend Functional Components

1. **Phosphor CRT Radar Canvas:**
   - A 280 x 280 px circular display with rotating radar sweep ray.
   - Phosphor persistence effect achieved by clearing the canvas with semi-transparent fills (`rgba(0, 8, 0, 0.08)`).
   - CSI subcarrier disturbance projects dynamic blips along the sweep radius: higher CSI variance maps to larger, scattered anomalous blips.
2. **Strip-Chart Telemetry Recorder:**
   - 100-point scrolling canvas tracking `emf_p2p` and `temp_derivative` in real time.
3. **Synthetic Audio Synthesizer (Zero-Hardware Web Audio API):**
   - Audio generated entirely on the smartphone/tablet client via `AudioContext`.
   - **Geiger Mode:** When `emf_p2p` exceeds the calibrated noise baseline, fire short randomized burst clicks using a custom `AudioBuffer` filled with white noise passed through a high-pass filter ($f_c = 1.2\text{ kHz}$). Click rate scales linearly with anomaly intensity.
   - **Spirit Drone Mode:** A low-frequency carrier oscillator (40 Hz square wave) modulated through an active resonant bandpass filter, with center frequency tied to CSI RF variance.

---

## 5. Composite Anomaly Calculation Engine

The firmware computes an aggregated Paranormal Perturbation Index ($P \in [0, 100]$):

$$P = \min\left(100, \; w_1 \cdot \widehat{\text{EMF}}_{\text{p2p}} + w_2 \cdot \widehat{\Delta T}_{\text{neg}} + w_3 \cdot \widehat{\sigma}^2_{\text{CSI}}\right)$$

- **Weights:** $w_1 = 0.45$ (EMF field disruption), $w_2 = 0.25$ (Thermal drop), $w_3 = 0.30$ (RF multipath scatter).
- **Normalization ($\widehat{X}$):** Each channel normalizes current readings against a 10-second running exponential moving average (EMA) baseline:

  $$\mu_t = \alpha \cdot X_t + (1 - \alpha) \cdot \mu_{t-1}, \quad \text{with } \alpha = 0.02$$

  Only positive deviations exceeding $2.5\sigma$ above the noise floor contribute to the index.

---

## 6. Verification Checklist

1. **Power Isolation:** Power board via 5V battery bank; verify no 60 Hz harmonics exist in touch readings when completely isolated from AC mains.
2. **Capacitive Pin Test:** Approach GPIO 7 wire with an energized power cord; observe immediate spike in `emf_p2p` and trigger of Geiger audio clicks.
3. **CSI Responsiveness:** Walk through the line of sight of the ESP32-S3 antenna; confirm real-time scattering blips on the CRT radar canvas.
4. **Core Separation:** Verify in serial monitor that Core 0 sampling runs at >= 195 Hz with zero dropped frames during active WebSocket streaming on Core 1.
