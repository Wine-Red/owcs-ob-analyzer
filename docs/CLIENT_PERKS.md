# 从本机客户端提取威能

## 当前状态（2026-10-16）

已确认本机战网客户端位于 `E:\Overwatch`，`.build.info` 中的活动版本为
`2.25.0.0.154461N`，国服 `cn`，Build Key 为
`1fb898e8e1f784272b6df1af05341f1d`。Steam 的 Overwatch 目录只有缓存。
这是本机已安装版本的证据，不能据此宣称已与战网服务器最新版本一致。

选用开源 [overtools/OWLib DataTool](https://github.com/overtools/OWLib)。
[v2.25.0.0+1151 发布说明](https://github.com/overtools/OWLib/releases/tag/v2.25.0.0%2B1151)
标明支持 2.25.0.0.154088，并修复新英雄威能提取。下载的 `toolchain-release.zip`
已与 GitHub release asset digest 对照，SHA256 为
`6bd283281cbf4df2f365fcc93804a042a23a5ce298031e715866ec465955a231`。
实际程序报告 `DataTool v2.25.0.0+1151+68397365`，位于 `.deps/owlib/bin/DataTool.exe`。
工具和完整游戏资源不纳入项目发行包；游戏图片权利仍归 Blizzard Entertainment。

初次尝试因游戏占用 CASC 索引失败。退出游戏和战网后，已从客户端导出
54 名英雄、216 张当前威能 PNG，以及中文名称和精确的当前威能引用。
本机未安装英文文本，因此英文名称通过 DataTool 从该安装构建对应的国服 CDN 补读；
图标、英雄归属、等级和中文名称均从本机离线资源读取。
DataTool 失败时仍可能返回退出码 0，脚本因此同时检查实际输出和完整性。

可复核的本地诊断文件（不纳入版本控制）：

- `.diagnostics/client-perks/list-heroes-enUS.log`：真实提取尝试及异常。
- `.diagnostics/client-perks/preflight.json`：初次活动构建、工具哈希、16 个 CASC 索引的占用检查。
- `.diagnostics/client-perks/verified-extract`：完整可复现导出、运行日志和哈希清单。
- `.diagnostics/client-perks/baseline-regression.json`：现有资源的原生识别基线。

## 关闭客户端后执行

先在战网确认更新完成，再正常退出守望先锋和战网。图片仅离线读取本机文件，
英文名称可显式启用同构建 CDN 补读；工具不结束进程，不改游戏配置，不安装资源到 OBS。
DataTool 解压目录应在游戏目录之外。

在仓库根目录运行（使用已有 Python 环境，并需要已安装的 .NET SDK 编译引用导出助手）：

```powershell
.venv/Scripts/python.exe -X utf8 scripts/extract_client_perks.py --preflight
.venv/Scripts/python.exe -X utf8 scripts/extract_client_perks.py --allow-cdn-text --raw .diagnostics/client-perks/verified-extract --output-source .build/client-perks/final-source --resource-version 2026.10.16.1
```

第一条只检查版本及文件访问。第二条执行 `list-heroes`（enUS、zhCN）、
`list-talents`（zhCN）和 `extract-hero-icons --no-names=true`。
国服使用 `--rcn=true`；仅 enUS 英雄/威能名称允许 CDN 补读，其余调用使用 `--online=false`。
不需要补读英文文本的安装可省略 `--allow-cdn-text`。DataTool 自身仍可能检查上游 keyring，
其网络请求不能等同于从 CDN 下载图片；完整日志保留了所有可见 CDN 请求。
底层导出命令还会生成英雄 GUI 和技能图片，导入器仅取当前威能引用的纹理。

原始导出默认保存到 `.diagnostics/client-perks/extracted`，候选原图目录为
`.build/client-perks/final-source`。两者均要求新目录，防止混入旧导出文件；重试时用
`--raw` 和 `--output-source` 指定不同目录。完整导出已成功但导入失败时，
可在检查问题后使用 `--import-only`，并保留相同的 `--raw`。
另一个轻量助手 `scripts/owlib_current_perks.cs` 使用 OWLib 公共 API 读取
`STUHero → 当前 PerkSet → Perk GUID → Loadout GUID / Major / Level`。
脚本使用本机 C# 编译器及 DataTool 自带运行库编译执行，无需修改或重新构建 OWLib。

如果本机未安装英文文本，enUS 导出可能缺失名称。脚本将拒绝缺失名称或英雄覆盖不足的结果；
此时可启用同版本 CDN 补读，或通过战网安装对应文本。不能用猜测翻译补齐映射。

## 映射与兼容性检查

- 以客户端 `Hero.Perks` 当前列表确定归属，不把 `list-talents` 内所有历史条目加入识别。
- 用精确的当前 Perk GUID 关联 `Major` 字段，保留原始 Level 和 Loadout GUID；不从导出顺序、名称或图案猜等级。
- 用 Texture GUID 找原图，保存 SHA256、客户端 Build/CDN Key、DataTool 二进制哈希及原始导出日志。
- 中文威能名称通过 Loadout GUID 关联，中文英雄名称通过 Hero GUID 关联。0.5.3 起随模型清单分发，OBS 显示中文，API 同时保留英文标识和中文名称；翻译不参与图像匹配。
- 要求项目内每个英雄都有两项小威能、两项大威能。遇到不同结构、缺失、冲突、纹理数组或未知纹理类型时停止，留待检查。
- 客户端中不在项目英雄表内的英雄单列报告，不宣称已支持。
- 候选清单替换全部旧威能映射，报告新增和移除项；候选目录只复制仍被引用的英雄/UI 资源和当前威能原图，不带入旧威能图片。
- 原图经 `client-glyph` 转换器验证后才生成候选资源。该检查仅确认格式可转换，不能代替实图识别验证。

当前客户端的原始 Level 为小威能 1、大威能 2。`list-talents` 中有 251 个威能历史记录，
但当前有效集合只有 216 项。仅以 Loadout GUID 关联会遇到历史冲突：西拉 `Locked In`
同时存在大威能 `0000000028EE.134` 和小威能 `0000000029A8.134`；当前 PerkSet
实际引用前者。助手读取精确引用后才允许导入。

构建独立候选模型和 ZIP：

```powershell
.venv/Scripts/python.exe -X utf8 scripts/build_resource_pack.py --source .build/client-perks/final-source --output .build/client-perks/final-models --zip release/OWCS-Recognition-2026.10.16.1-candidate.zip
```

随后用原生识别器验证历史人工标注裁片：

```powershell
.venv/Scripts/python.exe -X utf8 scripts/verify_client_perks.py --models .build/client-perks/final-models --patches .diagnostics/recognition-research/patches --report .diagnostics/client-perks/recheck-regression.json
```

该裁片集是本机已有诊断素材，未随仓库分发。其他维护者需要传入自己的已标注实图目录。
报告同时检查 ID 和 selected/unselected 状态；unknown 不算正确的未选槽位。
本次重新运行现有模型得到：24 种威能，91 个正例裁片及其三组变换共 364 次正确，
25 个未选槽位及其三组变换共 100 次正确，零失败。变换样本相互关联，不能算独立实战样本。

## 本次识别结果与接入

直接套用旧 `official-glyph` 转换器时，正例只通过 333/364，负例 100/100。
失败涉及血律 Sanguine Siphon、黑影 Life Hack 和 Cybersecurity。
客户端 PNG 的透明留白包含图案的实际偏移；旧转换器裁掉留白后重新居中，会改变计分板内的位置。
新增 `client-glyph` 类型保留完整方形画布，统一生成 22/24/26/28/30 五档尺寸，
没有按威能单独调参，也没有改变原生识别阈值。

修正后，客户端图标独立生成的模板通过正例 364/364、负例 100/100，
原生全画面与状态回归已随裁剪修复扩展至 1638 项，检查零失败；15 项提取/映射契约测试通过。
对比使用固定的原生可执行文件，哈希保存在来源记录中。
24 种有人工标注正例的威能之外，其余 192 种尚没有独立实战截图覆盖，不能宣称全量实战准确率 100%。

已更新项目内 `assets/recognition/manifest.json`、216 张 `perks/client/` 原图及
`data/models`，资源版本为 `2026.10.16.1`，共 1080 条威能模板变体。
旧的 220 张截图/官网威能图片已移出维护目录和源码发行包，只保留当前 216 张客户端原图。来源记录
`assets/recognition/client-perks-provenance.json` 保存当前引用链、构建和工具哈希、覆盖范围。
旧模型备份在 `.diagnostics/client-perks/pre-update-models`。
当前资源自 0.5.0 起随完整插件分发；资源目录和发行包已清理旧威能素材。

与旧清单相比，当前客户端名称有六项差异：

| 英雄 | 旧名称 | 当前客户端名称 |
| --- | --- | --- |
| D.Mon | Meka Mobility | MEKA Mobility |
| 生命之梭 | Sew the Seed | Sow the Seed |
| 天使 | Divine Momentum | Winged Reach |
| 路霸 | Hydrogen Exposure | Hogdrogen Exposure |
| 探奇 | Covered in Dirt | Covered In Dirt |
| 探奇 | Smart Extender | SMART Extender |

这些是以当前 GUID 关联得到的名称集合差异，不能仅凭名称变化断言具体重做时间或机制变更。

准备阶段的脚本契约测试：

```powershell
.venv/Scripts/python.exe -X utf8 scripts/test_client_perks.py
```

这些测试使用合成 OWLib JSON，覆盖 GUID 关联、历史条目排除、冲突/缺失拒绝、
导出失败但退出码为 0、候选隔离及哈希校验，不能视作真实客户端导出成功。
