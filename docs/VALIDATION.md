# 构建与验证

适用版本：0.4.0。环境：Windows x64、Visual Studio 2022、OBS 32.2.2。运行时无需 Python；下面的 Python 工具仅用于开发验证和资源维护。

## 原生识别与状态回归

```powershell
./scripts/build-plugin.ps1 -ObsPath 'C:/Program Files/obs-studio'
```

脚本编译 Release 并运行 CTest；已准备依赖时可加 `-SkipBootstrap`。当前原生回归 844 项断言，覆盖：

- 英雄、问号未选状态、大招、死亡倒计时、威能和六项统计。
- 面板缩放/平移、HUD 垂直偏移与改色、无 HUD 和单边队伍拒绝。
- 粘连数字、短暂漏读、字段过期、行身份变更及英雄切换。
- 精简 schema 3.0 的业务值、过期/未知列表、诊断字段隔离。

`tests/data/scoreboard/expected.json` 为人工核对预期；各 PNG 均由当前回归引用。问号参考图与回归画面来自同组用户裁片，恢复全画幅位置后的变换测试不是独立设备验证。固定样本通过不能外推为任意赛事的准确率。

## 资源工具

开发 Python 依赖：`numpy opencv-python aiohttp jsonschema`。

```powershell
python -m unittest discover -s scripts -p test_resource_pack.py
./scripts/test-resource-install.ps1
```

资源映射/归档有 7 项 Python 测试；安装器有 5 项隔离检查，覆盖格式、路径、校验拒绝和有效安装/备份。

## OBS 集成

只对独立便携 OBS 执行，不能指向日常直播实例。准备：

1. 解压独立 OBS 到 `.diagnostics/obs-portable`，使用 `--portable --multi` 启动。
2. 创建名为 `Test` 的场景；打开预览。OBS 自带 WebSocket 设为端口 4457，测试实例关闭认证；不要用于对外服务。
3. 将编译后的插件安装到该目录；插件数据接口设为端口 8766，仅监听本机。
4. 运行 `python scripts/test-obs-integration.py`。脚本创建/删除 Fixture、MediaFixture 测试来源，禁用自动 Tab。

覆盖图片与媒体源、原画面像素不变、HTTP/WS 首帧和后续快照、schema 3.0、多源、暂停/移除、问号与英雄切换、粘连数字与死亡头像。报告输出到被 Git 忽略的 `.diagnostics`。HUD 锁定和 Tab 持键不是公开 API 字段，HUD 算法由原生测试覆盖。

## 发布检查与边界

- 构建和上述检查通过后运行 `scripts/package-plugin.ps1 -SkipBuild`，得到安装包、源码包和 SHA256 清单。
- 安装包只包含插件运行依赖、许可证、使用/API/资源说明及当前 schema；不分发回归图片、旧程序或诊断记录。
- 安装器校验包内文件并备份覆盖内容。发布前校验归档、清单和中文路径安装。
- 自动 Tab 曾在真实游戏及管理员 OBS 中由用户确认生效；当前集成不模拟真实游戏按键。
- 未覆盖所有设备、实体采集卡、HDR、任意字体/语言、超宽屏、赛事横向重排及长时间直播压力。姓名 OCR 可能存在同形字误读。
