# Test results

A rough check on Mosaico / ESP32-S31 with unmodified ESP-IDF v6.1, 1,200 particles and QSPI at 60 MHz. Measured on the released source, with the two performance log lines in `main/main.c` raised from Debug to Info so they print.

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
