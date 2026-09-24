# 解码数据结构说明（中文，格式 v01）

本文面向使用 `dji_h1_ground` Python API、HTTP API 或 JSONL 导出的开发者，说明数据的层级、字段、单位、有效性和使用边界。配套调用方法见 [API 使用说明](API使用说明_zh.md)。

本文对应当前 `ground_app` 独立副本：Python 包版本 0.1.0，测量记录 DHR1 v01，SD 文件头 DHF1 v01，完整传输消息 DTM1 v01，旧版传输分片 DTF2 v02，GPS 批次 DGB1 v01。不同层的版本号独立，不能用包版本代替协议版本。本说明不修改既有 mission viewer 或固件。

## 1. 先区分三层数据

| 层级 | Python 对象 | 用途 |
|---|---|---|
| 传输层 | `ReassembledTelemetry`；旧 DTF2 另用 `TelemetryFragment` | DTM1 直接校验完整 publication；DTF2 才经过分片重组，最后返回相同逻辑消息对象 |
| 记录层 | `DecodedMessage.records` 中的 `GpsRecord`、`RawSpectrum`、`ReflectanceSpectrum`、`OperationEvent` | 保留设备产生的记录、时间戳和质量标志 |
| 查询与展示层 | `MissionService` 字典、`InterpolatedPosition`、`MissionMapModel` | 添加分页索引、百分比换算、位置插值和地图图层 |

M100M 使用 DTM1，一个 MQTT publication 恰好是一条完整逻辑消息，但一个 GPS 批次会展开为多条 GPS 记录。仅旧 DTF2 路径可能出现 publication 含半个或多个分片。应用通常应消费 `GroundReceiver.get_message()` 的结果，而不是直接对 MQTT payload 做 JSON 解码。

DTM1 开销 40 字节，总长不超过 4100 字节；含版本、类型、设备/任务身份、序号、内层长度、内层 CRC32、payload 和外层 CRC32。没有线上分片字段。校验通过后返回 `ReassembledTelemetry`，名字是兼容已有 API，并不表示新路径发生过重组。711 点反射率总长 2273 字节，十条 GPS 合包总长 784 字节。SD DHR1 记录和 DTA1 确认格式不变；DTA1 的 message_crc32 指内层 payload CRC，不是最外层 CRC。

SD 文件和实时遥测共享记录解释，但可用产品不同：接收器支持原始光谱类型，不代表生产固件正在通过 MQTT 发送原始光谱。也不能因实时视图缺少某种产品，就认为 SD 上未记录该产品。

## 2. 完整接收消息：DecodedMessage

这是接收 API 返回的不可变 dataclass。Python 整数没有 uint32/uint64 的固定宽度；下文这些类型表示线上字段范围。

| 字段 | Python 类型 | 含义 |
|---|---|---|
| `source_id` | `int`，uint64 范围 | 传输协议中的设备身份；不是光谱仪角色或 A 板文本序列号 |
| `mission_id` | `int`，uint64 范围 | 传输任务身份；不等同于记录内 `session_id` |
| `message_type` | `int` | 1 GPS、2 原始光谱、3 反射率、4 事件、5 GPS 批次 |
| `message_sequence` | `int`，uint32 范围 | 逻辑消息序号；批次为第一条 GPS 记录的序号 |
| `fragment_count` | `int` | DTM1 固定为 1，仅为 API 兼容值；DTF2 为实际分片数。均不是记录数或重传次数 |
| `received_utc_ns` | `int` | 地面收到补全该消息的 publication 时的 UTC 纳秒 |
| `received_monotonic` | `float` | 同一接收时刻的地面单调时钟秒；不与设备单调时钟直接相减 |
| `topic` | `str` | 最后补全 publication 的 MQTT 主题 |
| `qos` | `int` | 该 publication 实际交付的 QoS，不代表另一方向的发布配置 |
| `records` | `tuple` | 类型 1～4 各包含一条记录；类型 5 包含 1～10 条 `GpsRecord` |
| `payload` | `bytes` | 完整逻辑消息体：DHR1 记录或 DGB1 批次，不含 DTM1 或 DTF2 外壳 |

