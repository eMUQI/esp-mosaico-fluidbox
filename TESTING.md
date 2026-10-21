# Test results

## Iris integration, 2026-10-21

Built with official ESP-IDF `7b9cc1ac79f865983f59bb8ff3ff43eb74ff1dbe` (6.2), its RISC-V 16.1 toolchain, and ESP-Mosaico Utils `c6b43a6be7fb8aba832fc4016d4076702f5414ae`. Tested on ESP-Mosaico with Vibe Mode 0.2.0, 1,200 particles and QSPI at 60 MHz.

The initial XIP-enabled integration passed installation health checks but ran the particle mode at approximately 13–14 FPS. Removing the inherited unbuffered stdout setting alone did not recover the frame rate. With PSRAM XIP disabled and 72,000 bytes of particle exchange/snapshot buffers placed in PSRAM, particle mode returned to 59.0–59.1 FPS. Solver working arrays and rendering scratch remain in internal SRAM.

The successful sample reported approximately 76.8 simulation steps/second, 63,915 bytes of free internal SRAM, no IMU misses, no TE timeouts, and no transfer underruns. Iris was active and streaming logs during sampling. This establishes particle-mode performance in the observed device pose; it is not a matched benchmark of every mode or long-term stability.

The official Iris and Ideas product validators accepted the package. The wired BIN was checked for the matching application and reviewed Vibe Mode image. Host simulation/render checks (ASan/UBSan and pixel equivalence) and the display pipeline check passed. The wired BIN has not been flashed in this session.

The final no-XIP build also passed a complete normal application → Vibe Mode → Iris update → healthy normal application cycle in approximately 20 seconds. Firmware identity, system partition hashes and the device's healthy confirmation passed the official installer checks.

### Checks before release

- Compare all five modes with the standalone firmware using the same device pose and sampling duration. Particle FPS is currently comparable to the historical sample, but simulation frequency has no matched baseline.
- Check sustained operation, mode changes, reset behavior, and display quality on the device.
- Install the complete wired BIN on a device with backed-up data, then verify the application and Vibe Mode recovery path.
- Select the next version and rebuild both release artifacts with that version before publishing.

## Historical standalone firmware

A rough check on Mosaico / ESP32-S31 with ESP-IDF v6.1, 1,200 particles and QSPI at 60 MHz, before Iris integration. The two performance log lines in `main/main.c` were raised from Debug to Info so they printed.

Median FPS while lying flat:

| Mode | FPS |
|---|---:|
| Particles | 59.1 |
| Liquid | 24.1 |
| Water | 14.1 |
| Lava | 12.5 |
| Ukiyo-e | 33.5 |

No resets, TE timeouts, or transfer underruns were observed, and IMU read failures did not increase. Free internal SRAM remained at 43,086 bytes.

This check does not establish long-term stability or visual correctness.

Clean SDK build and host simulation/display checks passed.
