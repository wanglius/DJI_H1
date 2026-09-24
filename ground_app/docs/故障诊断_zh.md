# 故障诊断与验证

## 无消息或反复断开

1. 确认 MQTT listener 的 host/port，不要填 EMQX 管理网页 URL。
2. 查看 state，必须为 subscribed 才表示订阅已确认。
3. 确认上下行 topic 与 B 板一致。
4. 同时运行的 MQTTX、验证器、GUI 必须使用不同 client_id，避免相互踢下线。空 client_id 会自动生成唯一值。
5. 检查 source_id / mission_id 过滤是否排除了本次任务。
6. 用 `/api/v1/health` 查看当前接收计数，区分「没收到 MQTT」与「收到但无法重组/解码」。

不需要为本应用安装 ESP-IDF 或接入 A/B 串口；真实采集控制仍由 A 板或上级工程的模拟器负责。

M100M 分支使用 DTM1 和独立 topic（默认 `dji-h1/m100m/up`、`dji-h1/m100m/down`）；必须使用支持 DTM1 的 `ground_app`。旧 mission viewer 或只调用 DTF2 stream decoder 的自编应用会忽略新消息。自行提供 payload 时请使用 [API 文档第 3 节](API使用说明_zh.md#3-不经过-mqtt自行提供-payload) 的格式分流示例。

## 接收计数的含义

| 计数 | 含义/判断 |
|---|---|
| mqtt_messages / mqtt_bytes | 进入指定主题回调的 publication 和字节数 |
| processed_publications | 处理线程已处理的 publication，可能包含无效或部分数据 |
| fragments | 可恢复的旧 DTF2 完整分片数；纯 DTM1 任务为 0 是正常现象 |
| complete_messages | 应用接受完成次数，GPS 批次计一次 |
| gps_records / reflectance_records / event_records | 内存中新接受的相应记录数量 |
| decoded_messages | 完成解码输出次数 |
| filtered_messages | 设备/任务筛选排除的完整消息数；不发送任何 DTA1 |
| duplicate_fragments | 历史计数名：DTF2 重复分片或 DTM1 重复完整消息 |
| invalid_messages | 重组/完整消息校验或类型处理错误；并非每个坏字节都计一次 |
| expired_assemblies | 空闲清理时发现的过期重组数；不能据此计算精确丢包率 |
| ingress_dropped | 入口队列满；对应 payload 没进入处理/日志 |
| output_dropped | Python 消费队列满；已接受数据仍在快照及已启用的日志中 |
| journal_failures | 写盘失败；该次处理不发送成功 DTA1 |
| acknowledgements | 已交给 Paho 的 DTA1 数，不代表 B 板已经收到 |
| ack_publish_failures / ack_queue_overflows | ACK 出口异常或压力 |
| processing_max_ms | 处理一次 publication 的最大耗时，含 SQLite 提交 |

DTM1 不创建重组状态；纯 DTM1 任务的 inflight、buffered_bytes、expired_assemblies 正常为 0，不能据此认为未收到数据，应看 complete_messages 和各记录计数。每个 DTM1 publication 都先校验完整长度及 CRC，即使它是已确认消息的重传。

旧 DTF2 是连续字节流，魔数扫描会跳过噪声，因此 invalid_messages 不等于物理链路错误总数。原始 publications 可用于另行逐字节分析。

## 地图坏了，数据仍在

先看曲线、事件列表、接收计数是否继续变化，再看 HTTP。地图失败信息只表示底图/地图渲染失败。不要为了地图问题修改 QoS 或重启 B 板。

map.enabled=false 可以完全禁用 WebEngine。地图开关和 AK 有错误时回退离线图。无法加载的地图不会阻止“跟随最新光谱”显示尚无定位的反射率。

## 文件系统问题

启动无法创建日志目录会明确失败；不会默默以“已经记录”的状态运行。运行中磁盘满/权限变化会产生 journal_failures / processing_failures，成功 ACK 被暂缓。故障修复后的发送端重传允许重新处理，不被已完成重组缓存永久屏蔽。

SQLite 使用 WAL 和 FULL 同步，每次原始 publication / 完整解码消息提交。磁盘越慢，处理队列越可能积压；网络线程仍会及时返回。队列容量有上限，不能代替磁盘性能。当前没有自动日志删除，测试后关闭接收器再归档数据库。

进程或电脑突然断电时，已完成事务可恢复，但尚在入口队列的数据可能丢失；B 板可能重试，仍受其池条目寿命限制。不能把地面 ACK 当成跨所有故障的绝对保证。

## 内存与多任务边界

实时内存保留最多四个设备/任务组合；GUI/查询默认展示时间排名最新的任务。更早任务仍保留在接收日志中，消息队列会输出所有未过滤的已接受消息。

当前每个活动任务的光谱/GPS 列表随时间增长，适用于任务时段接收；连续多日值守应分会话归档和重启接收器。自动内存滑窗、自动磁盘配额和多机选择 GUI 是后续增强项。本版不承诺无限时长、无限任务保持在内存。

## 自动测试

```powershell
python -m unittest discover -s tests -v
```

包含复制来的协议/文件测试及新增集成测试：分片拆包粘包、乱序、重复、CRC、重组容量/过期、GPS 批次、SD 受损尾部、HTTP、日志导出/回放、写盘失败后重试、消费队列压力、Qt 导入隔离、地图初始化/超时/渲染故障以及地图失败时曲线继续跟随。

本版交付时的软件测试使用本机合成数据和假的 MQTT transport。它验证业务路径、线程边界与输出，不能替代真实 broker、DTU 和飞行平台联调。现场验证顺序建议：headless 接收 → GUI 离线地图接收 → 开启百度图 → 故意禁用地图网络，确认光谱/ACK/日志继续 → 正常关闭并回放数据库。

起飞前先确保地面接收器 subscribed。与现有验证器同时订阅时明确谁负责 DTA1：纯观察实例设 ack_enabled=false，避免把不同接收器的成功计数误当成单个接收器的完整覆盖率。