直接调用低层 `decode_message()` 时，若没有传入接收元数据，接收时间默认为 0、主题为空；它们不是测量时间。接收时刻减测量时刻得到的差还包含时钟误差、缓存及网络延迟，不能不加条件地称为纯网络时延。

需要跨任务保存数据时，至少保留设备、任务、消息类型和序号；GPS 批内还需保留各记录的 `header.sequence`。序号不是全球唯一 ID，不能仅以一个 sequence 作为数据库主键。

### 2.1 to_dict() / JSONL 表示

`message.to_dict()` 的外层键固定为：

```text
schema_version, source_id_hex, mission_id_hex,
message_type, message_sequence, fragment_count,
received_utc_ns, received_monotonic, topic, qos,
records, payload_base64
```

- `schema_version` 为整数 1，是地面 JSON 外壳版本。
- `source_id_hex`、`mission_id_hex` 为大写、定长 16 位十六进制字符串，不带 `0x`；用 `int(value, 16)` 还原。
- `received_utc_ns` 为十进制字符串，避免 JavaScript Number 丢失纳秒精度。
- `payload_base64` 为原始逻辑 payload 的 Base64 字符串。
- `records` 是数组；dataclass 变为对象，tuple 和 bytes 变为数组。因此 `sample_flags` 在 Python 为 bytes，在 JSON 中是整数数组。
- 记录内部的时间戳仍输出 JSON 数字，并未全部改为字符串。JavaScript 对超过 `2**53 - 1` 的整数不能精确表示；跨语言存储时需保留这一边界。

JSON 对象没有额外的 `crc_ok` 或 `scientifically_valid` 字段。正常返回表示通过相应格式检查；不表示定位、曝光、时间或所有光谱样本都有效。

## 3. 公共记录头：RecordHeader

四种记录都带 `header`。魔数、版本和固定头长度由解码器检查，不重复出现在此 dataclass 中。

| 字段 | 线上类型 | 含义 / 单位 |
|---|---|---|
| `record_type` | uint16 | 1 GPS、2 原始光谱、3 反射率、4 事件；不存在 DHR1 类型 5 |
| `record_size` | uint32 | 完整 DHR1 记录字节数，含 60 字节头和 4 字节尾部 CRC |
| `session_id` | uint32 | 设备记录会话号 |
| `segment_id` | uint16 | 会话内测量段 / 测线编号 |
| `flags` | uint16 | 通用记录标志，当前定义为 0；与下列时间和质量位不是同一字段 |
| `sequence` | uint32 | 该记录的序号；不是数组索引，也不是 A-B UART 的 8 位 SEQ |
| `b_monotonic_us` | uint64 | B 板事件时间，单调微秒 |
| `a_monotonic_ms` | uint64 | 由同步模型关联到 A 板时域的单调毫秒 |
| `utc_ms` | uint64 | 同步 UTC Unix 毫秒，即自 1970-01-01T00:00:00Z 起的毫秒 |
| `sync_age_ms` | uint32 | 同步观测的年龄，毫秒；不是时间精度或网络延迟 |
| `sync_generation` | uint16 | 同步模型代次，用于区分重建后的时钟关联 |
| `sync_state` | uint8 | 同步状态，见下表 |
| `time_valid_flags` | uint8 | 哪些时间域可用，见下表 |

| `sync_state` 值 | 名称 | 解释 |
|---|---|---|
| 0 | UNSYNCED | 尚未同步 |
| 1 | ACQUIRING | 正在建立同步 |
| 2 | LOCKED | 已锁定同步 |
| 3 | HOLDOVER | 暂无新观测，保持已有模型 |
| 4 | INVALID | 同步无效 |

`time_valid_flags` 按位组合：`0x01` B 单调时间有效，`0x02` A 单调时间有效，`0x04` UTC 有效。例如 7 表示三种时间均有效。不能仅凭 `utc_ms != 0` 或状态名称判断时间可用；应检查对应有效位，并根据业务决定是否接受 HOLDOVER 和较大的同步年龄。

UTC 数字本身不含时区。显示北京时间时转换为 UTC+08:00；不要把存储值加 8 小时后仍称为 UTC。反射率使用地面光谱的时间戳，不使用计算完成或 MQTT 接收时刻。GPS 头中的 B 时间对应 UART 实时帧接收完成，GPS 正文则保留 A 侧采样时刻，两者语义不同。

