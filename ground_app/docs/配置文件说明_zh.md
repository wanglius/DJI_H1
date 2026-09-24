# 配置文件说明

配置格式为 UTF-8 JSON；允许 UTF-8 BOM，不允许注释或末尾多余逗号。`schema_version` 必须为 1。配置文件是运行输入，修改后重新连接实时模式；API 端口在下次启动 GUI 时生效。

## MQTT

| 字段 | 默认/示例 | 含义 |
|---|---|---|
| `mqtt.host` | `mqtt.example.com` | MQTT listener 主机，不是 EMQX 管理网页 URL |
| `mqtt.port` | 1883 | 实际 listener 端口；TLS 常见为 8883，以部署为准 |
| `mqtt.username` / `password` | 空 | MQTT 客户端凭据；与管理后台管理员账号不是一回事 |
| `mqtt.client_id` | 空 | 空值自动生成随机唯一 ID；手填时每个同时在线客户端必须不同 |
| `mqtt.uplink_topic` | `dji-h1/test/up` | 精确订阅主题，本版本拒绝 `+`、`#` 通配符 |
| `mqtt.ack_topic` | `dji-h1/test/down` | B 板接收 DTA1 的精确主题 |
| `mqtt.subscribe_qos` | 1 | 请求订阅 QoS，可选 0/1 |
| `mqtt.expected_uplink_qos` | 1 | 期望收到的 QoS；不匹配计数，不直接丢消息 |
| `mqtt.ack_qos` | 0 | 地面 DTA1 的 MQTT QoS，可选 0/1 |
| `mqtt.ack_enabled` | true | 主地面接收器应为 true；仅观察客户端可设 false |
| `mqtt.keepalive_seconds` | 30 | 5～3600 秒 |
| `mqtt.tls` | false | 开启后使用证书校验，不提供忽略证书错误的开关 |
| `mqtt.tls_ca_file` | null | 自定义 CA 文件；null 使用系统 CA |
| `mqtt.source_id` | null | 指定设备 ID；null 不筛选 |
| `mqtt.mission_id` | null | 指定任务 ID；null 不筛选 |
| `mqtt.max_inflight` | 512 | 同时重组的逻辑消息上限，1～4096 |
| `mqtt.ingress_queue_size` | 512 | MQTT payload 入口队列，1～8192 |
| `mqtt.ack_queue_size` | 512 | ACK 出口队列，1～8192 |
| `mqtt.reassembly_timeout_seconds` | 30 | 分片重组闲置超时，1～600 秒 |

设备/任务 ID 可填整数或 `"0x11223344"` 字符串，范围为非零 uint64。过滤的消息不会获得成功 DTA1。应另有负责该设备的接收器，否则其发送池中的数据只能重试或过期。

M100M 的 DTM1 保留 MQTT publication 边界，不使用 reassembly_timeout_seconds。仅旧透明 DTU 的 DTF2 可能被拆成多个 MQTT payload，接收器按精确主题维护连续字节流。对多个独立 DTU，推荐使用不同上行主题并分别创建接收器，尤其要避免旧 DTF2 的半个分片交错。新固件默认 topic 为 `dji-h1/m100m/up`、`dji-h1/m100m/down`，须与固件本地 JSON 对齐。

## 地图与显示

| 字段 | 默认 | 含义 |
|---|---|---|
| `map.enabled` | false | 是否加载百度卫星底图 |
| `map.ak` | 空 | 百度浏览器端 AK，只有 map.enabled=true 时使用 |
| `map.allow_mission_coordinates` | false | 是否把航迹、光谱位置和事件坐标交给百度脚本 |
| `map.load_timeout_ms` | 15000 | 地图加载期限，1000～120000 ms |
| `viewer.refresh_ms` | 1000 | GUI 刷新间隔，100～60000 ms |
| `viewer.timezone_name` | Asia/Shanghai | 实时数据显示时区名称 |
| `viewer.timezone_offset_minutes` | 480 | 相对 UTC 的显示偏移，-720～840 分钟 |

需要卫星图和航迹叠加时，将两个地图开关均设为 true，并填写 AK；这代表允许地图服务处理显示区域和任务坐标。两个开关都为 false 时无需 WebEngine、网络或 AK。地图异常只显示在地图面板，自动保留离线图，无阻塞式弹窗，不停止 MQTT、文件接收或光谱跟随。

`map` 段错误（如 AK 无效、timeout 非法）使地图退回离线模式，其余合法配置仍生效。整个 JSON 文件语法错误或 MQTT 配置错误则拒绝启动，因为无法可靠读取接收配置。

当前保留 WGS84 原坐标显示，尚未做 BD09 转换；在中国境内与百度底图可能有偏移。原始记录绝不因地图显示而改写。离线 SD 模式优先使用任务自身的时区元数据；UTC 数值不加减时区偏移。

## API 与存储

| 字段 | 默认 | 含义 |
|---|---|---|
| `api.enabled` | true | 开启本机只读 HTTP API |
| `api.port` | 8765 | 0～65535；0 表示系统选择空闲端口 |
| `storage.enabled` | true | 保存地面 SQLite 记录 |
| `storage.directory` | ../data | 相对于配置文件目录，例如 config/../data |
| `storage.output_queue_size` | 512 | Python 消息消费队列，1～8192；溢出计数，接收不阻塞 |

HTTP 默认绑定 `127.0.0.1`，不暴露到局域网。Python 低层 `start_http_api` 可指定 host，但本版不提供认证层；默认用于同机集成。

GUI 只使用快照，不启用消息消费队列，所以不消费 Python 消息也不会在 GUI 中产生假输出丢失。开发者可 `GroundReceiver(config, output_enabled=False)` 获得同样的快照模式。

每次启动独立 SQLite 文件，记录范围可以包含多个飞行任务。数据库用 source_id/mission_id 区分数据；目前不自动按任务拆库、不自动清理旧文件。长时间值守请安排归档，避免磁盘耗尽。

## 环境变量与配置保密

环境变量优先级高于 JSON：`DJI_H1_MQTT_PASSWORD` 覆盖密码，`DJI_H1_BAIDU_AK` 覆盖地图 AK。不要把环境变量定义写进可提交的启动脚本。

`config/*.json` 默认忽略，只跟踪 `*.example.json`。配置对象 repr 隐藏密码和 AK，地图错误不回显远程 URL。不要主动 `print` 整份原始 JSON。`.gitignore` 无法撤回已经提交的秘密，提交前仍应查看待提交文件。
