# Python 与 HTTP API 使用说明

公共导入入口为 `dji_h1_ground`。本 API 版本为 0.1.0，记录格式 DHR1 v01、分片格式 DTF2 v02、GPS 批次 DGB1 v01、确认 DTA1 v01。这些版本独立于 Python 包版本。

字段类型、单位、质量位、GPS 批次展开以及 Python/JSON 差异见 [解码数据结构说明](解码数据结构说明_zh.md)。

## 1. 完整接收器（推荐）

```python
from dji_h1_ground import GroundReceiver

receiver = GroundReceiver.from_config("config/ground_app.local.json")
receiver.start()  # 异步连接；返回不代表已经 SUBACK
try:
    while True:
        item = receiver.get_message(timeout=1.0)
        if item is None:
            print(receiver.status()["state"])
            continue
        print(item.source_id, item.mission_id, item.message_sequence)
        for record in item.records:
            print(record.header.utc_ms)
        output = item.to_dict()  # 可交给 json.dumps / 自己的 HTTP 服务
finally:
    receiver.stop()
```

`state == "subscribed"` 表示已获订阅确认；`connected == true` 可能仍处于 subscribing。起飞前等待 subscribed，再确认已收到本次设备的数据。断线后 Paho 自动尝试重连并重新订阅。

`get_message(timeout)` 返回一个不可变 `DecodedMessage`，超时返回 None。队列是单消费者语义：多个线程竞争同一个队列会分走不同消息，不是广播订阅。需要多个消费方时，在自己的工作线程消费后分发；不要调用接收器的内部 MQTT 回调来执行业务处理。

`iter_messages()` 是便利生成器；当 receiver.stop 完成且输出队列排空时结束。它适合运行在调用方工作线程，不能直接放到 Qt 主线程里无限循环。

`start()` 已运行时无操作；`stop()` 可重复调用。停止先拒绝新入口，再排空解码和 ACK 工作，最后关闭 MQTT 和数据库。若工作线程超过停止等待时限，stop 抛 RuntimeError 并保留资源，调用方可稍后再次 stop。每次新会话构造新对象，停止后不能复用旧对象启动。

## 2. 输出消息与单位

| 属性 | 含义 |
|---|---|
| source_id / mission_id | Python uint64 范围整数，用于区分设备与任务 |
| message_type | 1 GPS、2 原始光谱、3 反射率、4 操作事件、5 GPS 批次 |
| message_sequence | DTF2 逻辑消息序号；GPS 批次对应批内第一条序号 |
| fragment_count | 本逻辑消息的分片总数，不是 MQTT publication 数 |
| received_utc_ns | 最后一个补全分片所在 publication 到达地面时的 UTC 纳秒 |
| received_monotonic | 同一 publication 的地面单调时钟秒，仅在同一进程时钟域比较 |
| topic / qos | 最后补全 publication 的主题及实际交付 QoS |
| records | 解码后的记录 tuple；GPS 批次展开后有 1～10 条，其余通常 1 条 |
| payload | 完整原始逻辑 payload bytes，供审计或另一套解码器使用 |

`to_dict()` 输出 schema_version=1。uint64 身份输出固定 16 位十六进制字符串，接收纳秒时间输出十进制字符串，避免 JavaScript Number 精度损失。payload 用 base64；样本 flags bytes 转为整数数组。

记录对象带 `header`，其字段包括 UTC 毫秒、B 单调微秒、同步 A 单调毫秒、同步代次、有效位、会话与测线编号。UTC 为 0 或有效位不满足时，不应解释成可靠绝对时间。

反射率记录为 `ReflectanceSpectrum`：`reflectance_0p01_percent` 使用 0.01% 单位，除以 100 得到百分比；`sample_flags` 保存每个样本质量标记。原始记录为 `RawSpectrum`，samples 是原始数值，曝光时间见 `info.exposure_us`。GPS 为 `GpsRecord.sample`（纬经度 1e-7 度、相对高度 mm 等原协议单位）。操作事件为 `OperationEvent.info`（事件码、严重级、参数）。精确字段名以 dataclass 和类型提示为准。

未知消息类型不伪造成功解码结果：原始 MQTT payload 被保存，错误计数增加，对完整且无法接受的消息按现有协议发送永久拒绝 DTA1。未来增加消息类型时应更新 decoder 和协议测试，再用旧日志回放。

原始光谱和反射率对象的 `wavelengths_nm` 属性提供已确认的 H1 波长网格：711 个样本对应 340～1050 nm（含首尾），间隔 1 nm。其他样本数返回 None，不推测波长。查询 / HTTP 的单条光谱结果另含 `wavelengths_nm` 数组或 null；低层 `to_dict()` 不增加此派生属性，保持记录 JSON 不变。外部应用也可调用 `from dji_h1_ground import spectrum_wavelengths_nm` 获取网格。

## 3. 不经过 MQTT，自行提供 payload

已有自己的 MQTT 客户端时，可以只调用协议层：

