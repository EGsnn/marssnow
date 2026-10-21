# 可复现的工具链基线

首版在 macOS arm64、ESP-IDF v6.1、Xtensa GCC 15.2.0 上构建。
实际 ESP-IDF 提交为 `fff9895c82d744c7237be8847347bdd1b07c6643`。
本机 IDF 的 `tools/cmake/toolchain_flags.cmake` 已带一个局部修复，保存在
`patches/esp-idf-v6.1-toolchain-flags.patch`；它调整 CMake 读取工具链参数文件时的换行处理。
这不是完全未修改的上游 SDK 构建结果。

在自己的 SDK 副本中准备相同基线：

```sh
git clone https://github.com/espressif/esp-idf.git esp-idf-marssnow
cd esp-idf-marssnow
git checkout fff9895c82d744c7237be8847347bdd1b07c6643
git submodule update --init --recursive
git apply /absolute/path/to/marssnow/patches/esp-idf-v6.1-toolchain-flags.patch
./install.sh esp32s3
. ./export.sh
cd /absolute/path/to/marssnow
python3 scripts/build.py alientek-atk-box
```

不要直接修改正在用于其他产品的 SDK。SDK 源码及本补丁中的上游内容沿用 ESP-IDF 的 Apache-2.0 授权。本仓库不附带本地 SDK、Python 虚拟环境或编译器。

正常编译使用已生成的字库和壁纸；如需重新生成，可在单独 Python 环境安装 Pillow 后运行 `python3 scripts/generate_community_assets.py`。字体授权与源文件哈希见 `resources/fonts/README.md`。