## 4. 原始光谱：RawSpectrum

结构为 `header`、`info: RawRecordInfo`、`samples: tuple[int, ...]`。`samples` 保存 H1 返回的 uint16 原始值，没有由解码器自动做曝光归一化。

| `info` 字段 | 线上类型 | 含义 |
|---|---|---|
| `frame_count` | uint32 | 该采集流的帧计数 |
| `exposure_us` | uint32 | 曝光时间，微秒 |
| `sample_count` | uint16 | 本帧样本数；等于 `len(samples)` |
| `spectrum_scale` | int16 | 十进制缩放指数；H1 数值换算为 `raw / 10**spectrum_scale` |
| `spectrometer_role` | uint8 | 0 地面 ground / SC16-A，1 天空 sky / SC16-B |
| `exposure_status` | uint8 | 0 正常、1 过曝、2 欠曝 |
| `frame_quality` | uint8 | 帧质量位，可组合 |

`frame_quality`：`0x01` VALID，`0x02` SATURATED，`0x04` UNDEREXPOSED，`0x08` AFTER_RECOVERY，`0x10` COUNT_MISMATCH。保留原值供追溯，不要把多位标志当单一枚举比较。

已由项目方确认：本项目 H1 的每条完整光谱含 **711 个波长样本**，覆盖 **340～1050 nm，含首尾，间隔 1 nm**。零基下标 i 对应 `340 + i` nm：下标 0 → 340 nm，下标 355 → 695 nm，下标 710 → 1050 nm。这里是“一条光谱内的 711 个样本”，不是 711 条独立记录。地面、天空原始光谱以及反射率共用该波长网格。

格式 v01 本身未携带波长表，该映射属于已确认的设备约定。`RawSpectrum.wavelengths_nm` 和 `ReflectanceSpectrum.wavelengths_nm` 属性在 sample_count=711 时返回 711 项整数 tuple，否则返回 None。公共函数 `spectrum_wavelengths_nm(sample_count)` 提供同样映射。其他长度可能是合成数据或异常/不同设备数据，不猜测其波长，不截断、补点或拉伸为 711 点。

这些属性是派生信息，不改变 DHR1/MQTT 格式，也不会由 dataclass 的 `asdict()` 自动加入 `DecodedMessage.to_dict()`；需要 JSON 波长数组时使用查询 API 的 `wavelengths_nm`，或显式调用映射函数。缩放后的 H1 数值不能在没有辐射标定的情况下自行标成绝对辐亮度；波长映射确认不等于完成辐射标定。

## 5. 反射率：ReflectanceSpectrum

结构为 `header`、`info: ReflectanceRecordInfo`、`reflectance_0p01_percent: tuple[int, ...]`、`sample_flags: bytes`。

| `info` 字段 | 线上类型 | 含义 |
|---|---|---|
| `calculation_count` | uint32 | 计算结果计数 |
| `ground_frame_count` | uint32 | 使用的地面原始帧计数 |
| `sky_frame_count` | uint32 | 使用的天空原始帧计数 |
| `sky_b_monotonic_us` | uint64 | 天空帧 B 时域时间，微秒 |
| `sky_age_us` | uint32 | 地面帧相对天空帧的时间差，微秒 |
| `sample_count` | uint16 | 样本数；值数组与质量数组长度均应与之相等 |
| `valid_sample_count` | uint16 | 有 VALID 标志的样本数 |
| `clamped_low_count` | uint16 | 被下限截断的样本数 |
| `clamped_high_count` | uint16 | 被上限截断的样本数 |
| `invalid_denominator_count` | uint16 | 天空分母低于门限的样本数 |
| `input_quality_flags` | uint16 | 输入整体质量位 |

数值单位为 **0.01%**：0 → 0.00%，1234 → 12.34%，10000 → 100.00%。百分比 = 整数 / 100；无量纲比例 = 整数 / 10000。

当前固件按地面帧驱动计算，选择时间不晚于该地面帧且满足新鲜度要求的天空帧，进行曝光与缩放补偿：

