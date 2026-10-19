# Scoping: more nodes and probe-to-probe links

Status: **scoping only: the multi-node parts are not built.** Written 2026-10-08. Claims marked **[verify]** are physics or
hardware hypotheses that have not been tested on this project's boards. **Update (same day): §3 and phases 0-1 are done** (the
capture tooling, two recorded sessions, and a single-link presence detector, flashed but not yet walk-tested; see
`SpectralGrid.md` §12.9 / §13.7-13.8).

## 1. Goal and honest expectations

Detect people in an area and place them roughly on the dashboard map, using the ESP32-S3 boards' Wi-Fi CSI.

| Setup | Presence in the area | Position |
| :--- | :--- | :--- |
| Hub + 1 probe (today) | Yes, once the CSI feature is fixed (see §3) | **No.** One link: it can say "this link was disturbed", and possibly "on the line" vs "elsewhere" |
| Hub + 2 probes, probes also hear each other (3 links) | Better | Coarse: which side of the triangle, roughly |
| Hub + 4 probes around the area (10 links) | Good | Coarse blob, **realistic best case about 1-2 m**, one moving person **[verify]** |

Limits that more nodes do not remove: a person standing still is much harder than a moving one; two or more people blur
into one blob; walls and furniture change the baseline; this is a coarse tool, not a tracker.

## 2. Today's topology (from the code)

- Star: every probe sends telemetry to the hub only (ESP-NOW, 50 Hz, one 1.5 ms slot per probe; `shared/protocol.h`).
  Max 16 probes, and `tx_hz` must drop to 25 above 13 probes.
- CSI is taken only on the hub, per frame, per source MAC (`hub/src/csi.cpp`). Every link therefore ends at the hub.
- Probes do not compute CSI at all.

Geometry problem: N links that all share the hub as an endpoint fan out from one point. Position needs links that
**cross each other**, i.e. links between probes.

## 3. Prerequisite: fix the single-link detector first (DONE, awaiting a walk test)

More nodes multiply a weak feature. The current CSI feature (`csi_lltf_variance`, `shared/dsp.h:65`) is the variance
across subcarriers within one frame, scored one-sided against a fast-adapting baseline (`shared/dsp.h:25`,
`alpha = 0.02`). Plan: capture raw frames (the capture mode now in the firmware), choose features from the data, and
implement a two-sided, gain-normalised, temporal feature with a baseline that freezes while presence is detected. Every
phase below assumes this is done.

## 4. Proposed topology

Every node transmits a short **broadcast beacon** in its slot; every other node measures CSI on the beacons it hears
and reports a per-link summary to the hub.

- N+1 nodes (hub + N probes) give N(N+1)/2 links. 3 nodes: 3. 4 nodes: 6. 5 nodes: 10. 6 nodes: 15.
- The hub already broadcasts sync frames (`PKT_HUB_SYNC`), so probe-from-hub CSI needs no new transmit. Probes would
  gain a beacon (about 20 bytes) in their existing slot.
- Probes need each other's identity: the hub already knows every probe MAC and slot, and can distribute the roster in
  the sync/pairing messages.
- Telemetry grows a per-peer list, e.g. `{peer_id, motion_metric, mean_amp, rssi}` (not raw CSI): 4 bytes per peer.
  With 4 probes that is 16 bytes on top of the current 19. This is a wire change: **bump `PROTO_VERSION` to 3**
  (house rule: any wire change bumps it), and re-flash hub and all probes.

Key unknown **[verify]**: does a probe's radio deliver CSI for frames that are not addressed to it? Unicast frames to the
hub probably are filtered before the CSI callback. Broadcast beacons should pass. This must be tested early with three
boards, and decides whether probes need the extra beacon.

## 5. Localisation method (runs in the dashboard, not the firmware)

Radio tomographic imaging, simplified:
1. Each link has a disturbance score from the fixed detector (empty-room baseline per link).
2. The map area is a grid, e.g. 24 x 24 cells. A cell is "on" for a link if the extra path
   (distance to node A + distance to node B - distance A to B) is under a threshold (about one Fresnel zone).
3. Heat per cell = sum over links of (link score x on-weight), normalised and smoothed in time. Peaks are the blobs.

The dashboard already stores node positions per device (browser storage), so geometry lives there, and the firmware does
not need to know where anything is. The score per link is the only new payload. The heat map replaces the current red
glow along links.

## 6. Placement guidance

- Ring the area: nodes on the perimeter, about 2-4 m apart, at roughly waist to chest height, away from large metal.
- Avoid putting all nodes in a line (collinear links localise nothing). A rough rectangle or triangle is best.
- Keep links longer than about 2 m: very short links sit in the near field (hub-to-probe at 1 ft read -13 dBm).
- Fixed positions: recalibrate the empty-room baseline whenever a node moves.

## 7. Hardware

Probes use the same board and firmware as today (ESP32-S3-DevKitC-1), plus one jumper wire each for GPIO7 if the EMF
channel is wanted.

| Option | Boards to buy | Links | Use |
| :--- | :--- | :--- | :--- |
| Minimum useful | 1 more (3 nodes total) | 3 | Proves probe-to-probe CSI; coarse sides only |
| Recommended | 3 more (5 nodes total) | 10 | First real localisation attempt |
| Larger room | 5 more (7 nodes total) | 21 | Better coverage; airtime and tx_hz need review |

Each probe needs power (USB supply or the battery setup already tried).

## 8. Phased plan

| Phase | Work | Needs | Exit criterion |
| :--- | :--- | :--- | :--- |
| 0 | Record labelled CSI (capture mode), pick features offline | Current 2 boards | **Done 2026-10-08:** temporal std separates moving people (AUC 0.94-1.0); RSSI drop marks the line |
| 1 | Hub scoring rewrite: two-sided, gain-normalised, presence-latched | Phase 0 | **Built and flashed 2026-10-08; replay: empty 100 % clear, room motion 100 %.** Open: calibration + walk test on the board (#30) |
| 2 | Probe-to-probe CSI feasibility (beacons, unicast filtering question) | A third board | Probe hears another probe's frames and logs CSI |
| 3 | Protocol v3 + probe CSI + roster + per-peer telemetry | Phase 2 | All links visible in the nodes table, loss under 2 % |
| 4 | Dashboard heat map | Phase 3 | Blob follows a person walking a known path |
| 5 | Tuning and test protocol | 4-5 nodes | Documented accuracy in the real room |

## 9. Risks

- Probe CSI may be unavailable for unicast traffic (see §4); a beacon adds airtime.
- AGC and TX power change amplitude; features must be normalised **[verify]**.
- 2.4 GHz is shared: the Pi, phones and the home router add foreign frames (about 20 per second seen on the hub).
- Slot timing: more probes and beacons push on the 1.5 ms slot and the 50 Hz frame; `tx_hz` may need to drop.
- OTA to probes is unproven on hardware; a protocol bump means USB-flashing every board until it is.
- Static presence (a person sitting still) may stay out of reach.

## 10. Decisions needed from you

1. How many extra boards you are willing to buy (1, 3, 5)?
2. Room size and shape the map should cover, and where nodes can physically sit and be powered.
3. Is a coarse blob acceptable, or do you need named zones (e.g. "near the door")? Zones are far easier and more reliable.
4. Should the hub's own EMF/thermal channels stay in the score once presence comes from CSI?