```python
from dji_h1_ground import (
    TelemetryFragmentStreamDecoder, TelemetryReassembler, decode_message,
)

stream = TelemetryFragmentStreamDecoder()
assembler = TelemetryReassembler(timeout_seconds=30, max_inflight=512)

def consume_payload(payload: bytes):
    output = []
    for fragment in stream.feed(payload):
        complete = assembler.push(fragment)
        if complete is not None:
            output.append(decode_message(complete))
    return output
```

每条独立传输字节流分别建立 stream/assembler 实例；这些低层对象本身不是线程安全的，应由一个处理线程独占。DTU 可能把一个 DTF2 分片切成多个 MQTT payload，也可能把多个分片拼在一起，所以不能假设一次 MQTT 回调等于一个 DTF2 分片。

`stream.feed` 处理魔数重同步及分片 CRC；`assembler.push` 处理乱序、重复、元信息一致性、完整消息 CRC。push 返回 None 表示未完成或重复。空闲时可调用 assembler.expire 清理未完成记录。调用方传入 now 时需始终使用同一个单调时钟域。

只有需要自己实现 ACK 时才调用 `encode_acknowledgement(complete)`。低层解码成功不代表已持久化或被业务系统接受；不能在原始 payload 到达时提前 ACK。使用 GroundReceiver 时这些步骤已完成。

## 4. 快照、离线任务与插值

```python
from dji_h1_ground import GroundConfig, GroundReceiver, MissionService

receiver = GroundReceiver(GroundConfig.load("config/ground_app.local.json"),
                          output_enabled=False)
receiver.start()
try:
    snapshot = receiver.snapshot()  # 无数据时 None
    if snapshot and snapshot.reflectance:
        print(receiver.service.reflectance_spectrum(0))
finally:
    receiver.stop()

offline = MissionService()
offline.load(r"D:\F_0003")
print(offline.overview())
print(offline.reflectance_spectrum(0, time_domain="auto", max_gap_ms=1000))
```

HTTP 与 Python query facade 都从处理线程的数据状态查询，不依赖 GUI 的一秒刷新。外部程序应把返回的任务快照视为只读。

位置插值沿用已验证实现：优先同步 A 时间，缺失时按 auto 规则使用 B 时间；寻找记录前后的有效 GPS 点进行线性插值，不外推。结果带 time_domain、sync_generation、前后 GPS 索引、插值比例和 gap_ms。用 max_gap_ms 拒绝跨度过大的 GPS 缺口。不要把地面接收时间当成测量时间插值。

实时数据到达可能晚于光谱：此时光谱本身可用，position 为 None；下一条 GPS 到达后快照可得到位置。原始 UTC 始终保持 UTC，时区偏移只用于显示。

## 5. 本机只读 HTTP

GUI 默认启动 HTTP；headless 模式也能启动。自建应用可：

```python
from dji_h1_ground import start_http_api

server, thread = start_http_api(receiver.service, port=8765)
# 应用退出时：
server.shutdown()
server.server_close()
thread.join()
```

| GET 路径（前缀 /api/v1） | 内容 |
|---|---|
| /health | 模式、任务是否加载、连接状态及处理/日志/队列计数 |
| /mission | 当前任务概况及各类记录数量 |
| /gps?offset=0&limit=500 | GPS 页 |
| /events?offset=0&limit=500 | 操作事件页 |
| /reflectance-index?offset=0&limit=100 | 反射率索引 |
| /reflectance?index=0&max_gap_ms=1000 | 指定光谱及插值位置，含百分比数组 |
| /raw-index?role=ground&offset=0&limit=100 | 原始光谱索引 |
| /raw?index=0 | 原始光谱 |
| /position?b_monotonic_us=123456&max_gap_ms=1000 | 对外部 B 时钟样本插值 |
| /map | 离线地图模型（坐标数据），不调用百度服务 |

分页 limit 最多 5000。索引是当前快照里的位置，不是永久记录 ID；保存引用时应保存设备/任务/记录类型/序号。没有任务时先轮询 health，不能假设 /mission 总是已有数据。

HTTP 当前使用查询请求，不提供远程控制、串口指令、任意文件写入、WebSocket 或 SSE。接收完整消息推送应使用 Python 消费队列。

## 6. 日志回放

```python
from dji_h1_ground import GroundConfig, GroundReceiver, export_jsonl

replay = GroundReceiver(GroundConfig.load("config/ground_app.local.json"))
for message in replay.replay("data/GROUND_xxx.sqlite3", speed=0):
    process = message.to_dict()
print(replay.service.overview())
export_jsonl("data/GROUND_xxx.sqlite3", "new_export.jsonl")
```

replay 是生成器，必须迭代才会执行。它始终关闭网络、ACK 和新数据库写入，使用原接收单调时间重放分片过期逻辑。回放用于验证解码器，不模拟网络连接状态。不要对同一对象同时 start 和 replay。

SQLite 原始 publications 是接收证据；messages 是当时成功解码输出。它们不是 exactly-once 业务数据库：重试、ACK 缓存窗口结束或接受阶段故障可能造成重复证据。业务端可用 source_id + mission_id + message_type + sequence 作为幂等键，同时核对 payload 一致性。
