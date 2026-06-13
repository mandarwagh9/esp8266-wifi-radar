# WiFi Radar

[![Build](https://github.com/mandarwagh9/esp8266-wifi-radar/actions/workflows/build.yml/badge.svg)](https://github.com/mandarwagh9/esp8266-wifi-radar/actions/workflows/build.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
![Platform: ESP8266](https://img.shields.io/badge/platform-ESP8266-555.svg)

Motion and presence detection on a bare ESP8266 using only the WiFi signal, with no camera, PIR, or additional sensors. A moving body disturbs the 2.4 GHz multipath in a room, making the received signal strength (RSSI) fluctuate. The firmware learns an empty-room baseline and reports motion when that fluctuation rises above it, and serves a real-time web dashboard for monitoring.

![WiFi Radar dashboard](docs/screenshot.png)

## Features

- Runs on any ESP8266 with no additional hardware.
- Detects motion from RSSI variance against an auto-calibrated baseline.
- Real-time web dashboard with a live signal scope and motion state.
- Low resource use: about 36% RAM and a 137-byte telemetry payload per poll.
- Onboard LED and serial output for status without a browser.
- mDNS hostname (`wifiradar.local`).
- Sensitivity and timing exposed as configurable constants.

## How it works

WiFi from the router reaches the ESP8266 over many reflected paths that combine into a single RSSI value, which acts as a fingerprint of the room. A moving person changes those paths, so RSSI fluctuates. The firmware samples RSSI at 20 Hz, computes the standard deviation over a roughly 3-second window, divides it by a calibrated empty-room baseline to produce a score, and reports motion when the score crosses a threshold (with hysteresis to prevent flicker).

See [docs/how-it-works.md](docs/how-it-works.md) for the full explanation.

## Hardware

| Component | Requirement |
|---|---|
| Board | Any ESP8266 (ESP-12E/F, NodeMCU, Wemos D1 mini, ESP-07) |
| Flash | 1 MB minimum, 4 MB recommended |
| Programmer | Onboard USB, or a USB-serial adapter (FTDI, CP2102, CH340) |
| Additional parts | None |

## Quick start

### PlatformIO

```bash
git clone https://github.com/mandarwagh9/esp8266-wifi-radar.git
cd esp8266-wifi-radar
cp src/secrets.example.h src/secrets.h    # add your WiFi credentials
pio run -t upload
pio device monitor
```

### Arduino IDE

1. Install the ESP8266 board package (Boards Manager, search "esp8266").
2. Copy `src/secrets.example.h` to `src/secrets.h` and set your WiFi credentials.
3. Open `src/main.cpp`, select your board, and upload.

On boot the board connects to WiFi, prints its IP over serial, runs a 10-second baseline calibration (stay still or leave the room), then begins sensing.

WiFi credentials are stored in `src/secrets.h`, which is git-ignored and never committed.

## Configuration

Constants at the top of [`src/main.cpp`](src/main.cpp):

| Constant | Default | Description |
|---|---|---|
| `TRIP_FACTOR` | 2.2 | Score (multiple of baseline) required to enter the motion state |
| `CLEAR_FACTOR` | 1.5 | Score below which the motion state clears |
| `WINDOW` | 64 | Live averaging window in samples (about 3.2 s) |
| `CAL_SAMPLES` | 200 | Calibration length in samples (about 10 s) |
| `MOTION_HOLD_MS` | 2000 | Minimum time held in the motion state |
| `SAMPLE_INTERVAL_MS` | 50 | RSSI sample period (20 Hz) |

Raise `TRIP_FACTOR` to reduce sensitivity, lower it to increase sensitivity. Recalibrate after moving the board.

## Dashboard and HTTP API

Open `http://wifiradar.local`, or the IP shown on serial.

| Endpoint | Method | Response |
|---|---|---|
| `/` | GET | Dashboard page |
| `/data` | GET | Telemetry JSON (about 137 bytes) |
| `/recal` | GET | Re-runs baseline calibration |

Example `/data` response:

```json
{"rssi":-57,"std":1.96,"score":1.26,"motion":false,"calibrated":true,
 "cal":200,"calTotal":200,"baseline":1.56,"trip":2.20,"up":42,"ch":3}
```

## Serial output

```text
=== WiFi Radar :: RSSI motion sensing ===
Connecting to "Home".........
Connected.  IP 192.168.1.9  ch 3  RSSI -57 dBm
[cal] Done. baseline RSSI std = 1.56 dB. Watching for motion...
RSSI  -57 dBm | std 1.96 | x1.26 [########----------------------] idle
RSSI  -55 dBm | std 6.10 | x3.91 [##############################] ** MOTION **
```

## Limitations

- Detects motion and presence only, not identity, count, position, or pose, and not an image.
- Effective range is roughly one room and depends heavily on the environment.
- Requires a brief still calibration; recalibrate after moving the board or rearranging the room.
- More ambient WiFi traffic improves sampling. The firmware sends small UDP packets to the router to keep readings fresh.
- Uses RSSI, which is coarser than CSI-based WiFi sensing (CSI requires an ESP32 or specialized network cards).

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) and the [Code of Conduct](CODE_OF_CONDUCT.md). CI builds the firmware with PlatformIO on every push and pull request.

## License

Released under the [MIT License](LICENSE).

## Acknowledgements

Built with the [ESP8266 Arduino core](https://github.com/esp8266/Arduino) and [PlatformIO](https://platformio.org/).
