# OWCS OB Analyzer

Windows x64 原生 OBS 插件，自动识别守望先锋 OB 计分板中的选手、英雄、大招、威能、生存状态和比赛统计，并通过 HTTP / WebSocket 输出结构化数据。

支持游戏、窗口、显示器、媒体和采集卡等视频源。OBS 面板显示英雄与威能中文名称，API 同时保留英文标识。模型随插件提供，无需 Python 或独立识别服务。

## 安装

适用 Windows 10/11 x64、OBS Studio 30.0 及以上。

1. 从 [Releases](https://github.com/Wine-Red/owcs-ob-analyzer/releases/latest) 下载 `OWCS-OB-Analyzer-OBS-0.5.6-windows-x64.zip` 并解压。
2. 退出 OBS，将安装包中的 `obs-plugins` 和 `data` 文件夹复制到 OBS 安装根目录，与原目录合并；也可执行包内 `Install.ps1`。
3. **每次以管理员身份启动 OBS**。
4. 在视频源的“滤镜 → 效果滤镜”中添加“OWCS 观战数据识别”，在“停靠窗口”菜单打开“OWCS 观战数据”。

本机观战可在滤镜属性中开启“自动按住 Tab”，切回游戏后使用。默认数据接口为 `http://127.0.0.1:8765/v1/state`。

详细安装步骤见 [安装指南](README_USER.md)，接口说明见 [API 文档](docs/STATE_API.md)。

## 开发

安装 Visual Studio 2022 的“使用 C++ 的桌面开发”和 CMake 组件，在 PowerShell 中执行：

```powershell
./scripts/package-plugin.ps1 -ObsPath 'C:/Program Files/obs-studio'
```

固定使用 OBS 30.0.0 SDK 和 Qt 6.5.3 构建；`-ObsPath` 仅指定原生测试使用的 OBS 安装目录，省略时使用下载的 30.0.0。

脚本下载并校验构建依赖，编译插件、运行原生测试，将安装包、源码包和 SHA256 清单输出到 `release/`。

依赖已准备好后，可分别编译测试和打包：

```powershell
./scripts/build-plugin.ps1 -ObsPath 'C:/Program Files/obs-studio' -SkipBootstrap
./scripts/package-plugin.ps1 -SkipBuild
```

- `native/core/`：定位、识别与状态更新。
- `native/runtime/`：帧处理、自动 Tab 与数据接口。
- `native/obs/`：OBS 滤镜与停靠面板。
- `native/tests/`、`tests/data/`：回归测试与样本。
- `assets/recognition/`、`data/models/`：原始识别资源与运行模型。

英雄与威能资源通过 `assets/recognition/manifest.json` 维护，使用 `scripts/build_resource_pack.py` 生成资源包，详见 [资源维护](docs/RESOURCE_PACKS.md)。
