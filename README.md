# Mosaico FluidBox

English | [简体中文](README.zh-CN.md)

Particle fluid firmware for ESP-Mosaico / ESP32-S31. Tilt, shake, or rotate the device to move the fluid. Choose from five visual modes: particles, liquid, water, lava, and ukiyo-e.

![Particles, liquid, water, lava, and ukiyo-e modes, rendered on the host from the same particle state](docs/preview.png)

## Usage

- Press the **AI button** to cycle through visual modes without resetting the particles.
- Press **BOOT** while the firmware is running to reset the particles.
- Screen brightness is fixed at 60%. No network connection is required.

Supports Mosaico V1.0, V1.1, and V1.2, with automatic board revision detection through eFuse. Devices with an unknown or unprogrammed revision report an error. The hardware uses a 480×480 CO5300 display and a BMI270 motion sensor. Touch controls are not supported. Iris provides USB logs and recovery transitions; Vibe Mode handles firmware installation.

Version **0.1.0**. Frame rate varies with the mode and device orientation. See the [test results](TESTING.md).

## Flashing

Download `mosaico-fluidbox-0.1.0.bin` from the release page. Flashing overwrites the existing firmware and data within the image's address range, including NVS settings. Back up anything you need to keep.

1. Hold **BOOT** and reset or power-cycle the device to enter ROM download mode.
2. In an ESP-IDF 6.2 environment with an esptool version that supports ESP32-S31, run the following command from the directory containing the file. Replace `PORT` with the serial port that appears in download mode:

   ```sh
   python -m esptool --chip esp32s31 --port PORT --before no-reset --after no-reset --no-stub write-flash --flash-size 16MB 0x0 mosaico-fluidbox-0.1.0.bin
   ```

3. Release BOOT and power-cycle the device after flashing. The USB logging port used at runtime may differ from the download port.

The file is a full firmware image flashed at `0x0`. A web flasher must support ESP32-S31 and full binary images. The release notes list its SHA-256 checksum.

### Iris installation

Use `mosaico-fluidbox-0.1.0.irisfw` with the official Iris installer or the Ideas **Add Iris firmware** option. The package requires Vibe Mode **0.2.0** and the `esp-mosaico/v2` contract. Enter Vibe Mode before the first installation. Installing replaces the application layout and code; back up existing application data first.

The package contains the built 4 KiB partition table at `0x8000` and application at `0x210000`, preserving the first 2 MiB system layout. It supplies no bootloader, Vibe Mode firmware, credentials, or NVS image. The complete wired BIN includes the pinned official Vibe Mode image and matching product bootloader; flashing it overwrites existing settings within its range.

FluidBox uses the official application recovery adapter for USB transport, installation health confirmation, system inventory, and returning to Vibe Mode. It confirms health only after the display presents a frame. TCP and the application-side OTA writer are disabled. Simulation and rendering algorithms are unchanged; Iris adds runtime memory and background tasks, so performance must be measured on hardware.

## Development

Use **ESP-IDF 6.2**, with its matching RISC-V toolchain. The official Vibe Mode bundle uses [ESP-IDF commit 7b9cc1ac79f](https://github.com/espressif/esp-idf/commit/7b9cc1ac79f865983f59bb8ff3ff43eb74ff1dbe). Initialize the pinned [ESP-Mosaico Utils](https://github.com/esp-mosaico/esp-mosaico-utils/tree/c6b43a6be7fb8aba832fc4016d4076702f5414ae) submodule, then build in an activated SDK terminal:

```sh
git submodule update --init --recursive
idf.py --preview -B build-iris build
python tools/package.py --build-dir build-iris
```

Packaging requires `cryptography` in that Python environment. It validates the effective Mosaico application configuration, uses the official bundle builder and Ideas product validator, and generates both `dist/mosaico-fluidbox-0.1.0.irisfw` and `dist/mosaico-fluidbox-0.1.0.bin`. Old standalone builds are rejected instead of being converted into Iris packages. `partitions.csv` is shared by the build and packager.

Device operations use the pinned official CLI through the workspace launcher:

```sh
python mosaico.py iris device-status
python mosaico.py iris system-update --bundle dist/mosaico-fluidbox-0.1.0.irisfw
python mosaico.py iris logs --timeout 20
python mosaico.py iris test enter-recovery
```

The CLI prepares its own pinned host dependencies. Installation success requires the same device to reconnect with the matching firmware identity, partition hashes, and healthy state. The initial wired installation can use the full BIN described above; normal Iris installation preserves Vibe Mode in place.

Program code remains in Flash with PSRAM XIP disabled. Particle exchange buffers and the renderer snapshot use PSRAM; the solver working arrays and rendering scratch stay in internal SRAM. Preserve Iris line buffering instead of switching stdout to unbuffered mode.

Simulation runs on Core 1; rendering and display updates run on Core 0. All visual modes use the same 1,200 particles. The display uses two PSRAM frame buffers and TE synchronization, sending bands according to the panel's scan position. Physics parameters and hardware configuration are in `main/config.h`.

Host checks run on macOS/Linux and require Python 3 and a C compiler with ASan/UBSan support:

```sh
python3 tools/check_host.py
python3 tools/check_display.py
```

Previews also require FFmpeg. Run `python3 tools/preview.py out.mp4 --modes liquid,water` to generate a video comparing two modes. By default, the device logs startup, mode changes, and errors or warnings. Frame rate and timing logs are emitted every two seconds and are available through `mosaico.py iris logs`.

## Acknowledgments and licensing

Thanks to [V4C38/esp32-fluidbox](https://github.com/V4C38/esp32-fluidbox) for the particle simulation, particle renderer, and firmware foundation. This project adapts that foundation to Mosaico hardware, changes the display synchronization pipeline, and adds liquid, water, lava, and ukiyo-e visual modes.

The project uses the [MIT License](LICENSE) and retains the upstream `Copyright (c) 2026 V4C38` notice alongside this project's own. The display initialization sequence is adapted from [esp-mosaico/esp-mosaico-bsp](https://github.com/esp-mosaico/esp-mosaico-bsp), via QuotaFrame; that portion is also subject to the [Apache-2.0 License](licenses/Apache-2.0.txt). See [NOTICE](NOTICE) for source repositories, revisions, and modifications. Third-party dependencies retain their respective licenses.
