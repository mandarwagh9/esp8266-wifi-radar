# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- On-device WiFi setup via WiFiManager: no credentials compiled in. First boot (or a failed
  reconnect) opens a `WiFiRadar-Setup` access point and captive portal; the choice is saved to
  flash. `/resetwifi` forgets the saved network and reopens the portal.
- Optional external digital motion sensor (PIR, RCWL-0516, or similar) on a configurable GPIO,
  read independently of the RSSI detector and reported alongside it (dashboard, `/data`, SNMP).
- Optional read-only SNMPv2c agent (UDP 161) exposing RSSI, score, std-dev, baseline, calibrated/
  motion flags, channel, uptime, and the external sensor's state as OIDs, for polling by Zabbix or
  similar. Standard `sysDescr`/`sysName`/`sysLocation` are also exposed.
- `/config` page to toggle the external sensor and SNMP, pick the sensor's pin/polarity, and set
  the SNMP community/sysName/sysLocation. Settings persist to `/config.json` on LittleFS.
- `merge_bin.py` post-build step producing a clearly-named single flashable image.

### Changed
- `platformio.ini`: added WiFiManager, ArduinoJson, and Arduino_SNMP dependencies; switched to the
  LittleFS filesystem.

### Fixed
- Boot crash loop on hardware whose actual flash size is smaller than `platformio.ini`'s `board`
  setting assumes (most commonly a 1 MB ESP-01/ESP-01S flashed with a config built for a 4 MB
  board). The firmware now checks `ESP.getFlashChipRealSize()` against `ESP.getFlashChipSize()`
  before touching LittleFS and skips the filesystem (config just won't persist) instead of
  mounting/formatting flash addresses that don't physically exist, which could hang or crash the
  chip before WiFiManager ever ran. `platformio.ini` now documents the correct settings for both
  4 MB boards (the default) and 1 MB ESP-01/ESP-01S modules.
- Config saves silently/generically failing on boards whose flash size matches but whose linker
  script reserves little or no space for a filesystem (the default on PlatformIO's generic
  `esp01_1m` layout). `platformio.ini` now documents an explicit `board_build.ldscript` for 1 MB
  boards that reserves a 64 KB LittleFS partition. The firmware also now tracks filesystem
  availability as explicit state (rather than a `setup()`-local variable), logs how many bytes a
  successful mount actually has, reports the real reason a save failed over serial, and shows a
  warning banner on the `/config` page itself whenever persistence isn't available.

### Removed
- `src/secrets.h` / `src/secrets.example.h` and the compile-time WiFi credentials flow — replaced
  by the on-device WiFiManager setup above.

## [1.0.0] - 2026-06-13

### Added
- RSSI-variance motion and presence detection with an auto-calibrated baseline and hysteresis.
- Web dashboard with a live signal scope, threshold line, motion state, and telemetry.
- Compact `/data` JSON API (about 137 bytes) with client-side history and rendering.
- Onboard-LED motion indicator and a serial status meter.
- mDNS (`wifiradar.local`) and direct WiFi connection via `src/secrets.h`.
- PlatformIO build configuration, GitHub Actions CI, documentation, and contribution guides.

[1.0.0]: https://github.com/mandarwagh9/esp8266-wifi-radar/releases/tag/v1.0.0
