# Mosaico FluidBox

[English](README.md) | 简体中文

面向 ESP-Mosaico / ESP32-S31 的粒子流体固件。倾斜、摇晃或转动设备，流体随之运动；提供粒子、液体、海水、岩浆和浮世绘五种画面模式。

![粒子、液体、海水、岩浆和浮世绘模式，由主机从同一粒子状态渲染](docs/preview.png)

## 使用

- 短按 **AI 键**切换画面模式，切换时保留粒子状态。
- 运行时短按 **BOOT 键**重置粒子。
- 屏幕亮度固定为 60%，无需联网即可运行。

支持 Mosaico V1.0、V1.1 和 V1.2，通过 eFuse 自动识别板卡版本；未知或未写入版本的设备会报告错误。显示为 480×480 CO5300，运动传感器为 BMI270。暂不支持触摸操作。Iris 提供 USB 日志和恢复模式切换，固件安装由 Vibe Mode 完成。

版本 **0.1.0**。帧率随模式和姿态变化，见[测试结果](TESTING.md)。

## 烧录

从发布页下载 `mosaico-fluidbox-0.1.0.bin`。烧录会覆盖镜像地址范围内的原固件及数据，包括 NVS 设置，请先保存需要保留的内容。

1. 按住 **BOOT** 并重启或重新上电，进入 ROM 下载模式。
2. 使用 ESP-IDF 6.2 环境中支持 ESP32-S31 的 esptool，在该文件所在目录执行以下命令，将 `PORT` 替换为重新枚举后的串口：

   ```sh
   python -m esptool --chip esp32s31 --port PORT --before no-reset --after no-reset --no-stub write-flash --flash-size 16MB 0x0 mosaico-fluidbox-0.1.0.bin
   ```

3. 烧录完成后释放 BOOT 并重新上电。运行时 USB 日志端口可能与下载端口不同。

该文件是完整固件镜像，烧录地址为 `0x0`。网页烧录器需支持 ESP32-S31 和完整二进制镜像。发布说明中列出了它的 SHA-256 校验值。

### Iris 安装

使用 `mosaico-fluidbox-0.1.0.irisfw`，通过官方 Iris 安装器安装，或在 Ideas 上传时选择「添加 Iris 固件」。该包要求 Vibe Mode **0.2.0** 和 `esp-mosaico/v2` 契约。首次安装前先进入 Vibe Mode。安装会替换应用布局及代码，请先备份原应用数据。

包内包含构建生成的 4 KiB 分区表（`0x8000`）和应用（`0x210000`），保留前 2 MiB 系统分区布局，不包含 bootloader、Vibe Mode 固件、凭据或 NVS 镜像。完整有线 BIN 则包含固定版本的官方 Vibe Mode 镜像和配套产品 bootloader，烧录会覆盖镜像范围内的原有设置。

FluidBox 使用官方应用恢复组件管理 USB、安装健康确认、系统分区校验及返回 Vibe Mode，在屏幕实际显示一帧后才确认健康。TCP 和应用侧 OTA 写入均关闭。模拟与渲染算法保持原样；Iris 会增加运行时内存和后台任务，性能需要真机测量。

## 开发

使用 **ESP-IDF 6.2** 及配套 RISC-V 工具链。官方 Vibe Mode 预置包使用 [ESP-IDF 提交 7b9cc1ac79f](https://github.com/espressif/esp-idf/commit/7b9cc1ac79f865983f59bb8ff3ff43eb74ff1dbe)。初始化固定版本的[官方 ESP-Mosaico Utils](https://github.com/esp-mosaico/esp-mosaico-utils/tree/c6b43a6be7fb8aba832fc4016d4076702f5414ae)子模块后，在已激活 SDK 的终端执行：

```sh
git submodule update --init --recursive
idf.py --preview -B build-iris build
python tools/package.py --build-dir build-iris
```

打包所用 Python 环境需要 `cryptography`。脚本会检查实际构建配置，调用官方打包器和 Ideas 产品校验器，同时生成 `dist/mosaico-fluidbox-0.1.0.irisfw` 与 `dist/mosaico-fluidbox-0.1.0.bin`。旧的独立固件构建会被拒绝，避免再次直接转换成不完整的 Iris 包。构建与打包共用 `partitions.csv`。

设备操作通过工作区入口调用固定版本的官方 CLI：

```sh
python mosaico.py iris device-status
python mosaico.py iris system-update --bundle dist/mosaico-fluidbox-0.1.0.irisfw
python mosaico.py iris logs --timeout 20
python mosaico.py iris test enter-recovery
```

CLI 会准备独立的官方主机依赖。安装成功要求同一设备重新连接，固件身份、分区哈希与健康状态均通过校验。首次有线安装可使用前述完整 BIN，日常 Iris 安装则保留设备已有的 Vibe Mode。

程序代码仍从 Flash 执行，关闭 PSRAM XIP。粒子交换缓冲和渲染快照放在 PSRAM，求解器工作数组及渲染临时缓冲保留在内部 SRAM。保留 Iris 的按行输出缓冲，不将 stdout 改为无缓冲模式。

模拟在 Core 1 运行，渲染与显示在 Core 0 运行。1200 个粒子驱动各画面模式；显示使用 PSRAM 双帧缓冲及 TE 同步，按面板扫描位置逐段发送。物理参数和硬件配置位于 `main/config.h`。

主机检查在 macOS/Linux 上运行，需要 Python 3 及支持 ASan/UBSan 的 C 编译器：

```sh
python3 tools/check_host.py
python3 tools/check_display.py
```

预览另需 FFmpeg，可用 `python3 tools/preview.py out.mp4 --modes liquid,water` 生成画面对比视频。设备默认输出启动、模式切换和异常日志；帧率和耗时日志每两秒输出一次，可通过 `mosaico.py iris logs` 查看。

## 致谢与许可

感谢 [V4C38/esp32-fluidbox](https://github.com/V4C38/esp32-fluidbox) 提供粒子流体模拟、粒子渲染和固件基础。本项目在其基础上适配 Mosaico 硬件，改造显示同步，并新增液体、海水、岩浆和浮世绘画面模式。

项目采用 [MIT 许可](LICENSE)，在本项目版权声明之外保留上游 `Copyright (c) 2026 V4C38`。屏幕初始化序列改编自 [esp-mosaico/esp-mosaico-bsp](https://github.com/esp-mosaico/esp-mosaico-bsp)，经由 QuotaFrame 引入，该部分另须遵守 [Apache-2.0 许可](licenses/Apache-2.0.txt)。完整来源、对应提交和修改说明见 [NOTICE](NOTICE)。第三方依赖遵循各自的许可证。