```text
percent = 100 × ground_raw / sky_raw
              × sky_exposure_us / ground_exposure_us
              × 10**(sky_scale - ground_scale)
```

结果限制在 0～100%，以 0.01% 整数记录。这仍是尚待辐射 / 几何标定的表观反射率，不能因为字段叫 reflectance 就认为已经完成科学标定。解码器负责读取，不重算这个公式。

`input_quality_flags`：`0x01` GROUND_VALID，`0x02` SKY_VALID，`0x04` SKY_FRESH，`0x08` SAMPLE_COUNTS_MATCH。

| 每个 `sample_flags[i]` 的位 | 名称 | 使用注意 |
|---|---|---|
| `0x01` | VALID | 样本具有计算有效标志 |
| `0x02` | CLAMPED_LOW | 已截断到下限 |
| `0x04` | CLAMPED_HIGH | 已截断到上限；100% 可能是截断结果 |
| `0x08` | SKY_TOO_SMALL | 分母过小；该位置的 0 不能解释为真实零反射率 |
| `0x10` | GROUND_OVER | 地面输入过曝 |
| `0x20` | SKY_OVER | 天空输入过曝 |
| `0x40` | INTERPOLATED | 预留的光谱样本插值标志，当前计算路径不设置；与地理位置插值无关 |

VALID 可与截断、过曝位同时存在；统计项也不全是互斥分类。应用可进一步排除过曝或截断样本，但不要静默删除原始质量位。

## 6. GPS：GpsRecord / GpsSample

结构为 `header` 和 `sample: GpsSample`。位置、UTC、电量的有效性分别判断。

| `sample` 字段 | 线上类型 | 含义 / 换算 |
|---|---|---|
| `protocol_sequence` | uint8 | A-B 实时帧的原始 SEQ，会循环；不是 DTF2 序号 |
| `latitude_e7` | int32 | 纬度 × 1e7，北纬为正；除以 1e7 得到度 |
| `longitude_e7` | int32 | 经度 × 1e7，东经为正；除以 1e7 得到度 |
| `altitude_relative_mm` | int32 | 相对起飞点高度，毫米；不是海拔 |
| `utc_seconds` | uint32 | A 发来的 UTC Unix 秒 |
| `a_monotonic_ms` | uint32 | A 原始采样单调毫秒，可能回绕；不是头中的 uint64 关联时间 |
| `utc_milliseconds` | uint16 | A 发来的毫秒部分，协议范围 0～999 |
| `source_flags` | uint8 | 经纬度和高度各自的数据来源 |
| `gps_fix` | uint8 | GPS 定位状态 |
| `rtk_solution` | uint8 | RTK 解算状态，取值不连续 |
| `flight_status` | uint8 | 0 电机停止、1 地面电机转动、2 空中 |
| `display_mode` | uint8 | 飞行模式原始码 |
| `battery_percent` | uint8 | 电量百分比，协议范围 0～100；须检查有效位 |
| `a_status` | uint8 | A 板状态位 |
| `valid_flags` | uint8 | 正文各类数据有效位；与 `header.time_valid_flags` 不同 |

A 原始 UTC 毫秒为 `sample.utc_seconds * 1000 + sample.utc_milliseconds`。它是 A 上报的时间观测，而 `header.utc_ms` 是 B 时钟模型对记录时刻的估计，两者不保证逐毫秒相同。

### 6.1 来源与有效位

- `source_flags & 0x01`：经纬度来自 RTK；未置位表示 GPS。
- `source_flags & 0x02`：相对高度来自 RTK；未置位表示 GPS。
- 经纬度与高度可来自不同来源，不能用一个 RTK 标志统管二者。
- `valid_flags`：`0x01` 经纬度、`0x02` 相对高度、`0x04` UTC、`0x08` 电量有效。对应位未置位时，保留值仅供审计，不能参与正常业务计算。

### 6.2 A-B 协议状态码

