# OWCS OB Analyzer — OBS 插件

Windows x64 原生 OBS 视频滤镜，当前版本 **0.4.0**。面向守望先锋 OB 观战画面，识别十人计分板并输出结构化数据。运行时无需 Python、单独服务或网络下载。

- 在 OBS 中选择任意可渲染视频源：游戏、窗口、显示器、图片、媒体、采集卡、嵌套场景等。
- 自动定位计分板和十人 HUD；提取英雄、大招、威能、生存、复活倒计时、E/A/D、伤害、治疗、抵挡及选手名。
- 本机 OB 可选自动按住 Tab；默认关闭。失去游戏焦点、停止识别、断帧或卸载滤镜时释放。
- 原生停靠窗口查看状态、复制/导出 JSON；HTTP 与 WebSocket 输出。
- 未知值保留 `null`；历史值与未知值通过 `stale_fields` / `unknown_fields` 区分。行号不是永久选手身份。

## 使用

详见 [安装与使用](README_USER.md)。Windows 10/11 x64、OBS Studio 32.2.2 起；实测版本 32.2.2。其他 OBS 大版本未验证。

安装包：`release/OWCS-OB-Analyzer-OBS-0.4.0-windows-x64.zip`。源码包与 SHA256 清单在同目录。

## 工程

```text
native/core/      定位、视觉识别、ONNX OCR、时序状态
native/runtime/   帧队列、识别线程、Tab 生命周期、HTTP/WebSocket
native/obs/       OBS 滤镜、Qt 停靠窗口、模块入口
native/tests/     原生回归测试
data/models/     随包模型和版本化模板
data/licenses/   第三方许可
tests/data/      用户截图回归样本
 scripts/         依赖、编译、测试、打包工具
```

渲染线程按设定频率读取源帧，交给容量为一的最新帧队列；后台线程识别，Qt 主线程提供面板与数据接口。不会修改原视频。默认分析 5 fps，可设 1–10 fps；慢机器会丢弃待处理旧帧，避免延迟不断增加。滤镜需处于 OBS 正在渲染的来源中。

## 从源码构建

安装 Visual Studio 2022 的“使用 C++ 的桌面开发”和 CMake 组件，使用 PowerShell：

```powershell
./scripts/package-plugin.ps1 -ObsPath 'C:/Program Files/obs-studio'
```

不传 `ObsPath` 会下载固定版本 OBS 运行包作为 SDK 配套文件，不会安装或启动它。首次构建下载 Qt 6.8.3、OpenCV 4.10、ONNX Runtime 1.20.1 和 OBS 32.2.2 头文件，验证固定 SHA256。生成匹配 DLL 的导入库，不需要自行编译整个 OBS。后续使用缓存。CI 执行相同构建和原生测试。

```powershell
./scripts/build-plugin.ps1 -ObsPath 'C:/Program Files/obs-studio' -SkipBootstrap
./scripts/package-plugin.ps1 -SkipBuild
```

英雄与威能原图、映射统一维护在 `assets/recognition/manifest.json`。`scripts/build_resource_pack.py` 生成独立版本化资源包。使用 `--output data/models` 更新仓库内模型。导出需要 Python、NumPy、OpenCV，正常编译、安装和运行不使用 Python。参见 [资源维护与安装](docs/RESOURCE_PACKS.md)。二进制模板读取支持中文安装目录。

## 验证与边界

[验证记录](docs/VALIDATION.md)；[API 契约](docs/STATE_API.md)。图片源及压缩媒体源已在隔离 OBS 实例验证，未改动原 OBS 场景。自动 Tab 已在管理员 OBS 和真实游戏中验证：用户确认自动打开，接口同步确认计分板可见和十人大招/生存数据。

当前模板针对样本中的中文默认计分板布局；HDR、强压缩、特殊 UI 缩放、改版英雄/威能、遮挡和额外黑边不保证覆盖。可在滤镜前放置 OBS 裁剪滤镜移除信号黑边。选手名 OCR 可能有同形字或繁简误读；置信度不是正确率。威能未显示与尚未选择分别输出 `not_shown`、`unselected`。

原生代码使用 GPL-3.0-or-later；游戏图像衍生特征及第三方模型不因此改换许可。来源和权利说明见 [THIRD_PARTY_NOTICES](data/THIRD_PARTY_NOTICES.md)。

0.4.0 将全部对外数据输出统一为精简 schema 3.0：业务字段移到选手对象直属层，去掉置信度、定位与性能信息；HTTP/WS 路径不变，旧调用方需按 `docs/STATE_API.md` 迁移。
