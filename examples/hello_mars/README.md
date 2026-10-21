# Hello Mars

独立于产品应用的最小公开示例：显示文字，等待 1.5 秒，然后请求返回桌面。
需先激活 ESP-IDF v6.1 的 ESP32-S3 工具链，再从仓库根目录运行：

```sh
bash examples/hello_mars/build.sh
```

脚本在 `build/examples/hello_mars` 输出 ELF 和 `.marsapp`，并校验包结构、重定位与导入符号。它不安装或烧录应用。实机运行与返回交互须另行验证，安装前见 `docs/INSTALL.md`。