| 字段 | 当前协议含义 |
|---|---|
| `gps_fix` | 0 无定位；1 仅航位推算；2 二维；3 三维；4 GPS + 航位推算；5 仅授时 |
| `rtk_solution` | 0 不可用；16 单点；17/18 差分；32/33/34 浮点；48 L1 整数；49 宽巷整数；50 固定解；1/2/8/19/20 为其他状态 |
| `display_mode` | 常用：11 自动起飞；12 自动降落；15 返航；33 强制降落；其他值保留 |
| `a_status` | `0x01` 飞行器链路正常；`0x02` 控件注册成功；`0x04` 数据订阅正常；`0x08` 起飞基准已锁存；`0x10` RTK 起飞高度基准有效 |

协议仅将 RTK 状态 50 视为固定解。地面仍应保留原始状态码、来源位与有效位，不能仅凭 `gps_fix` 或 `rtk_solution` 一项覆盖 A 的其他标志。

### 6.3 GPS 批次不会变成一个“平均位置”

类型 5 的 DGB1 为传输压缩容器，`decode_message()` 将其还原为 1～10 条普通 `GpsRecord`，每条保留自身序号、时间和位置。批内记录的 `header.record_type` 仍为 1。不要把一个批次只作为一个轨迹点，也不要把批次接收时刻赋给全部 GPS 样本。

## 7. 操作事件：OperationEvent

结构为 `header` 和 `info: OperationEventInfo`。

| `info` 字段 | 线上类型 | 含义 |
|---|---|---|
| `event_code` | uint16 | 非零事件码；未识别的非零码可保留 |
| `severity` | uint8 | 0 info、1 warning、2 error、3 critical |
| `reserved` | uint8 | 当前必须为 0 |
| `argument0` | uint32 | 事件相关无符号参数 |
| `argument1` | int32 | 事件相关有符号参数，可存负错误码 |

| 事件码 | 查询层名称 | 含义 |
|---|---|---|
| 1 | `handshake` | 握手 |
| 2 | `segment_start` | 测量段开始 |
| 3 | `stop_request` | 收到停止请求 |
| 4 | `segment_end` | 测量段结束 |
| 5 | `power_off_request` | 收到断电预告 |
| 6 | `protocol_crc_error` | 协议 CRC 错误 |
| 7 | `protocol_timeout` | 协议超时 |
| 8 | `clock_observation_drop` | 时钟观测丢弃 |
| 9 | `reflectance_rejected` | 反射率结果被拒绝 |
| 10 | `capture_result` | 采集结果 |
| 11 | `flight_closed` | 飞行记录关闭 |
| 12 | `drone_identity_mismatch` | 飞行器身份不一致 |
| 13 | `ab_link_lost` | A-B 链路丢失 |
| 14 | `ab_link_restored` | A-B 链路恢复 |

参数含义依赖事件码，不能统一解释成经纬度、次数或停止原因。低层记录保留数值；实时查询层将其投影为扁平字典，包含 `event`、字符串 `severity`、`sequence`、会话/测量段、时间和两个参数。未知事件名称形如 `event_123`。

SD 的 `EVENTS.JSONL` 是可读事件记录，可能含比紧凑遥测事件更多的键。不要要求实时事件和 SD 事件键集合完全一致，也不要把“收到 stop_request”误认为“文件已经关闭”。

## 8. 光谱位置：InterpolatedPosition

位置插值是地面派生结果，不是光谱原始记录自带的 GPS 字段。按相邻有效 GPS 点间恒速假设插值，不外推轨迹范围。经度处理了跨 ±180° 的短路径。

| 字段 | 类型 | 含义 |
|---|---|---|
| `latitude_deg` / `longitude_deg` | float | 插值得到的纬度 / 经度，度 |
| `altitude_relative_m` | float 或 None | 相对高度，米；任一端高度无效则为 None |
| `time_domain` | str | 实际使用 `a_monotonic_ms` 或 `b_monotonic_us` |
| `target_time` | int | 被定位记录的目标时间，单位由 time_domain 决定 |
| `before_time` / `after_time` | int | 两端 GPS 时间，与 target_time 同域同单位 |
| `before_gps_index` / `after_gps_index` | int | 两端记录在该任务 GPS 集合内的索引 |
| `interpolation_fraction` | float | `(target-before)/(after-before)`；精确命中为 0 |
| `gap_ms` | float | 两端 GPS 的时间间隔，始终为毫秒 |
| `quality` | str | `exact` 精确命中、`interpolated` 普通插值、`wide_gap` 大间隔插值 |
| `before_protocol_sequence` / `after_protocol_sequence` | int | 两端 A-B SEQ |
| `before_gps_fix` / `after_gps_fix` | int | 两端 GPS 定位状态 |
| `before_rtk_solution` / `after_rtk_solution` | int | 两端 RTK 状态 |
| `sync_generation` | int 或 None | A 时域使用的同步代次；B 时域为 None |

