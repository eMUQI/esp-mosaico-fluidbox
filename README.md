# Mosaico FluidBox

English | [简体中文](README.zh-CN.md)

Particle fluid firmware for ESP-Mosaico / ESP32-S31. Tilt, shake, or rotate the device to move the fluid. Choose from five visual modes: particles, liquid, water, lava, and ukiyo-e.

![Particles, liquid, water, lava, and ukiyo-e modes, rendered on the host from the same particle state](docs/preview.png)

## Usage

- Press the **AI button** to cycle through visual modes without resetting the particles.
- Press **BOOT** while the firmware is running to reset the particles.
- Screen brightness is fixed at 60%. No network connection is required.

Supports Mosaico V1.0, V1.1, and V1.2, with automatic board revision detection through eFuse. Devices with an unknown or unprogrammed revision report an error. The hardware uses a 480×480 CO5300 display and a BMI270 motion sensor. Touch controls and OTA updates are not supported.

Version **0.1.0**. Frame rate varies with the mode and device orientation. See the [test results](TESTING.md).

## Flashing

Download `mosaico-fluidbox-0.1.0.bin` from the release page. Flashing overwrites the existing firmware and data within the image's address range, including NVS settings. Back up anything you need to keep.

1. Hold **BOOT** and reset or power-cycle the device to enter ROM download mode.
2. In an ESP-IDF 6.1 environment with an esptool version that supports ESP32-S31, run the following command from the directory containing the file. Replace `PORT` with the serial port that appears in download mode:

   ```sh
   python -m esptool --chip esp32s31 --port PORT --before no-reset --after no-reset --no-stub write-flash --flash-size 16MB 0x0 mosaico-fluidbox-0.1.0.bin
   ```

3. Release BOOT and power-cycle the device after flashing. The USB logging port used at runtime may differ from the download port.

The file is a full firmware image flashed at `0x0`. A web flasher must support ESP32-S31 and full binary images. The release notes list its SHA-256 checksum.

## Development

Install **ESP-IDF v6.1** using the [Espressif installation guide](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s31/get-started/index.html), then open a terminal with that environment activated. Run these commands from the repository root:

```sh
idf.py --preview build
python tools/package.py
```

The target chip and default configuration are defined in the project. The first build downloads the dependencies pinned in `dependencies.lock`. ESP32-S31 requires the `--preview` flag in this SDK version.

The full firmware image is written to `dist/mosaico-fluidbox-0.1.0.bin`. If you use `idf.py -B` to select another build directory, pass `--build-dir DIRECTORY` to the packaging command.

Simulation runs on Core 1; rendering and display updates run on Core 0. All visual modes use the same 1,200 particles. The display uses two PSRAM frame buffers and TE synchronization, sending bands according to the panel's scan position. Physics parameters and hardware configuration are in `main/config.h`.

Host checks run on macOS/Linux and require Python 3 and a C compiler with ASan/UBSan support:

```sh
python3 tools/check_host.py
python3 tools/check_display.py
```

Previews also require FFmpeg. Run `python3 tools/preview.py out.mp4 --modes liquid,water` to generate a video comparing two modes. By default, the device logs startup, mode changes, and errors or warnings. Frame rate and timing logs are Debug-level lines in `main/main.c`; change those two `ESP_LOGD` calls to `ESP_LOGI` to print them. Raising the global ESP-IDF log level to Debug instead stops the device from booting.

## Acknowledgments and licensing

Thanks to [V4C38/esp32-fluidbox](https://github.com/V4C38/esp32-fluidbox) for the particle simulation, particle renderer, and firmware foundation. This project adapts that foundation to Mosaico hardware, changes the display synchronization pipeline, and adds liquid, water, lava, and ukiyo-e visual modes.

The project uses the [MIT License](LICENSE) and retains the upstream `Copyright (c) 2026 V4C38` notice alongside this project's own. The display initialization sequence is adapted from [esp-mosaico/esp-mosaico-bsp](https://github.com/esp-mosaico/esp-mosaico-bsp), via QuotaFrame; that portion is also subject to the [Apache-2.0 License](licenses/Apache-2.0.txt). See [NOTICE](NOTICE) for source repositories, revisions, and modifications. Third-party dependencies retain their respective licenses.
