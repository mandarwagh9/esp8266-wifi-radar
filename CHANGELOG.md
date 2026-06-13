# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.0.0] - 2026-06-13

### Added
- RSSI‑variance motion / presence detection with an auto‑calibrated baseline and hysteresis.
- Live "signals console" web dashboard — phosphor scope, threshold line, motion alarm, telemetry.
- Lean `/data` JSON API (~137 bytes) with client‑side history and 60 fps rendering.
- Onboard‑LED motion indicator and a live serial ASCII meter.
- mDNS (`wifiradar.local`) and direct‑connect WiFi via `src/secrets.h`.
- PlatformIO build config, GitHub Actions CI, documentation, and contribution guides.

[1.0.0]: https://github.com/mandarwagh9/esp8266-wifi-radar/releases/tag/v1.0.0