默认 `time_domain="auto"` 优先 A 时域，并在 A 无有效结果时尝试 B 时域，包括 A 轨迹不能包围目标的情况。A 时域按同步代次分组，并将正文 32 位 A 时间关联到头的 64 位时间附近。明确指定 A 时域则不回退 B。

默认 `wide_gap_ms=500` 仅使质量变成 `wide_gap`，不是拒绝门限；`max_gap_ms=None` 不限制包围区间大小。需要禁止跨长时间掉线插值时，调用方必须传入例如 `max_gap_ms=500.0`。拒绝、缺少有效定位或尚未收到后一个点时，结果为 None，JSON 为 null，而不是坐标 (0, 0)。

实时光谱先到、后一个 GPS 点后到时，位置可能先为空，再在后续快照中补全。`quality="exact"` 只说明时间精确命中某条 GPS，不是定位精度保证。比较定位结果时应一起检查 `time_domain`、`sync_generation`、`gap_ms` 和原始质量码。

目前地面应用不把这些经纬度自动转换为百度 BD-09。地图底图坐标适配属于展示问题；百度 API 失败不应影响上述接收、解码和插值结果。

## 9. 查询 / HTTP 输出与地图模型

`MissionService` 不是 `DecodedMessage.to_dict()` 的别名，两者结构不同：

| 查询方法 | 主要返回字段 |
|---|---|
| `raw_index()` / `reflectance_index()` | `total, offset, items`；每个 item 为 `index, header, info`，不含大样本数组 |
| `raw_spectrum(index)` | `index, header, info, samples, wavelengths_nm, position` |
| `reflectance_spectrum(index)` | `index, header, info, reflectance_0p01_percent, reflectance_percent, sample_flags, wavelengths_nm, position` |
| `gps()` | `total, offset, items`；每项为 `header, sample` |
| `events()` | `total, offset, items, read_errors`；items 为事件字典 |
| `position_at_b_monotonic_us()` | 位置字典或 None；HTTP `/api/v1/position` 另外包一层 `position` |
| `map_data()` | `route, measurements, events, unlocated_measurements, unlocated_events` |

`index` 是当前产品集合的访问下标，与设备 sequence 不等价。持久化业务关联时保留身份与记录头，不要只保存 GUI 下标。`reflectance_percent` 是查询层新增的浮点百分比数组，低层 `ReflectanceSpectrum` 没有这个属性。

地图模型无需百度 SDK 即可构建：

- `RoutePoint`：`latitude_deg, longitude_deg, altitude_relative_m, b_monotonic_us, gps_index`，包括无光谱的有效航迹点。
- `MeasurementPoint`：`latitude_deg, longitude_deg, altitude_relative_m, reflectance_index, session_id, segment_id, calculation_count, utc_ms, quality, gps_gap_ms, time_domain, sync_generation`。
- `MissionEventPoint`：`latitude_deg, longitude_deg, event_index, event_name, severity, b_monotonic_us, utc_ms`。
- `MissionMapModel` 的三类点集合在 Python 中为 tuple，在 JSON 中为数组。两个 `unlocated_*` 计数记录未能定位的测量 / 事件，未显示在地图上不等于原数据丢失。

## 10. 文件索引与故障信息

离线读取使用轻量 `RecordRef`，其字段为 `header, file_offset, body_offset, body_size, info, expected_crc`。字节偏移从文件起点算，样本按需读取，避免索引时一次加载全部光谱。实时兼容视图中的偏移 / CRC 占位值不是可用磁盘地址。

`FileHeader` 提供 `record_type` 与 `format_version`。记录扫描故障 `RecordScanIssue` 包含 `file_offset`、`message`、`recovered_records`、`discarded_tail_bytes`，用于说明从何处停止扫描及已恢复的记录前缀；不表示损坏尾部已被修复。

