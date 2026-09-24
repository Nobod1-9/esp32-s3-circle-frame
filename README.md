# ESP32-S3-LCD-2.8C Circular Media Frame / 圆形媒体相框

[中文](#中文) | [English](#english)

## 中文

这是一个面向微雪 **ESP32-S3-LCD-2.8C**（480×480 圆形屏）的 Arduino 项目。它可以从手机网页上传并播放静态图片和 GIF，并使用板载 QMI8658 IMU 让静态图片保持水平、检测刹车动作。

### 功能

- 手机网页批量上传普通图片和 GIF，支持混合选择、自动识别格式和上传进度。
- Micro SD 卡持久保存最多 30 张静态图片和 30 个 GIF。
- GIF 直接保存，由 ESP32 实时解码播放，无需手机预转换帧文件。
- 静态图片根据 IMU 姿态自动保持水平。
- BOOT 单键操作：短按下一项、双击上一项、长按约 1 秒确认/进入设置、主页面长按约 3 秒休眠。
- 设置屏幕亮度、Wi-Fi 上传、手动刹车灯及日落自动刹车灯。
- 刹车时在图片边缘显示红色圆环。
- 选择“WIFI UPLOAD”后开启设备热点 `CircleFrame`（密码 `12345678`），仅用于上传和管理照片/GIF；不提供家庭 Wi-Fi 配网或保存功能。
- Wi-Fi 仅用于本地上传，不连接外部路由器；自动刹车的联网定位/校时功能因此不会启动。

### 硬件与环境

- 微雪 ESP32-S3-LCD-2.8C
- FAT32 Micro SD 卡（播放 GIF 必需，静态图片也建议使用）
- Arduino IDE
- ESP32 Arduino Core `3.3.11`
- [AnimatedGIF](https://github.com/bitbank2/AnimatedGIF) `2.2.0`

### Arduino IDE 配置

- Board：`ESP32S3 Dev Module`
- PSRAM：`OPI PSRAM`
- Flash Mode：`QIO`
- 串口监视器：`115200`

在 Arduino IDE 库管理器中搜索并安装 `AnimatedGIF` 2.2.0，然后打开 `sketch_aug31a.ino` 编译上传。

### 使用方法

1. 插入 FAT32 格式的 Micro SD 卡并启动设备。
2. 长按 BOOT 约 1 秒进入设置，选择 `WIFI UPLOAD`。
3. 手机连接热点 `CircleFrame`（密码 `12345678`），访问 `http://192.168.4.1`。
4. 多选普通图片和 GIF 后上传。设备会按顺序逐个处理文件。
5. 在“管理和删除媒体”页面可分别删除任意图片或 GIF。

建议在准备公开或用于实际产品前修改 `AP_PASSWORD`，不要把个人 Wi-Fi 密码写入源码。通过网页保存的家庭 Wi-Fi 凭据存放在 ESP32 NVS 中，不会进入 Git 仓库。

### 许可与第三方代码

本项目作者拥有版权的代码采用 [PolyForm Noncommercial License 1.0.0](LICENSE)：允许个人、研究、学习、公益及其他非商业用途，禁止商业使用。该许可证带有用途限制，因此本项目属于 **source-available（源码可见）**，并非 OSI 定义的开源软件。

显示、I2C 和 GPIO 扩展器底层代码由微雪官方示例演变而来；这些上游部分仍受其原始许可和版权约束，不因本仓库的 PolyForm 许可证而被重新许可。`AnimatedGIF` 是外部依赖，并未复制进本仓库，使用者还应遵守其自身许可证。

商业授权请联系仓库所有者。

---

## English

This Arduino project targets the Waveshare **ESP32-S3-LCD-2.8C** with its 480×480 round display. It accepts still images and GIFs from a phone browser, keeps still images level using the onboard QMI8658 IMU, and detects braking motion.

### Features

- Batch upload still images and GIFs from one mobile web page, including mixed selection, automatic format detection, and upload progress.
- Persist up to 30 still images and 30 GIFs on a Micro SD card.
- Store original GIF files and decode them on the ESP32 in real time; no browser-side frame conversion is required.
- Keep still images level using IMU orientation data.
- One-button operation: short press for next, double press for previous, hold for about 1 second to confirm/open settings, and hold for about 3 seconds on the main screen to sleep.
- Configure brightness, Wi-Fi upload, manual brake light, and sunset-based automatic brake-light availability.
- Draw a red outer ring when braking is detected.
- Save home Wi-Fi credentials and reconnect after reboot. Leaving settings disables the device access point while retaining the router connection.
- Wi-Fi is used only for local uploads; the device does not connect to an external router, so network-based automatic-brake location/time synchronization is disabled.

### Hardware and toolchain

- Waveshare ESP32-S3-LCD-2.8C
- FAT32 Micro SD card (required for GIF playback and recommended for still images)
- Arduino IDE
- ESP32 Arduino Core `3.3.11`
- [AnimatedGIF](https://github.com/bitbank2/AnimatedGIF) `2.2.0`

### Arduino IDE settings

- Board: `ESP32S3 Dev Module`
- PSRAM: `OPI PSRAM`
- Flash Mode: `QIO`
- Serial monitor: `115200`

Install `AnimatedGIF` 2.2.0 from the Arduino Library Manager, open `sketch_aug31a.ino`, then compile and upload it.

### Usage

1. Insert a FAT32-formatted Micro SD card and power on the device.
2. Hold BOOT for about 1 second to enter settings and select `WIFI UPLOAD`.
3. For first-time setup, connect to the `CircleFrame` access point using the default password `12345678`, then open `http://192.168.4.1`.
4. Alternatively, save a home Wi-Fi network on the Wi-Fi setup page. Once connected, the settings screen shows the device's LAN IP address.
5. Connect your phone to the `CircleFrame` hotspot (password `12345678`), open `http://192.168.4.1`, select multiple still images and GIFs, and upload them. Files are processed sequentially.
6. Use the media-management page to delete individual images or GIFs.

Change `AP_PASSWORD` before public or production use, and do not place personal Wi-Fi credentials in source code. Home Wi-Fi credentials saved through the web UI live in ESP32 NVS and are never committed to Git.

### License and third-party code

Code copyrighted by this project's authors is available under the [PolyForm Noncommercial License 1.0.0](LICENSE). Personal, research, educational, charitable, and other noncommercial use is permitted; commercial use is not. Because this license restricts fields of use, this repository is **source-available**, not Open Source as defined by the OSI.

Low-level display, I2C, and GPIO-expander code evolved from Waveshare's official examples. Those upstream portions remain subject to their original licenses and copyrights and are not relicensed by this repository's PolyForm license. `AnimatedGIF` is an external dependency and is not vendored here; its own license also applies.

For commercial licensing, contact the repository owner.
