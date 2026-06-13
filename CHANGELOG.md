# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.0.0] - 2026-06-13

### Added
- RSSI-variance motion and presence detection with an auto-calibrated baseline and hysteresis.
- Web dashboard with a live signal scope, threshold line, motion state, and telemetry.
- Compact `/data` JSON API (about 137 bytes) with client-side history and rendering.
- Onboard-LED motion indicator and a serial status meter.
- mDNS (`wifiradar.local`) and direct WiFi connection via `src/secrets.h`.
- PlatformIO build configuration, GitHub Actions CI, documentation, and contribution guides.

[1.0.0]: https://github.com/mandarwagh9/esp8266-wifi-radar/releases/tag/v1.0.0
