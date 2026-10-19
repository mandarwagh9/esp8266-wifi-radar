# WiFi Radar

[![Build](https://github.com/mandarwagh9/esp8266-wifi-radar/actions/workflows/build.yml/badge.svg)](https://github.com/mandarwagh9/esp8266-wifi-radar/actions/workflows/build.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
![Platform: ESP8266](https://img.shields.io/badge/platform-ESP8266-555.svg)

Motion and presence detection on a bare ESP8266 using only the WiFi signal, with no camera, PIR, or additional sensors required. A moving body disturbs the 2.4 GHz multipath in a room, making the received signal strength (RSSI) fluctuate. The firmware learns an empty-room baseline and reports motion when that fluctuation rises above it, and serves a real-time web dashboard for monitoring.

An external digital motion sensor (PIR, RCWL-0516, etc.) can optionally be wired in alongside the RSSI detector, and an SNMP agent can optionally be enabled for monitoring systems like Zabbix. Both are off by default and fully configurable from the web UI — no recompiling needed.

![WiFi Radar dashboard](docs/screenshot.png)

## Features

- Runs on any ESP8266 with no additional hardware required.
- Detects motion from RSSI variance against an auto-calibrated baseline.
- Real-time web dashboard with a live signal scope and motion state.
- On-device WiFi setup: no credentials are compiled in. The board opens its own setup access point and captive portal on first boot, and saves your choice to flash.
- Optional external digital motion sensor (PIR, RCWL-0516, or similar) on a configurable GPIO, reported alongside the RSSI detector.
- Optional SNMP agent (SNMPv2c, read-only) exposing live metrics as OIDs, for polling by Zabbix or any other SNMP-based monitoring system.
- A `/config` page to turn the sensor and SNMP on or off, pick the sensor's pin and polarity, and set the SNMP community string — all persisted to flash.
- Onboard LED and serial output for status without a browser.
- mDNS hostname (`wifiradar.local`).
- Sensitivity and timing exposed as configurable constants.

## How it works

WiFi from the router reaches the ESP8266 over many reflected paths that combine into a single RSSI value, which acts as a fingerprint of the room. A moving person changes those paths, so RSSI fluctuates. The firmware samples RSSI at 20 Hz, computes the standard deviation over a roughly 3-second window, divides it by a calibrated empty-room baseline to produce a score, and reports motion when the score crosses a threshold (with hysteresis to prevent flicker).

See [docs/how-it-works.md](docs/how-it-works.md) for the full explanation.

If the optional external sensor is enabled, it's read independently every loop tick (a plain `digitalRead()` against its active-high/low setting — the sensor module itself has no firmware of its own, it just outputs a HIGH or LOW signal) and reported as a second, separate motion flag alongside the RSSI-based one.

## Hardware

| Component | Requirement |
|---|---|
| Board | Any ESP8266 (ESP-12E/F, NodeMCU, Wemos D1 mini, ESP-07) |
| Flash | 1 MB minimum, 4 MB recommended |
| Programmer | Onboard USB, or a USB-serial adapter (FTDI, CP2102, CH340) |
| Additional parts | None required. Optionally: a digital-output PIR or RCWL-0516 motion sensor module, wired to one free GPIO. |

## Quick start

### PlatformIO

```bash
git clone https://github.com/mandarwagh9/esp8266-wifi-radar.git
cd esp8266-wifi-radar
pio run -t upload
pio device monitor
```

No credentials file to copy or edit — WiFi is configured after flashing (see below).

### Arduino IDE

1. Install the ESP8266 board package (Boards Manager, search "esp8266").
2. Install the libraries listed under [Dependencies](#dependencies) below via the Library Manager.
3. Open `src/main.cpp`, select your board, and upload.

### First boot: WiFi setup

No WiFi credentials are compiled into the firmware. On first boot (or whenever previously-saved credentials fail to connect), the board opens its own access point:

- **SSID:** `WiFiRadar-Setup`
- **Password:** `radarsetup`

Connect to it from a phone or laptop; a captive portal should open automatically at `192.168.4.1` (or open it manually). Pick your WiFi network, enter its password, and save. The board reboots, connects, and remembers the network from then on.

To forget the saved network and reopen the setup portal later, browse to `http://wifiradar.local/resetwifi`.

On a normal boot the board connects to the saved WiFi, prints its IP over serial, runs a 10-second baseline calibration (stay still or leave the room), then begins sensing.

## Dependencies

Installed automatically by PlatformIO via `platformio.ini`; for the Arduino IDE, install these through the Library Manager:

| Library | Purpose |
|---|---|
| [WiFiManager](https://github.com/tzapu/WiFiManager) (tzapu) | On-device WiFi setup portal |
| [ArduinoJson](https://arduinojson.org/) (bblanchon) | Reading/writing `/config.json` |
| [Arduino_SNMP](https://github.com/0neblock/Arduino_SNMP) (0neblock) | SNMP agent (only used if SNMP is enabled) |

LittleFS (bundled with the ESP8266 Arduino core) stores the configuration file.

## Configuration

### Runtime (web UI)

Open `http://wifiradar.local/config` (or the board's IP + `/config`):

| Setting | Description |
|---|---|
| External sensor: enable | Turns the optional digital motion sensor input on or off |
| External sensor: pin | Which GPIO the sensor is wired to |
| External sensor: polarity | Whether the sensor reads HIGH or LOW on motion (most PIR/RCWL-0516 modules are active-HIGH) |
| SNMP: enable | Turns the SNMP agent on or off |
| SNMP: community | The SNMPv2c community string your poller must use |
| SNMP: sysName / sysLocation | Standard MIB-II identity fields reported by the agent |

Saving reboots the board to apply the new settings cleanly. Settings persist across reboots and reflashes (as long as the flash filesystem isn't erased).

**Wiring the external sensor:** connect the sensor's signal/OUT pin to the chosen GPIO, and its VCC/GND to the board's power rails (check your specific module's voltage requirements — most PIR and RCWL-0516 modules are fine on 3.3V or 5V, but confirm before wiring). GPIO2 (the onboard LED) and GPIO1/3 (Serial TX/RX) aren't offered as choices since they're already in use; the config page's dropdown only lists free GPIOs.

### Compile-time constants

Constants at the top of [`src/main.cpp`](src/main.cpp):

| Constant | Default | Description |
|---|---|---|
| `TRIP_FACTOR` | 2.2 | Score (multiple of baseline) required to enter the motion state |
| `CLEAR_FACTOR` | 1.5 | Score below which the motion state clears |
| `WINDOW` | 64 | Live averaging window in samples (about 3.2 s) |
| `CAL_SAMPLES` | 200 | Calibration length in samples (about 10 s) |
| `MOTION_HOLD_MS` | 2000 | Minimum time held in the motion state |
| `SAMPLE_INTERVAL_MS` | 50 | RSSI sample period (20 Hz) |
| `SNMP_OID_PREFIX` | `.1.3.6.1.4.1.52145.1` | Base OID for the custom SNMP metrics — see [SNMP](#snmp) below |

Raise `TRIP_FACTOR` to reduce sensitivity, lower it to increase sensitivity. Recalibrate after moving the board.

## Dashboard and HTTP API

Open `http://wifiradar.local`, or the IP shown on serial.

| Endpoint | Method | Response |
|---|---|---|
| `/` | GET | Dashboard page |
| `/data` | GET | Telemetry JSON |
| `/recal` | GET | Re-runs baseline calibration |
| `/config` | GET | Configuration page |
| `/config` | POST | Saves configuration and reboots |
| `/resetwifi` | GET | Erases saved WiFi credentials and reboots into the setup portal |

Example `/data` response:

```json
{"rssi":-57,"std":1.96,"score":1.26,"motion":false,"calibrated":true,
 "cal":200,"calTotal":200,"baseline":1.56,"trip":2.20,"up":42,"ch":3,
 "radarEnabled":false,"radarMotion":false,"snmpEnabled":false}
```

## SNMP

When enabled from `/config`, the board runs a read-only SNMPv2c agent on UDP 161.

- **Community string:** set on the config page (default `public`).
- **Standard OIDs:** `sysDescr` (`.1.3.6.1.2.1.1.1.0`), `sysName` (`.1.3.6.1.2.1.1.5.0`), `sysLocation` (`.1.3.6.1.2.1.1.6.0`), both configurable from `/config`.
- **Custom OIDs** under `.1.3.6.1.4.1.52145.1.*`:

  | OID suffix | Type | Value |
  |---|---|---|
  | `.1` | Integer | RSSI, dBm |
  | `.2` | Gauge32 | Motion score × 100 |
  | `.3` | Gauge32 | Live RSSI std-dev × 100 |
  | `.4` | Gauge32 | Calibrated baseline std-dev × 100 |
  | `.5` | Integer | Calibrated: 0 or 1 |
  | `.6` | Integer | RSSI-based motion: 0 or 1 |
  | `.7` | Integer | WiFi channel |
  | `.8` | Gauge32 | Uptime, seconds |
  | `.9` | Integer | External sensor enabled: 0 or 1 |
  | `.10` | Integer | External sensor motion: 0 or 1 |

  Score/std-dev/baseline are scaled ×100 and reported as integers so they fit cleanly as SNMP Gauge32 values; divide by 100 in your monitoring system (e.g. a `0.01` multiplier on a Zabbix item) to get the original decimal value.

**Important:** `.1.3.6.1.4.1.52145` is a placeholder enterprise number, not a real IANA-registered [Private Enterprise Number](https://pen.iana.org). It's fine for a private LAN / internal monitoring setup, but if you plan to use this scheme anywhere it might collide with someone else's numbering (a shared network, a published integration, etc.), register your own PEN and change `SNMP_OID_PREFIX` in `src/main.cpp` accordingly.

Test with `snmpwalk` before pointing a full monitoring system at it:

```bash
snmpwalk -v2c -c public wifiradar.local .1.3.6.1.4.1.52145.1
```

## Serial output

```text
=== WiFi Radar :: RSSI motion sensing ===
Connecting to saved WiFi (or opening setup portal)...
Connected.  IP 192.168.1.9  ch 3  RSSI -57 dBm
[snmp] Agent enabled on UDP 161, community "public"
[cal] Done. baseline RSSI std = 1.56 dB. Watching for motion...
RSSI  -57 dBm | std 1.96 | x1.26 [########----------------------] idle
RSSI  -55 dBm | std 6.10 | x3.91 [##############################] ** MOTION **
```

## Troubleshooting

### Nothing happens after flashing: no serial output, no `WiFiRadar-Setup` network, or a garbled repeating pattern on serial

This is almost always a **flash-size mismatch** between `platformio.ini`'s `board` setting and your module's actual flash size — see the comment block at the top of `platformio.ini` for the full explanation and the fix. In short: this firmware persists its `/config` page settings using LittleFS, whose partition layout is fixed at compile time based on the configured board. If the real chip has less flash than that (a bare ESP-01 or ESP-01S, commonly 1 MB, is the usual culprit — this project otherwise targets 4 MB boards like the ESP-12E/F, NodeMCU, or Wemos D1 mini), touching that filesystem can crash the chip into a boot loop before your code, or even WiFiManager, ever runs. On serial, that loop often looks like a repeating garbled pattern rather than readable text, because what you're actually seeing is the boot ROM's own banner repeating at high speed, not a baud-rate problem.

The firmware detects this mismatch at boot and skips the filesystem instead of crashing (you'll see a `[fs] WARNING` line on serial, and config just won't persist between reboots), but if you're still seeing the garbled-loop symptom, double-check `platformio.ini` is actually set up for your real hardware and re-flash — see the "IMPORTANT — flash size must match your actual module" comment there for the exact settings for a 1 MB ESP-01/ESP-01S versus a 4 MB module.

### The board boots fine, but `/config` says it can't save settings

This is a different (and more subtle) problem than the boot-loop one above: the chip's flash size now matches `platformio.ini`, so it boots, but the filesystem partition reserved on it is too small — or doesn't exist at all. This is common on 1 MB boards: PlatformIO's generic `board = esp01_1m` layout is optimized for sketch size, not persistence, and by default may reserve little or no space for LittleFS. Fix it by also setting an explicit linker script that carves out filesystem space:

```ini
board_build.ldscript = eagle.flash.1m64.ld
```

(64 KB is comfortably more than this project's config file needs.) See the full comment block at the top of `platformio.ini` for this alongside the other 1 MB settings.

To confirm what's actually happening on your board, watch serial at 115200 right after boot:
- `[fs] WARNING: firmware was built for ... flash but this chip reports ...` → the flash-size mismatch from the section above; fix `board`/`board_build.flash_size` first.
- `[fs] LittleFS still unavailable after format...` → sizes match, but no usable filesystem partition exists; this is the `board_build.ldscript` fix above.
- `[fs] LittleFS mounted: N / M bytes used` → the filesystem is fine; if saving still fails after this line, it's a genuine write error (check the serial message `saveConfig()` prints, and consider that LittleFS may have become corrupted — try `/resetwifi` or a full chip erase and re-flash).

The `/config` page itself also shows a warning banner whenever the filesystem isn't usable, so you don't have to be watching serial to notice.

### The `WiFiRadar-Setup` network disappears after a few minutes

That's expected: the setup portal times out after 180 seconds (`wm.setConfigPortalTimeout(180)` in `src/main.cpp`) if nobody connects to configure it, and the board reboots and retries. Power-cycle the board and connect to `WiFiRadar-Setup` promptly after reset.

## Limitations

- The RSSI detector senses motion and presence only, not identity, count, position, or pose, and not an image.
- Effective range for the RSSI detector is roughly one room and depends heavily on the environment.
- Requires a brief still calibration; recalibrate after moving the board or rearranging the room.
- More ambient WiFi traffic improves RSSI sampling. The firmware sends small UDP packets to the router to keep readings fresh.
- Uses RSSI, which is coarser than CSI-based WiFi sensing (CSI requires an ESP32 or specialized network cards).
- The external sensor input, when enabled, is only as good as the module wired to it — the firmware does a plain threshold read, with no debouncing beyond what the sensor module itself provides.
- The SNMP agent is read-only (no SNMP SET support) and SNMPv2c only (no v3/authentication) — treat it as suitable for a trusted internal network, not an untrusted one.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) and the [Code of Conduct](CODE_OF_CONDUCT.md). CI builds the firmware with PlatformIO on every push and pull request.

## License

Released under the [MIT License](LICENSE).

## Acknowledgements

Built with the [ESP8266 Arduino core](https://github.com/esp8266/Arduino), [PlatformIO](https://platformio.org/), [WiFiManager](https://github.com/tzapu/WiFiManager), [ArduinoJson](https://arduinojson.org/), and [Arduino_SNMP](https://github.com/0neblock/Arduino_SNMP).
