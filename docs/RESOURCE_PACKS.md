# 独立识别资源包

当前插件 0.5.5 使用资源格式 3（人物区域掩码）；不兼容旧版插件，首次升级需安装完整 0.5.0 或更新的插件包。插件自身不运行 Python；Python、NumPy 和 OpenCV 仅用于维护者导出模板。首次安装必须使用完整插件包。

资源 2026.10.16.2 增加英雄和威能中文显示名：源清单条目的 `name_zhCN` 在构建时写入模型清单 `names_zhCN`。显示中文需要插件 0.5.3 或更新版本；旧版 0.5.x 可继续读取该资源，但仍显示英文。中文缺失时不会影响匹配标识，面板回退英文，API 中文字段为 null。

## 更新已安装插件的资源

1. 解压可信来源的资源包，退出该安装目录下的 OBS。
2. 在 PowerShell 执行 `./Install-Resources.ps1 -ObsPath 'E:/SteamLibrary/steamapps/common/OBS Studio'`，路径改成自己的 OBS 安装根目录。受保护的安装目录需要管理员权限。
3. 安装器核对格式、文件集合和 SHA256，完整复制后替换模型目录。旧资源保存在 OBS 根目录的 `owcs-plugin-backups/resources-时间-标识/models`。重启 OBS 生效，DLL 和场景配置不改变。

需要回退时，退出 OBS，将备份中的整个 models 目录恢复到 `data/obs-plugins/owcs-ob-analyzer/models`；不要混用不同资源版本的 manifest 和 qz 文件。校验值用于检查完整性，不等同于来源签名。

## 维护者添加英雄和威能

统一原始资源入口是 `assets/recognition/manifest.json`，引用路径相对于该目录。

英雄示例：

```json
{"id":"hero-id","hud":"heroes/hero-id/hud.png","portraits":["heroes/hero-id/panel-blue.png","heroes/hero-id/panel-red.png"]}
```

`id` 是稳定的小写连字符 ID；`hud` 是官方方形英雄插画，导出器生成 HUD 裁剪变体；`portraits` 是紧贴正方形头像边缘的计分板头像，可有多个版本。只具备计分板参考图时允许 `hud: null`，但不会因此获得该英雄的 HUD 识别能力。不能用全屏截图或带文字边框的图片代替头像。全套 HUD 布局、数字字体和界面结构变化仍需要代码或模型适配。

格式 3 在 `foreground.qz` 保存逐模板人物掩码。`hud` 必须带有效透明通道；只有不透明的原图不能自动作为人物区域。导出器将透明插画的人物轮廓用于同构图的计分板参考图，HUD 缩放/翻转时同步变换掩码并排除轮廓附近的背景梯度。运行时仅用人物像素计算均值、方差和相似度，不以红蓝背景帮助匹配。

若计分板构图不同，可增加与 `portraits` 一一对应的 `portrait_masks` 图片路径：白色保留人物、黑色排除背景，尺寸须与原图相同。没有透明插画时，成对同构图的不同底色头像可通过差异提取背景；无透明通道、无配对参考、也无显式掩码的单张头像会被拒绝，避免悄悄退回含背景识别。维护后必须跑不同底色回归。

威能示例：

```json
{"hero":"hero-id","tier":"minor","id":"Stable Perk Name","kind":"client-glyph","file":"perks/client/hero-id/minor/texture.png"}
```

`hero` 必须已存在，`tier` 只能是 `minor` 或 `major`。`scoreboard` 类型接受计分板的白色圆盘/黑色图案截图；`official-glyph` 接受透明底白色官方图案，导出器会生成圆盘和多个尺寸。相同威能 ID 可以登记不同图标变体；删除已失效的映射，避免旧名称继续参与匹配。当前输出使用稳定 ID，没有单独的多语言显示名称表。

`client-glyph` 接受从客户端直接导出的原始方形纹理，保留透明留白及图案偏移，生成五种计分板尺寸。不能先裁成紧贴图案的图片。资源版本 `2026.10.16.1` 的 216 项威能均使用此类型，并以客户端当前名称替换旧名称；详见 [客户端提取与验证](CLIENT_PERKS.md)。这只改变维护端模板生成，运行时资源格式仍为 3。

修改 `resource_version` 后，在仓库根目录运行：

```powershell
python scripts/build_resource_pack.py --output .build/resource-pack/models --zip release/OWCS-Recognition-2026.10.16.1.zip
```

生成器从 `data/models` 继承经过验证的数字模型和姓名 OCR，完全从统一资源目录重建英雄、HUD、威能及未选取图标。全部参考图片由资源清单管理。资源目录仅保留 manifest 引用的当前威能原图；旧截图与官网威能图已移出。`scoreboard` 和 `official-glyph` 转换器保留给外部兼容资源使用，当前项目不再使用这两类威能素材。维护检查会拒绝未引用的威能文件或不匹配的原图哈希。

发布前，用新资源运行原生测试与 OBS 集成测试，再分发 ZIP 及旁边的 SHA256 文件。源代码包包含维护所需的原图与映射；识别资源包只包含运行时模型。图片权利和来源说明见 THIRD_PARTY_NOTICES.md。
