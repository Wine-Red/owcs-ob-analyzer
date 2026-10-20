# 实时选手 API v4

插件 0.6.0 起，HTTP、WebSocket、复制 JSON 和导出 JSON 统一使用 `schema_version: "4.0"`。只输出一份 `players` 列表，每位选手只出现一次；当前数据和保留的历史都在同一条记录里。接口路径仍为 `/v1`。

## 接入

| 请求 | 说明 |
|---|---|
| GET /health | 服务状态、插件版本 |
| GET /v1/streams | 来源列表及所选来源 |
| GET /v1/state?stream=ID | 完整快照；省略 ID 使用面板所选来源 |
| WS /v1/state-stream?stream=ID | 连接时及随后约每 200ms 发送完整快照 |

默认端口 8765，仅监听本机；可在插件界面允许局域网。接口只读，无账号认证，连接上限 32。没有来源、初始化中或核心不可用时 HTTP 返回 503，WebSocket 等待恢复；未知来源不会回退到其他来源。

## 选手列表

以 `player_id` 绑定选手，不以数组下标、队伍或画面行号绑定。ID 为 `name:` 加 NFKC 规范化、忽略大小写的已确认昵称；名单未匹配或候选过近时使用确认后的 OCR 名字。等待玩家和从未确认身份的行不建立选手记录。完全同名的不同人无法仅凭画面区分。

列表按当前或最后确认的面板位置排列，已确认离场的历史置后。顺序可能随补位变化，但 ID 和数据归属不变。不锁定十人，不推断比赛周期，不自动累加。会话内最多保留 100 位选手，优先淘汰最早的不在场记录。

| 字段 | 含义 |
|---|---|
| player_id、display_name | 选手身份和展示名 |
| team | A：面板上方/左侧；B：下方/右侧；未确认时 null。暂未确认或离场时保留最后确认的归属，确认换边后更新；不表示实际俱乐部 |
| presence_status | present：当前确认在场；unconfirmed：遮挡、复核或面板不可用；missing：最后所在侧的占位身份均已确认，但此人未出现。不推断掉线原因 |
| hero、hero_name_zhCN | 英雄标识和中文名，未知为 null |
| role | tank / damage / support / null，来自已确认英雄，随英雄变化 |
| hero_selection | selected / unselected / null，区分选中英雄、明确未选择和未知 |
| life_state | alive / dead / null |
| ultimate_charge、ultimate_ready | 大招百分比和是否就绪，未知为 null |
| respawn_seconds | 复活剩余秒数，未知为 null |
| perks.minor、perks.major | 已确认时为 `{id, name_zhCN, selection}`；selection 为 selected / unselected / not_shown。未知时整个 tier 为 null |
| stats | eliminations / assists / deaths / damage / healing / mitigated，未知为 null，不能当作 0 |
| stale_fields | 来自历史、当前未重新确认的字段路径；空数组表示没有历史字段 |

`hero_name_zhCN` 和 `role` 随 `hero` 的历史标记；威能名称随 `perks.minor` / `perks.major`。`team` 是否为当前确认的归属看 `presence_status`，不新增独立状态。

暂停只遮住部分字段时，清晰字段继续更新，被遮挡字段保留并标为历史；姓名归属不确定时保留该选手自己的历史，不接收其他占位者的数值。重连后使用新的已确认读数，包括 0 和低于历史的值，不相加。

例如，以下为一条记录的节选：

```json
{
  "player_id": "name:insane",
  "display_name": "Insane",
  "team": "B",
  "presence_status": "present",
  "hero": "tracer",
  "stats": {"eliminations": 4, "assists": 0, "deaths": 4, "damage": 3789, "healing": 0, "mitigated": 0},
  "stale_fields": ["stats.damage"]
}
```

这里伤害是最后一次确认的 3789；其他已知、未标历史的字段可作为当前有效观测。仅需持续展示时可直接显示保留值；需要实时性时检查对应路径是否在 `stale_fields` 中。

字段重新识别并确认后，会自动从 `stale_fields` 中移除，即使确认后的数值与原来相同。空数组只表示没有历史标记，不表示所有字段都有值；尚未确认的字段仍可能为 null。

## 快照信息

顶层只包含快照元数据和 `players`：

- `schema_version`：4.0；严格 JSON Schema 见 `schema/player-state-v4.schema.json`。
- `stream_id`、`session_id`：区分来源和手动重置后的识别会话。有对应状态时提供；不自动识别比赛边界。
- `sequence`、`observed_at`：快照序号和生成时间，不表示所有历史字段刚刚更新。
- `status`：online / delayed / stale / stopped / waiting / error，描述来源和处理状态。
- `panel_visible`：当前面板是否有效；游戏暂停但面板仍可读时可以为 true。
- `game_paused`：检测到支持的中文暂停横幅时为 true，未检测到为 false，面板不可用为 null。
- `error`：仅错误状态提供原因。

面板消失、来源停止或结果过期时，选手仍在 `players` 中，`presence_status` 为 unconfirmed，已知观测标为历史。选手从未确认过的字段用 null 表示，不另输出 `unknown_fields`。

## 清空和迁移

“清空数据”立即清除所选来源的选手和历史；新画面建立新的 `session_id`。后台旧结果不能重新写入。名单、接口设置和其他来源不受影响。修改名单或关闭再开启识别也会重置会话。切换比赛时如需彻底丢弃上一场历史，可手动清空。

从 v3 迁移：只遍历 `players`，按 `stream_id + session_id + player_id` 绑定。删除对 `players_by_id`、`player_order`、`retained_players` 的读取；不再输出 side、slot、slot_id、position、identity_generation、identity_status、slot_status 和 historical_*。不再用数组长度表示当前在场人数，需要时筛选 `presence_status == "present"`。未知字段改为 null；历史仍使用 `stale_fields`。

```javascript
for (const player of state.players) {
  const key = `${state.stream_id}:${state.session_id}:${player.player_id}`;
  // 按 key 更新同一个选手；player.team 用于分队展示。
}
```
