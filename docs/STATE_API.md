# 实时选手状态接口 v1

OWCS OB Analyzer 默认在 `0.0.0.0:8765` 启动局域网状态服务。包装端应把
WebSocket 作为主接口；HTTP 接口用于首次检查、断线恢复和人工调试。

## 接口

- `GET /health`：服务和采集源健康状态。
- `GET /v1/state`：最近一次完整状态；尚未收到识别快照时返回 HTTP 503。
- `GET /v1/state-stream`：WebSocket 状态流。

包装端连接 WebSocket 后会立即收到 `state.snapshot` 或
`state.unavailable`。之后在已确认的英雄、存活状态或选手名称发生变化时收到
新的完整 `state.snapshot`，无变化时每秒收到 `heartbeat`。

```javascript
const socket = new WebSocket("ws://192.168.1.20:8765/v1/state-stream");

socket.onmessage = ({ data }) => {
  const message = JSON.parse(data);
  if (message.type !== "state.snapshot") return;

  for (const player of message.players) {
    // 使用 slot_id 作为稳定主键，不要使用队伍颜色。
    console.log(
      player.slot_id,
      player.display_name,
      player.hero,
      player.life_state,
    );
  }
};
```

## 正式状态约束

- `slot_id` 固定为 `left-1` 至 `left-5`、`right-1` 至 `right-5`。
- `slot` 对外从 1 开始。
- `display_name` 支持英文和中文，只包含三次一致后确认的选手 ID；确认前为
  `null`，短暂漏读不会清空已经确认的 ID。
- `display_name_status` 表示 ID 是否已经确认；
  `display_name_confidence` 是双预处理一致结果的保守置信度。
- `hero` 只包含稳定器已经确认的英雄 ID；无法确认时为 `null`。
- `life_state` 只可能是 `alive`、`dead` 或 `unknown`。
- 内部候选英雄 `pending_hero` 不会输出给包装端。
- `sequence` 在当前 `session_id` 内单调递增；客户端应忽略更小的序号。
- `source_status=stale` 表示识别源已经超过 2.5 秒没有新画面。
- `hud.status=locked` 才表示顶栏已经通过结构和英雄模板双重定位；
  `searching` 时包装端不得把状态当作新确认结果。
- `hud.offset_y_px` 是相对标准16:9顶栏坐标的物理像素纵向偏移。
- 时间使用 UTC ISO 8601，`source_age_ms` 表示最新画面距离当前的时间。

完整快照的 JSON Schema 位于
`schema/player-state-v1.schema.json`。

## 启动参数

```powershell
owcs-ob-analyzer --api-host 0.0.0.0 --api-port 8765
```

也可设置 `OWCS_OB_API_HOST` 和 `OWCS_OB_API_PORT` 环境变量。使用
`--no-api` 可以关闭状态服务。第一版没有身份验证，应只在可信的比赛局域网中
使用，并通过 Windows 防火墙限制可访问的网络。
