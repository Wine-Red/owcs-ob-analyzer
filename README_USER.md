# OWCS OB Analyzer 0.5.5

守望先锋 OB 计分板识别插件，支持英雄、威能中文显示，以及 HTTP / WebSocket 数据输出。适用 Windows 10/11 x64、OBS Studio 30.0 及以上，无需另外安装 Python 或启动服务。

## 安装与使用

1. 解压安装包并退出 OBS。
2. 将 `obs-plugins` 和 `data` 两个文件夹复制到 **OBS 安装根目录**，与原目录合并。Steam 版同样适用。
3. **每次以管理员身份启动 OBS**：右键快捷方式或 `obs64.exe`，选择“以管理员身份运行”。也可在快捷方式“属性 → 高级”中勾选“用管理员身份运行”。
4. 在显示游戏画面的视频源中，打开“滤镜 → 效果滤镜 → +”，添加“OWCS 观战数据识别”。
5. 在“停靠窗口”菜单打开“OWCS 观战数据”，即可查看、复制或导出识别结果。

本机观战可在滤镜属性中开启“自动按住 Tab”，切回游戏后自动显示计分板。支持窗口、游戏、显示器、媒体文件和采集卡等视频源。

也可在解压目录使用安装脚本，自动校验文件并备份旧版本：

```powershell
powershell -ExecutionPolicy Bypass -File .\Install.ps1 -ObsPath 'C:\Program Files\obs-studio'
```

将路径替换为实际 OBS 安装目录；目录需要写入权限时使用管理员 PowerShell。升级时先退出 OBS，再安装完整新版包。卸载使用 `Uninstall.ps1 -ObsPath '实际安装目录'`。

## 开发与接入

- HTTP：`http://127.0.0.1:8765/v1/state`
- WebSocket：`ws://127.0.0.1:8765/v1/state-stream`
- 停靠面板可设置端口和局域网访问。
- 接口文档：[docs/STATE_API.md](docs/STATE_API.md)。
- 资源维护：[docs/RESOURCE_PACKS.md](docs/RESOURCE_PACKS.md)。
- 源码与构建指南：[GitHub](https://github.com/Wine-Red/owcs-ob-analyzer)。
