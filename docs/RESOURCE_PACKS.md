# 独立识别资源包

当前插件 0.4.0 使用资源格式 2（兼容 0.3.0 起的插件）。插件自身不运行 Python；Python、NumPy 和 OpenCV 仅用于维护者导出模板。首次安装必须使用完整插件包。

## 更新已安装插件的资源

1. 解压可信来源的资源包，退出该安装目录下的 OBS。
2. 在 PowerShell 执行 `./Install-Resources.ps1 -ObsPath 'E:/SteamLibrary/steamapps/common/OBS Studio'`，路径改成自己的 OBS 安装根目录。受保护的安装目录需要管理员权限。
3. 安装器核对格式、文件集合和 SHA256，完整复制后替换模型目录。旧资源保存在 OBS 根目录的 `owcs-plugin-backups/resources-时间-标识/models`。重启 OBS 生效，DLL 和场景配置不改变。

需要回退时，退出 OBS，将备份中的整个 models 目录恢复到 `data/obs-plugins/owcs-ob-analyzer/models`；不要混用不同资源版本的 manifest 和 qz 文件。校验值用于检查完整性，不等同于来源签名。

## 维护者添加英雄和威能

统一原始资源入口是 `assets/recognition/manifest.json`，引用路径相对于该目录。

英雄示例：

```json
{"id":"hero-id","hud":"heroes/hero-id/hud.png","portraits":["heroes/hero-id/panel.png"]}
```

`id` 是稳定的小写连字符 ID；`hud` 是官方方形英雄插画，导出器生成 HUD 裁剪变体；`portraits` 是紧贴正方形头像边缘的计分板头像，可有多个版本。只具备计分板参考图时允许 `hud: null`，但不会因此获得该英雄的 HUD 识别能力。不能用全屏截图或带文字边框的图片代替头像。全套 HUD 布局、数字字体和界面结构变化仍需要代码或模型适配。

威能示例：

```json
{"hero":"hero-id","tier":"minor","id":"Stable Perk Name","kind":"scoreboard","file":"perks/hero-id/minor/perk.png"}
```

`hero` 必须已存在，`tier` 只能是 `minor` 或 `major`。`scoreboard` 类型接受计分板的白色圆盘/黑色图案截图；`official-glyph` 接受透明底白色官方图案，导出器会生成圆盘和多个尺寸。相同威能 ID 可以登记不同图标变体；删除已失效的映射，避免旧名称继续参与匹配。当前输出使用稳定 ID，没有单独的多语言显示名称表。

修改 `resource_version` 后，在仓库根目录运行：

```powershell
python scripts/build_resource_pack.py --output .build/resource-pack/models --zip release/OWCS-Recognition-2026.10.13.1.zip
```

生成器从 `data/models` 继承经过验证的数字模型和姓名 OCR，完全从统一资源目录重建英雄、HUD、威能及未选取图标。全部参考图片由资源清单管理。尚未被 manifest 引用的 PNG 不会自动加入识别。

发布前，用新资源运行原生测试与 OBS 集成测试，再分发 ZIP 及旁边的 SHA256 文件。源代码包包含维护所需的原图与映射；识别资源包只包含运行时模型。图片权利和来源说明见 THIRD_PARTY_NOTICES.md。
