# 首版验证记录

日期：2026-10-21（Asia/Shanghai）。在独立社区版目录执行，未烧录或连接设备。

## 固件构建

两块参考板均通过 `scripts/build.py` 构建。工具链的精确 SDK 提交和本地修复见 [TOOLCHAIN.md](TOOLCHAIN.md)。第三方组件从本仓库引用，不再引用原开发仓库的第三方目录。

| 板型 | 系统镜像字节数 | SHA-256 |
| --- | ---: | --- |
| `alientek-atk-box` | 1,958,640 | `11ac505337f03a44931dc136f008101294670ce321d126040f2156c97e34464e` |
| `waveshare-esp32-s3-touch-lcd-1-46` | 2,072,016 | `b30e097dec35d4e1a605f2e648c325d3b63170a068515a6e0e096a6b423f18aa` |

默认共享资源包为 153,658 字节，只含程序生成的壁纸，无产品语音模型或表情。

## 已通过的离线检查

- `bash examples/hello_mars/build.sh`：最小示例编译，包结构、CRC、ELF、入口和系统导出符号校验。
- `bash firmware/mars_system/components/mars_audio_stream/host_test/run_tests.sh`：ASan/UBSan 三轮，核心、服务、行解析检查；ThreadSanitizer 并发检查。
- `bash firmware/mars_system/components/mars_services/host_test/run_tests.sh`：应用偏好存储检查，ThreadSanitizer，以及启动线程专属 NVS/不可变配置读取/头注入验证。
- `python3 firmware/mars_system/tools/tests/run_app_drawer_host.py`：桌面默认配置、应用抽屉交互、动画端点和保存配置迁移。
- `python3 firmware/mars_system/tools/tests/test_bitmap_scaled.py`：缩放绘图、裁剪、越界坐标的 ASan/UBSan 检查。
- Gitleaks v8.30.1：公开提交内容扫描，未发现密钥。
- 新仓库不带原工程 Git 历史；排除了网站、手机端、独立产品应用、设备备份、用户数据和本地 SDK。

整理时修正了测试路径和缺失的测试替身。音频测试原先在首包已可能解码之后才记录基线，导致“三包全部播放但差值只有二”的竞态；现按新建下行会话归零后的总计数检查。该修复仅改主机测试，未改变生产音频实现。

## 尚未完成

社区版替换后的字体、壁纸、实际显示、触摸、实体输入、持续运行以及示例应用实机安装/返回仍需设备验证。历史产品固件的验收不能替代本快照的实机结果。没有发布通用刷机包，也未部署网站。
