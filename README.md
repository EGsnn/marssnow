# 火星积雪 · MARSSNOW

面向 ESP32 的火星系统社区版：在资源受限的设备上运行桌面、设置、触摸交互与独立应用。

**本仓库只开源 ESP32 系统框架和最小开发示例。配套网站、手机 App、设备独立应用及云端服务不在本次开源范围内。** 网站尚在筹备，构建和基础使用无需访问官方网站。

## 首版范围

- 板级抽象、显示与输入驱动，支持正点原子 ATK-DNESP32S3-BOX 和微雪 ESP32-S3-Touch-LCD-1.46。
- 桌面、设置、Wi-Fi、BLE、时间服务和页面渲染。
- Xtensa ELF 动态应用加载、MARSAPP v2 / MARSRES 解析、应用服务 ABI。
- 音频流、Opus 与 WebSocket 基础服务；实际语音应用和云服务另行提供。
- 本地构建、应用打包与校验工具，以及最小示例。

这是从持续开发工程整理出的首个公开源码快照。社区版替换了中文字库和默认壁纸，**当前构建检查与原产品的实机验收分开记录**。不要把编译通过视为屏幕、触摸或语音已经实机验证。

| 板型 | 芯片 / 内存 | 社区版范围 |
| --- | --- | --- |
| 正点原子 ATK-DNESP32S3-BOX | ESP32-S3 / 16 MB Flash / 8 MB PSRAM | 320×240 桌面与系统框架 |
| 微雪 ESP32-S3-Touch-LCD-1.46 | ESP32-S3 / 16 MB Flash / 8 MB PSRAM | 412×412 圆屏桌面与系统框架 |

FoloToy AI Passport 的 ESP32-C3 compact 分支含有内嵌应用实现，需要进一步拆分，暂不包含在首版。请勿向 C3 设备烧录 S3 固件。

## 编译

按 [工具链基线](docs/TOOLCHAIN.md) 安装并激活 **ESP-IDF v6.1** 的 ESP32-S3 工具链（含已记录的局部 SDK 修复）。常规编译不需要安装 Pillow，也不需要网站、账号或云端密钥。固件依赖已放在 `third_party/components`，组件管理器在构建时关闭。

```sh
git clone https://github.com/EGsnn/marssnow.git
cd marssnow
. /path/to/esp-idf/export.sh
python3 scripts/build.py alientek-atk-box
# 或：
python3 scripts/build.py waveshare-esp32-s3-touch-lcd-1-46
```

输出位于 `build/<板型>/`：系统镜像 `box3_launcher.bin`、bootloader、分区表、共享资源包和 `build-manifest.json`。历史工程名称 `box3_launcher` 仅为兼容保留，**本项目的 ATK 板不是 ESP-BOX-3**。

构建脚本不连接串口、不烧录。向已有设备安装前，请先阅读 [安装与分区说明](docs/INSTALL.md)，核对设备真实分区和运行槽并备份。不要直接照抄 ESP-IDF 结尾的全量 flash 提示。

## 目录与开发

```text
firmware/mars_system/main/        系统启动、桌面、设置
firmware/mars_system/components/  驱动、显示、加载器及服务
firmware/boards/                  板型和引脚描述
firmware/ui_profiles/             UI 几何配置
examples/hello_mars/              最小独立应用示例
scripts/                         可移植构建及资源生成
third_party/components/          固定版本的第三方依赖
```

- [架构与开源边界](docs/ARCHITECTURE.md)
- [最小应用示例](examples/hello_mars/README.md)
- [验证记录](docs/VALIDATION.md)
- [贡献说明](CONTRIBUTING.md)

系统保留应用加载和服务接口，但不附带小智、火星伙伴、流体对话、时钟或海浪时钟等独立应用的源码、安装包和表情资源。未安装应用时，相应桌面入口可能打开空应用列表；这不代表已包含这些应用。

## 授权

本项目自有代码沿用 **Apache-2.0**，见 [LICENSE](LICENSE)。第三方依赖和字体按各自许可证分发，见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。部分 Espressif 音频/JPEG 库以预编译形式提供，带有仅限 Espressif 产品使用的条款，不能视为本项目 Apache-2.0 代码。

“火星积雪 / MARSSNOW”名称和标识不因代码开源自动授予商标使用权。欢迎提交问题和改进建议；报告问题时请移除 Wi-Fi 密码、访问令牌和设备个人资料。
