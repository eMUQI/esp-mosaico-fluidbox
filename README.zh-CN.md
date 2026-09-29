# Mosaico FluidBox

[English](README.md) | 简体中文

面向 ESP-Mosaico / ESP32-S31 的粒子流体固件。倾斜、摇晃或转动设备，流体随之运动；提供粒子、液体、海水、岩浆和浮世绘五种画面模式。

![粒子、液体、海水、岩浆和浮世绘模式，由主机从同一粒子状态渲染](docs/preview.png)

## 使用

- 短按 **AI 键**切换画面模式，切换时保留粒子状态。
- 运行时短按 **BOOT 键**重置粒子。
- 屏幕亮度固定为 60%，无需联网即可运行。

支持 Mosaico V1.0、V1.1 和 V1.2，通过 eFuse 自动识别板卡版本；未知或未写入版本的设备会报告错误。显示为 480×480 CO5300，运动传感器为 BMI270。暂不支持触摸操作或 OTA 更新。

版本 **0.1.0**。帧率随模式和姿态变化，见[测试结果](TESTING.md)。

## 烧录

从发布页下载 `mosaico-fluidbox-0.1.0.bin`。烧录会覆盖镜像地址范围内的原固件及数据，包括 NVS 设置，请先保存需要保留的内容。

1. 按住 **BOOT** 并重启或重新上电，进入 ROM 下载模式。
2. 使用 ESP-IDF 6.1 环境中支持 ESP32-S31 的 esptool，在该文件所在目录执行以下命令，将 `PORT` 替换为重新枚举后的串口：

   ```sh
   python -m esptool --chip esp32s31 --port PORT --before no-reset --after no-reset --no-stub write-flash --flash-size 16MB 0x0 mosaico-fluidbox-0.1.0.bin
   ```

3. 烧录完成后释放 BOOT 并重新上电。运行时 USB 日志端口可能与下载端口不同。

该文件是完整固件镜像，烧录地址为 `0x0`。网页烧录器需支持 ESP32-S31 和完整二进制镜像。发布说明中列出了它的 SHA-256 校验值。

## 开发

按照 [Espressif 安装指南](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s31/get-started/index.html)安装 **ESP-IDF v6.1**，并打开已激活该环境的终端。在源码仓库根目录执行：

```sh
idf.py --preview build
python tools/package.py
```

目标芯片和默认配置已写入项目，首次构建会按 `dependencies.lock` 下载依赖。ESP32-S31 在该 SDK 中需要 `--preview` 参数。

完整固件镜像生成在 `dist/mosaico-fluidbox-0.1.0.bin`。使用 `idf.py -B` 指定其他构建目录时，打包命令附加 `--build-dir 目录`。

模拟在 Core 1 运行，渲染与显示在 Core 0 运行。1200 个粒子驱动各画面模式；显示使用 PSRAM 双帧缓冲及 TE 同步，按面板扫描位置逐段发送。物理参数和硬件配置位于 `main/config.h`。

主机检查在 macOS/Linux 上运行，需要 Python 3 及支持 ASan/UBSan 的 C 编译器：

```sh
python3 tools/check_host.py
python3 tools/check_display.py
```

预览另需 FFmpeg，可用 `python3 tools/preview.py out.mp4 --modes liquid,water` 生成画面对比视频。设备默认输出启动、模式切换和异常日志；帧率和耗时日志是 `main/main.c` 中的两行 Debug 级别日志，将这两处 `ESP_LOGD` 改为 `ESP_LOGI` 即可输出。不要把 ESP-IDF 全局日志级别调到 Debug，否则设备无法启动。

## 致谢与许可

感谢 [V4C38/esp32-fluidbox](https://github.com/V4C38/esp32-fluidbox) 提供粒子流体模拟、粒子渲染和固件基础。本项目在其基础上适配 Mosaico 硬件，改造显示同步，并新增液体、海水、岩浆和浮世绘画面模式。

项目采用 [MIT 许可](LICENSE)，在本项目版权声明之外保留上游 `Copyright (c) 2026 V4C38`。屏幕初始化序列改编自 [esp-mosaico/esp-mosaico-bsp](https://github.com/esp-mosaico/esp-mosaico-bsp)，经由 QuotaFrame 引入，该部分另须遵守 [Apache-2.0 许可](licenses/Apache-2.0.txt)。完整来源、对应提交和修改说明见 [NOTICE](NOTICE)。第三方依赖遵循各自的许可证。
