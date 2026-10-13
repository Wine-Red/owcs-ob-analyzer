# 精简数据 API v3

插件 0.4.0 起，HTTP、WebSocket、复制 JSON 和导出 JSON 只提供此格式；不提供详细模式。端口默认 8765，仅监听本机。接口路径仍为 `/v1`，响应 `schema_version` 升为 `3.0`，这是对旧结构的破坏性变更。

| 请求 | 说明 |
|---|---|
| GET /health | 服务状态及插件版本 |
| GET /v1/streams | 来源列表：stream_id、name、selected |
| GET /v1/state?stream=ID | 精简快照；省略 ID 使用停靠窗口所选来源 |
| WS /v1/state-stream?stream=ID | 连接时及随后约每 200ms 发送完整精简快照 |

没有来源、初始化中或核心不可用时，HTTP 返回 503；WebSocket 保持连接并等待恢复。未知来源不会退回其他来源。未知路径返回 404。接口只读，连接上限 32；允许局域网需在插件界面启用，无账号认证。

## 顶层

- `schema_version`: `3.0`。
- `stream_id`、`session_id`：来源及识别会话标识，有对应状态时提供。
- `sequence`：识别结果序号；WebSocket 重发同一结果时可不变，但过期列表会重新计算。
- `observed_at`：本次识别结果生成时间，不代表所有保留字段都重新识别过。
- `status`：online / delayed / stale / stopped / waiting / error。online 仅说明来源和处理在线，字段有效性仍看下方列表。delayed 表示结果所用帧超过 1500ms；stale 表示超过 2500ms 未收到输入。
- `panel_visible`：当前处理在线且面板已经连续确认；等待、延迟、暂停或断流时为 false。
- `players`：按计分板顺序排列的十行；尚无识别结果时为空数组。
- `error`：仅错误状态出现，提供故障原因。

## 每名选手

`slot_id`、`side`、`slot`、`identity_generation`、`display_name`、`hero`、`hero_selection`、`life_state`、`ultimate_charge`、`ultimate_ready`、`respawn_seconds`、`stats`、`perks`、`stale_fields`、`unknown_fields`。

- `side`: left / right，分别表示计分板上队 / 下队，沿用旧命名。`slot` 为该队 1–5 行，不是顶栏位置。
- 使用 `stream_id + session_id + slot_id + identity_generation` 区分行身份；不是跨地图的永久选手 ID。
- `hero_selection`: selected / unselected / unknown。确认问号头像时为 unselected，并清空旧英雄、大招、威能、生存状态。
- `life_state`: alive / dead / unknown。累计阵亡不是当前生存状态。
- `ultimate_charge`: 0–100 或 null；`ultimate_ready`: true / false / null；`respawn_seconds`: 非负整数或 null。
- `stats`: eliminations / assists / deaths / damage / healing / mitigated，均为非负整数或 null。
- `perks.minor`、`perks.major`: `{id, selection}`，selection 为 selected / unselected / not_shown / unknown。id 为模板英文名称或 null。

## 字段有效性

`stale_fields` 列出保留了历史观测、但当前不可视为实时的字段；`unknown_fields` 列出尚未确认或暂时未知的字段。列表使用相对于选手对象的路径，例如 `hero`、`life_state`、`stats.damage`、`perks.minor`。

在本版本定义的 15 个观测字段中，未出现在两个列表里的字段是有效观测。两个列表互斥。`null` 不转成 0；“未选择 / 未显示的威能”可以是已确认的有效状态，即 id 为 null 但不在 unknown_fields 中。

数值和图标新鲜期为 1000ms，名字为 15000ms；面板不可用、来源停止、断流或结果延迟时，已知字段全部标为 stale。局部漏读只影响对应字段，不使整行失效。英雄替换待确认时，英雄相关字段为 unknown，不沿用旧英雄拼接新大招。

消费示例：

```javascript
const field = 'stats.damage';
const usable = !player.stale_fields.includes(field)
            && !player.unknown_fields.includes(field);
const damage = usable ? player.stats.damage : null;
```

## 从旧格式迁移

- `players[].scoreboard.stats/perks/ultimate_charge/ultimate_ready/respawn_seconds/identity_generation` 移到选手对象直属层。
- 原 `scoreboard.fields.*` 替换为 `stale_fields` / `unknown_fields`。
- 去除重复置信度、confirmed 状态、逐字段时间、每行 updated_at、面板坐标、HUD 定位、Tab 持键、耗时、资源版本等诊断信息。
- 顶层 source_status / analysis_status 合并为 status；面板状态统一为 panel_visible；不再提供 type 字段。
- JSON Schema：`schema/player-state-v3.schema.json`。内部诊断快照不构成公开接口。
