# Field test plan: battery-powered hub and probe, data kept on flash

Status: **F1 built (host-tested, builds; not flashed or run on hardware at the time of writing); battery monitoring, the dry run and the field run are not done.** Written 2026-10-08. Claims marked **[verify]** are estimates or hypotheses.

As built: `shared/field_format.h` (record framing), `hub/src/fieldlog.cpp` (writer), `hub/src/fieldcal.cpp` (BOOT double-click calibration), `hub/partitions_field.csv`,
dashboard "Field log" panel, `GET /api/field/log|status`, `POST /api/field/start|stop|erase?confirm=1`, console `field on|off|status`, `hub/tools/field_decode.py`.
Raw amplitudes are logged at ~25 frames/s per link (every 2nd frame): about 1.6 KB/s including features, so **about 5.6 MB per hour**, about 1.8 h in the ~10 MB data partition.

## 1. Goal

Run the hub and one probe on batteries in an open space, calibrate and detect people there, and keep the data on the
boards' flash so it can be extracted afterwards and evaluated offline (the same way `captures/session*.csv` were).

## 2. What already works, and what is missing

| Need | Today |
| :--- | :--- |
| Hub as a standalone access point (no Pi, no Wi-Fi network) | Yes: the phone joins `SPECTRAL_GRID` and uses the dashboard |
| Calibration in the field | Yes, from the phone (stored in flash, survives power loss) |
| Detector, held score, LEDs | Yes (the presence detector is flashed but not yet walk-tested, §12, #30) |
| Ground-truth labels from a phone | Only while a *serial* capture is running (`csicap`); they go to the Pi, not to flash |
| Data persisted on flash | **Events only** (48 KB). The session recording is a volatile 2 MB PSRAM ring that is lost at power-off (#26). The detector's windows and the EMF/thermal channels are not stored |
| Extraction after the fact | Session/Events CSV from the volatile ring only. No download of a persisted feature log |
| Battery power | A probe already ran on a battery. The hub has not. **Battery voltage is not monitored** (hub `HUB_BATT_PIN -1`, probe divider unwired, #25) |
| Wall clock | None (no RTC). The phone posts its clock when it connects; events before that carry uptime only |

## 3. Proposed design

**3.1 Field log (new, on the hub).** Append-only binary log in LittleFS, written from the existing 2 Hz aggregation loop:
- per link per 0.5 s window: uptime ms, node, temporal std, mean RSSI, presence state, score (~12 bytes);
- per node at 2 Hz: EMF p2p, temperature, battery mV, online flag (~8 bytes);
- events and phone **labels** (the dashboard's label panel, made to work without a serial capture), clock-sync records, boot records
  (firmware version, calibration values), and CRC-protected records so a power loss costs at most the last buffer.
- RAM buffer of ~30 s flushed to flash; LittleFS wear levelling; about 80 bytes/s for hub + 1 probe = ~290 KB/hour.

**3.2 Extraction.** `GET /api/field/log` (chunked download, any phone/PC on the AP), `GET /api/field/status` (bytes used, hours left, number of
boots), `DELETE`/console command to wipe. A small `hub/tools/field_decode.py` converts it to CSV compatible with `csi_analyze.py`.

**3.3 Optional "raw-lite" CSI.** The 52 subcarrier amplitudes as bytes at ~10 fps (~520 B/s, ~1.9 MB/hour) so the *features* can be redesigned
after the field test. Full 128-byte frames at 50 fps are **23 MB/hour** and do not fit; use serial capture on the bench for that.

**3.4 Storage budget.** The data partition is 3.375 MB (`default_16MB.csv`) and already holds the stored probe firmware (~1.2 MB). **Decision: repartition**
(custom table in section 6): ~10 MB of data, about 30 h of features or ~2 h at 25 fps raw amplitudes. Needs one USB flash of the hub and erases stored data.

**3.5 Power.** Estimate **[verify with a USB power meter]**: hub (AP + CSI at 240 MHz) about 0.15-0.25 A at 5 V, i.e. roughly a day on a 10,000 mAh power bank;
probe less. Pick a power bank that does not switch off at low current, or add a dummy load. Wire the hub's `HUB_BATT_PIN` or monitor
the pack, and wire the probe's divider (#25), so low battery is logged.

**3.6 Phone-free calibration (optional).** A BOOT-button gesture (e.g. hold 5 s) that starts the (now 30 s) calibration with LED feedback,
for use when the phone is not at hand. Auto-calibration at boot is not recommended: someone walking near the boards ruins it.

## 4. What will be different outdoors (set expectations)

- **Fewer reflections.** The detector's room-wide motion sensitivity came from multipath in a basement. In an open field expect good detection on
  or near the line between the nodes and much weaker detection of movement away from it **[verify]**.
- **New false alarms:** wind-blown vegetation, rain, passing vehicles and animals; temperature drift (the die sensor tracks the chip, not the air).
- **Placement:** keep the antennas at a consistent height and orientation (waist to chest); the thresholds are tied to the calibrated placement
  (#36); recalibrate when anything moves. Enclosures change the antenna: test the same enclosure in the basement first.
- **Longer range:** RSSI at 5-10 m line of sight is probably -50 to -65 dBm **[verify]**; the basement's -80 case flapped, so test the outdoor
  distance before committing.

## 5. Phases

| Phase | Work | Exit criterion |
| :--- | :--- | :--- |
| F0 | Calibrate and walk-test the detector in the basement (§12, #30) | Detector behaves as the replay predicted |
| F1 | Field log + download endpoint + decoder; labels without a serial capture | A bench run, power-cycled mid-run, yields a clean log with labels |
| F2 | Battery monitoring (hub and probe), optional partition change, power-bank test | Measured current and runtime; low-battery events logged |
| F3 | Dry run: one hour in the garden/driveway with a scripted walk | Log extracted and analysed with the existing tools; detector sensible outdoors |
| F4 | Field test with a labelled protocol (empty, cross, walk near, walk away, still) at 2-3 distances | Detection rates per activity and distance, false-alarm rate per hour |

## 6. Decisions (2026-10-08)

- **Run length: no longer than 1 hour.** Storage and battery are therefore easy: features ~0.3 MB, raw amplitudes at 25 fps ~4.7 MB.
- **Repartition now: yes** (see 3.4b; custom table below; one USB flash, wipes stored events and the stored probe image).
- **Location: an open garage** (indoors with the door open). More reflections than open air, so detection should be closer to the basement than to a field; metal door and parked car change the baseline, so recalibrate when either moves; wind and outdoor movement enter through the door.
- **Phone-free calibration: yes, confirmed with auto-accept**, as a BOOT **double-click** (BOOT already means hold 3 s = pairing, 8 s = OTA window). Sequence: double-click, **10 s countdown to walk away** (RGB white blink, IO2 fast blink; a second double-click cancels), 30 s capture (RGB cyan breathe, IO2 slow blink), then auto-accept if every captured link is valid and there is no movement warning (RGB green flashes), otherwise discard (RGB solid red); every outcome is logged as an event.
- **Raw amplitudes: yes** (feature log + 52 amplitudes per frame at ~25 fps) so the detector can be redesigned offline on garage data.
- **Placement: both boards on the garage floor.** Antennas at floor level (strong ground and car-body reflections, people are mostly above the line); calibrate in the final placement, and test the same placement before the run.

Proposed hub partition table (`hub/partitions_field.csv`, hub only; the probe keeps `default_16MB.csv`):

```
nvs,      data, nvs,     0x9000,   0x5000,
otadata,  data, ota,     0xe000,   0x2000,
app0,     app,  ota_0,   0x10000,  0x300000,
app1,     app,  ota_1,   0x310000, 0x300000,
spiffs,   data, spiffs,  0x610000, 0x9E0000,
coredump, data, coredump,0xFF0000, 0x10000,
```

Flash writes may stall the loop for tens of milliseconds per sector erase **[verify]**: buffer in PSRAM and write from a low-priority task; measure dropped CSI frames and acquisition rate while it flushes. Power loss loses at most the last buffer (flush about every 30-60 s).

## 7. Still open

1. Battery packs (and whether they switch off at low current), and battery-voltage monitoring on both boards (#25).
2. A dry run (F3) before the real one.

1. Duration of a run (1 h, 8 h, 24 h): sets the storage and battery choice.
2. Features only, or raw-lite CSI as well (§3.3).
3. Repartition now (§3.4b) or just free space by deleting the probe image?
4. Enclosures and mounting (height, stands), and where the field test will be.
5. Whether to add a phone-free calibration gesture (§3.6).

## 8. Run plan (revised 2026-10-08 with the garage answers)

**Facts:** closed garage, about 30 ft x 25 ft, no car, no helper, both boards on the floor about **16-20 ft apart** (one geometry, about 18 ft),
hub on 110 V mains (USB adapter), probe on a battery. **Primary goal: a baseline in a quiet environment**, away from the server and the
Raspberry Pi that sit beside the hub in the basement, then detection and false-alarm numbers there.

### 8.1 Before the day (at home, basement)

| # | Check | Pass if |
| :--- | :--- | :--- |
| 1 | **F0:** calibrate and walk-test the presence detector (§12, #30) | Walk the room: STIRRING + `ROOM_MOTION`; stand on the line: `LINE_BREAK`; empty: clear |
| 2 | **Dry run, 15 min, hub on a wall adapter, probe on its battery:** start the log, tap each label once, double-click BOOT to calibrate (leave the room), download, decode | `field_decode.py`: 1 boot, 0 drops, labels, a CAL record, a CLOCK record; BOOT calibration accepted |
| 3 | **Basement baseline for comparison:** 15 min empty, same distance, same power setup as the garage (adapter + battery), logged | A file to compare noise floors with the garage (EMF, CSI std, RSSI). Note the Pi and server are beside the hub there |
| 4 | Probe battery: note its charge before and after the dry run | Well under half used per hour |
| 5 | Dashboard label buttons visible while logging; printed or on-phone run sheet, tape, tape measure, notebook | |

### 8.2 On site (about 15 min, before logging)

1. Tape two spots on the floor 18 ft apart (note the real distance); same orientation for both boards; hub on its adapter, as far from other electronics as practical (note what is nearby).
2. Power the probe, join `SPECTRAL_GRID` on the phone, dashboard open. Check: nodes 2/2, RSSI better than about -65 dBm, loss under 2 %.
3. Notebook: door state (closed), temperature, time, what electronics are in the garage, anything unusual.
4. Do **not** calibrate yet: calibration happens inside the logged run so it is recorded.

### 8.3 The logged run (times are targets; about 52 min)

Tap the label at the **start** of every block, then pocket the phone. "Out" = out of the garage (door closed behind you), nothing moving in it.
Line = the straight path between the boards. **Near** = within 5 ft of the line; **Far** = 8 ft or more from it (the garage is 25 ft wide).

| Time | Block | Label | What you do |
| :--- | :--- | :--- | :--- |
| 0:00 | Start | (none) | Dashboard: **Start logging** (writes the CLOCK record), tap **Empty** |
| 0:30 | Calibrate | (event) | Double-click BOOT on the hub; walk **Out** within 10 s and stay out about 45 s until the RGB flashes green (red = rejected: come back, double-click again) |
| 1:30-16:30 | **Quiet baseline** | Empty | Stay **Out**, 15 min. The main dataset for this run |
| 16:30-40:30 | Activity: 4 rounds (6 min each) | Cross line, Walk near, Walk far, Stand on line, Sit off line, Empty (60 s each) | Cross: across the line at the midpoint, about 5 s per pass. Near/Far as defined above. Stand: still on the midpoint. Sit: still, about 5 ft off the line. Empty: **Out** |
| 40:30-50:30 | Final empty | Empty | **Out**, 10 min: false-alarm rate and drift after the activity |
| ~51:00 | Stop | | Return, **Stop logging**, **Download**, note the probe battery level |

About 5 MB of log (1.6 KB/s). The Door/Car labels are not used (closed garage, no car).

### 8.4 What the data should answer

1. **Quiet baseline vs basement:** temporal std, RSSI spread, EMF raw/p2p and temperature drift over the empty blocks, garage vs basement.
2. **Integrity:** one boot, zero drops, 0.5 s window steps without gaps, CLOCK and CAL records present.
3. **Detection per activity** (Cross, Near, Far, Stand, Sit), per round, against the basement table in `SpectralGrid.md` 12.9. **Cross = quick right-angle crossings (shadow in 1-2 s bursts, ~20 % of the time); the basement "cross" blocks were mostly lingering on the line, so this is the first real test of brief crossings** (check the shadow run lengths per block to confirm the activity was performed as intended).
4. **False alarms:** non-clear time and onsets per hour in the empty blocks, with and without calibration drift.
5. **Calibration:** accepted at the first attempt? Threshold and baseline RSSI compared with the basement values.
6. **EMF channel** (GPIO 7 wires): idle noise in a quiet room vs next to the server, and its response during the activities (a first look at #32).
7. **Re-tuning:** replay the raw amplitudes offline (a `field_replay` tool still to be written) with other margins, holds and features.

### 8.5 Failure modes and what to do

| Problem | Action |
| :--- | :--- |
| Calibration rejected (red) | Come back, double-click again, stay out; the event is logged |
| Probe drops (node_down) | Check its battery and distance; the gap is in `nodes.csv` |
| Phone cannot reach the AP | Move nearer to the hub; the log keeps running, labels are lost: write times in the notebook |
| Log full / drops > 0 | Stop, download, erase, restart (the run is shorter than the budget anyway) |
| Mains adapter noise suspected | Repeat a 10 min empty block with the hub on a power bank (note both) |
| A person or pet walks through | Write the time in the notebook |

## 9. Still open

1. **Part B (relocation):** with only 16-20 ft available it adds little. Proposed: skip it, and use the time for the long baseline.
2. **Threshold priority:** catching people vs few false alarms. The data will show the trade; we choose afterwards.
3. **Date, time, and the probe's battery** (which pack, capacity).
4. **Mains vs battery for the hub:** you chose mains. The basement baseline (8.1 item 3) uses the same setup so the comparison is fair.


## 10. Replay comparison (garage vs basement)

**Note: this replay emulates the original calibration rule (largest window x margin); the firmware on branch `calibration-v2` uses the median-based rule (SpectralGrid.md 13.10).** `hub/tools/field_replay.py` replays the detector (`csi_window.h` + `presence.h`) over the garage field log (amplitudes, 25-frame windows) and the basement captures (raw I/Q at 50 fps and subsampled to the garage's 25 fps) and sweeps margin, vote, hold and shadow depth, calibrating each environment in place from 15 s of quiet. Full tables: `captures/replay_report.md` (gitignored).

**Validation:** against the live firmware states in the garage log the replay agrees on **97.5 %** of windows (98.7 % clear vs non-clear; replay/live tstd ratio 1.003, correlation 0.991). The gap is window timing (25 frames at 25 fps vs the firmware's 50 frames) and 0.25 amplitude quantisation.

**Current settings (margin 1.15, 2-of-3, hold 4 s, shadow 1.5 dB)**

| | false alarms (window % / per 10 min) | cross | near/room | far | stand_on | sit_off | cross MOTION/ON_LINE |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| garage | 0.3 % / 0.0 | 97 % | 100 % | 95 % | 100 % | 24 % | 65 / 32 |
| basement (25 fps) | 0.0 % / 0.0 | 89 % | 100 % | n/a | 99 % | 26 % | 23 / 66 |

**Margin (others current)**

| margin | garage FA % | garage far | garage sit | basement FA % | basement sit | garage calibrations with FA > 1 % (60 windows) |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| 1.00 | 33.3 | 100 | 39 | 18.6 | 87 | 92 % |
| 1.05 | 0.3 | 100 | 30 | 0.0 | 42 | 3 % (worst 1.9 %) |
| 1.10 | 0.3 | 100 | 27 | 0.0 | 30 | 0 % |
| 1.15 | 0.3 | 95 | 24 | 0.0 | 26 | 0 % |
| 1.25 | 0.0 | 70 | 17 | 0.0 | 24 | 0 % |

**Other parameters:** a 1-of-2 vote lifts cross ON_LINE (32 -> 39 % garage, 66 -> 71 % basement) at no false-alarm cost; a 6 s hold lifts sit_off by 7-9 points for 0.7 % garage false alarms; shadow 2.0-3.0 dB loses stand_on (garage 81 % / 71 %); **shadow 1.0 dB is a knife edge** (the garage's constant -41 dBm empty block flips between 0 % and 100 % false alarm for a 0.1 dB change in the calibrated baseline): keep 1.5.

**Recommendation: keep the current settings.** The two environments agree on the knee (nothing safe below margin 1.10; detection decays above 1.20); possible gains are 1-5 points and the data cannot say whether the margin is still needed against drift over hours. If more sensitivity is wanted, margin 1.10 is the lowest value with no false-alarm penalty across all 60 calibration windows; 1.05 is not safe.

**Caveats:** one person, one placement per environment, 24 min of garage baseline; garage sample sizes 129-179 windows per activity (half independent); the basement empty blocks are short so its onsets per 10 min are a weak estimate; sitting still off the line stays at 25-35 % unless false alarms are accepted.
