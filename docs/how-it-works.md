# How WiFi Radar works

A deep-dive into how a bare ESP8266 detects people using only the WiFi signal.
Three layers: **the physics**, **the signal processing**, and **the system**.

---

## 1. The physics — why a radio "sees" you

Your router and the ESP8266 communicate at 2.4 GHz (wavelength ≈ 12.5 cm). The
signal does **not** travel in a single straight line — it reflects off walls,
floor, ceiling, furniture, and you. The ESP receives many copies of the same
signal, each arriving via a different path with a different delay and phase.

```
        ┌──────── direct ────────┐
ROUTER ─┼──► wall bounce ──►──────┼──► ESP8266
        └──► floor / ceiling ─────┘
   (all copies sum → one RSSI value)
```

Those copies **interfere** — some add, some cancel — and the sum is the **RSSI**
(received signal strength) the ESP reports. That value is effectively a
fingerprint of the room's geometry at that instant.

When a person (mostly water, which absorbs and reflects 2.4 GHz strongly) moves
through the space, they **block some paths and create new reflections**. The
interference pattern shifts, so the RSSI changes. Stand still → the pattern is
frozen → RSSI is steady. Move → RSSI keeps jumping around.

> **Key insight:** we ignore the *level* of RSSI (that mostly encodes distance)
> and watch its **variability**. Stillness = low jitter. Motion = high jitter.

---

## 2. The signal processing — jitter → decision

Running ~20 times per second on the chip:

```
every 50 ms:
  poke router (UDP) ──► read WiFi.RSSI() ──► push into 64-sample ring buffer
                                                      │
                                       std-dev over last ~3.2 s
                                                      │
                                  score = std / baseline
                                                      │
                            score ≥ TRIP ? ──► MOTION (LED on)
                            score < CLEAR for HOLD ? ──► CLEAR
```

- **Sample at 20 Hz.** `WiFi.RSSI()` only refreshes when a packet arrives from
  the AP, so each cycle the firmware sends a 1-byte **UDP "poke"** to the router
  to keep traffic — and therefore fresh RSSI readings — flowing even on a quiet
  network.
- **Sliding window.** The last `WINDOW` (64) readings (~3.2 s) are kept in a ring
  buffer, and we compute their **standard deviation** — literally "how much is
  RSSI bouncing around right now?"
- **Calibration → baseline.** On boot the firmware watches a still room for
  `CAL_SAMPLES` (~10 s) and records the *normal* jitter using
  [Welford's online variance algorithm](https://en.wikipedia.org/wiki/Algorithms_for_calculating_variance#Welford's_online_algorithm)
  (no need to store all samples).
- **The score.** `score = currentStd ÷ baseline`. An empty room sits near
  `score ≈ 1`; movement pushes the jitter — and the score — well above 1.
- **The decision (hysteresis).** Crossing `TRIP_FACTOR` (2.2×) flips to MOTION.
  It won't return to CLEAR until the score stays below `CLEAR_FACTOR` (1.5×) for
  `MOTION_HOLD_MS` (2 s). That hold prevents stuttering on the threshold edge.

---

## 3. The system — firmware + dashboard

```
ESP8266                            Browser (phone / laptop)
────────                           ────────────────────────
GET /        ──(once)──►   HTML + CSS + JS page (~10 KB, from flash)
GET /data    ◄─every 220ms─  fetch()          ──► push score into a local
137-byte JSON ──────────►   {rssi,std,score,…}     history array, then
                                                   requestAnimationFrame
                                                   redraws the scope @60fps
GET /recal   ──────────►   re-run baseline calibration
```

- The page loads **once**, then the browser polls the tiny `/data` endpoint for
  the latest numbers and keeps its **own** history. The ESP never rebuilds or
  re-sends the graph, which is why it stays responsive.
- The scope maps **score → height**, draws the amber **TRIP** line, and shifts
  the trace teal → amber → red as the score climbs, going full red-alarm on
  motion.

---

## Tuning & limitations

- **Recalibrate** after moving the board or rearranging the room — the baseline
  is specific to one geometry.
- Raise `TRIP_FACTOR` if it's too twitchy; lower it if it misses you.
- More ambient WiFi traffic improves sampling quality.
- This is **RSSI**-based sensing: it reports motion/presence, not identity,
  count, position, pose, or an image. Richer **CSI** (channel state information)
  sensing — used for breathing and fall detection — needs an ESP32 or special
  NICs, and is a natural future direction for this project.
