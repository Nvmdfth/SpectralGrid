# EMF wire test (GPIO 7): does a hand or a touch move the reading?

Status: **run 2026-10-08** (`captures/emf1/`). Results: a hand at **30 cm does not** register (hub +50 counts = 1.7 sigma, probe +11 = 2 sigma);
a hand at **10 cm does** (hub +100 to +200 counts, probe +95 to +145 counts, repeatable over two rounds); a **touch** is huge (hub saturates at 65,535, probe +17,000).
Blocks were shorter than planned (8-14 s) but both rounds agree. The windowed `emf_p2p` stayed at 0 throughout: the old EMF score cannot see a slow
level shift. As a result a separate **touch event** was added (`shared/touch.h`, 13.9 in the spec): it raises an event, a UI badge and a blue/white light
sequence, and is not part of the presence score.

**Why.** Both boards have a floating jumper wire on GPIO 7 (capacitive touch input). In the garage run neither node's EMF reading reacted to
people 5-20 ft away (hub p2p median 0 with noise spikes, probe 0-4). That says nothing about *near-field* sensitivity. This test
measures it directly, board by board, with a hand at 30 cm, a hand at 10 cm and a touch.

**What is logged.** The field log's NODE record now carries the instantaneous touch reading `emf_raw` (hub about 19,100, probe about
18,500-20,800 when idle) in addition to the windowed peak-to-peak `emf_p2p`, 2 times per second per node. A hand near the wire adds
capacitance, which shifts the raw level (a touch shifts it a lot). The decoder writes it to `nodes.csv` as `emf_raw`.

## Procedure (about 12 minutes, basement is fine)

Needs the new firmware on the hub (field log with `emf_raw` and the EMF label buttons). Leave both boards where they normally sit, about
10 ft apart, powered as usual. Stand back from them until each step.

1. Dashboard: **Start logging** (erase an old log first if you want a clean file).
2. Tap **EMF quiet**, step at least 3 m away, stay still for **60 s**.
3. Hub, then probe. For the board under test, do this sequence, tapping the label at the start of each step and keeping still for the time shown:

| Label | Time | What you do |
| :--- | :--- | :--- |
| EMF quiet | 20 s | Step back 3 m from the boards, still |
| Hub (or Probe) hand 30 cm | 30 s | Hold one hand still, palm toward the wire tip, 30 cm away (use a ruler) |
| EMF quiet | 20 s | Step back |
| Hub (or Probe) hand 10 cm | 30 s | Same, 10 cm from the wire tip |
| EMF quiet | 20 s | Step back |
| Hub (or Probe) touch wire | 30 s | Rest a fingertip on the **bare tip** of the wire (touch bare metal of the board's housing first to discharge static; do not touch the board's pins) |
| EMF quiet | 20 s | Step back |

4. Do the full sequence **twice** for each board (hub first, then probe). Roughly 10 minutes plus the 60 s baseline.
5. **Stop logging**, download, decode, analyse:

```
python3 hub/tools/field_decode.py field.bin captures/emf1/dec
python3 hub/tools/emf_analyze.py captures/emf1/dec
```

## What to expect and how it is judged

`emf_analyze.py` compares the median `emf_raw` of each step with the quiet baseline of the same node, in counts and in units of the
baseline's noise (robust sigma), and reports the share of samples whose `emf_p2p` exceeds 100. As a rule of thumb a shift of at least
**5 sigma and 50 counts**, repeatable in both rounds, counts as a real response. A touch should respond clearly; whether a hand at 30 cm does
is the actual question. If only touches respond, the wire is a touch sensor, not a proximity sensor, and the EMF channel's role (and its weight
in the score) should be reconsidered.

**Confounds to note in the notebook:** which charger/adapter powers each board, anything switching nearby (fridge, fluorescent lights,
phone chargers), your clothing (synthetic fabric near the wire), and whether the hub's mains adapter was on (the garage hub showed
spikes of up to 328 in p2p, about 9 % of samples, in every condition).