格式错误由 `RecordFormatError` 表示。标准解码检查魔数、版本、长度及 CRC；离线 API 若显式关闭 `verify_crc` 则不能再声称验证过 CRC。CRC 也不是签名、身份认证或科学质量校验。

### 10.1 二进制长度速查

所有字段显式按小端序列化。不要用 Python 对象大小或 C `sizeof` 推算线长。

| 内容 | 字节数 |
|---|---|
| DHF1 文件头 | 16，每个二进制产品文件一次 |
| DHR1 通用头 | 60，其中时间字段合计 32 |
| DHR1 尾部 CRC | 4，每条记录一次 |
| GPS 完整 DHR1 记录 | 98 |
| 原始光谱完整 DHR1 记录 | `80 + 2*N` |
| 反射率完整 DHR1 记录 | `100 + 3*N` |
| 紧凑事件完整 DHR1 记录 | 76 |

N 为本条 `sample_count`，本项目完整实测光谱预期为 711；格式仍允许其他数量以保留异常帧或测试数据。N=711 时原始光谱记录为 1502 字节，反射率记录为 2233 字节。以上不含 DTM1 或 DTF2 开销，也不适用于直接计算 DGB1 压缩批次大小。`magic` 只是帮助识别格式的固定签名，不是设备序号或加密密钥。

## 11. 最小数据消费示例

以下函数可在应用工作线程中处理 `get_message()` 返回值，不依赖 Qt 或地图：

```python
from datetime import datetime, timedelta, timezone
from dji_h1_ground import GpsRecord, ReflectanceSpectrum, OperationEvent


def consume(message):
    for record in message.records:
        header = record.header
        local_time = None
        if header.time_valid_flags & 0x04:
            utc = datetime(1970, 1, 1, tzinfo=timezone.utc) + timedelta(
                milliseconds=header.utc_ms)
            local_time = utc.astimezone(timezone(timedelta(hours=8)))

        if isinstance(record, GpsRecord):
            gps = record.sample
            position = None
            if gps.valid_flags & 0x01:
                position = (gps.latitude_e7 / 1e7, gps.longitude_e7 / 1e7)
            print("GPS", header.sequence, local_time, position)
        elif isinstance(record, ReflectanceSpectrum):
            # 缺失值用 None，不把无效分母产生的零当作真实零反射率。
            percent = [v / 100.0 if flag & 0x01 else None
                       for v, flag in zip(record.reflectance_0p01_percent,
                                          record.sample_flags)]
            print("反射率", header.sequence, local_time, percent)
        elif isinstance(record, OperationEvent):
            print("事件", record.info.event_code, record.info.severity,
                  record.info.argument0, record.info.argument1)
```

上例仅依据 VALID 位筛选，实际分析可进一步排除过曝和截断样本。`records` 必须逐条遍历，否则批次 GPS 会漏点。若需要光谱空间位置，使用第 8～9 节的查询接口，不要用“最近收到的 GPS”替代时间插值。

## 12. 维护依据

字段变更时同步核对以下文件，并更新测试和本文，不要在应用层再维护一份二进制字段偏移表：

- [decoder.py](../src/dji_h1_ground/decoder.py)：DHR1 字段及 CRC 解码。
- [messages.py](../src/dji_h1_ground/messages.py)：完整消息与 JSON 表示。
- [telemetry.py](../src/dji_h1_ground/telemetry.py)：DTM1 校验、旧 DTF2 重组及 DGB1 批次恢复。
- [geolocation.py](../src/dji_h1_ground/geolocation.py)：时间域与位置插值。
- [api.py](../src/dji_h1_ground/api.py)：查询 / HTTP 字典。
- [live.py](../src/dji_h1_ground/live.py)：实时事件名称投影。
- [presentation.py](../src/dji_h1_ground/presentation.py)：地图数据模型。

本说明还对照了主项目 `measurement_records.h`、`clock_sync.h`、`h1.h`、`calculation.c` 和《AB板串口通信协议_V1.0.md》。它们是本次交叉核对依据，而不是独立安装 ground_app 的运行依赖。
