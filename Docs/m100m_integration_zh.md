# M100M-B2 原生 AT 分支集成

本说明适用于 `codex/m100m-b2-integration`，替代交接文档中 **YY-M200 透明串口与 DTF2 分片**的部署步骤。`main` 的已验证基线不变；本分支尚不能视作已完成白天光谱/10 分钟资格测试。

## 硬件与边界

- MCU UART1：TX GPIO17 → 模组 RX；RX GPIO18 ← 模组 TX，共地，460800 8N1。
- 当前 PCB **没有模组 RST 接线**。不使用其他 GPIO 假定为 RST，也不修改板级供电。
- 2026-09-24：ESP32 USB COM10，A 板模拟器 COM11；COM8 是早先独立模组测试，不是当前串口。
- 实测模组原生固件 `AirM2M_780EPM_V2007_LTE_AT`。这是 AT MQTT，不是银尔达私有 `config,...` 透传固件。
- 保持 4 Hz 最新值反射率选择、10 条/2 秒 GPS 合包、事件优先、512 项 PSRAM 池和 10 秒驻留限制；SD 记录格式和全速记录不变。

## 配置与构建

复制 `config/m100m.example.json` 为 `config/m100m.local.json`，填写 broker、端口、用户名、密码、上行和 DTA1 下行 topic。当前分支开发使用独立的 `dji-h1/m100m/up` 与 `dji-h1/m100m/down`，避免旧版接收器误处理。

本地 JSON 已忽略。构建从 JSON 生成 build 目录内的 C 头文件；**固件二进制仍包含凭据，不应公开发布**。当前仅实现明文 MQTT TCP，与原开发环境一致；TLS 尚未实现。MQTT client ID 来自 B 板 MAC，格式 `DJI_` 加 16 位十六进制 source ID。

```powershell
idf.py -B build-m100m-production build
idf.py -B build-m100m-production -p COM10 flash
```

## 模组生命周期与无复位限制

`components/m100m` 仅由 telemetry task 调用。初始化与重连按状态机执行；主任务、A/B 心跳、采集与 SD 不等待蜂窝注册。

顺序：AT 探测（460800，失败交替尝试 115200）→ ATE0 → ATI → IPR460800 → AT → 清理旧 MQTT/TCP 会话 → CGATT 检查 → MQTTMODE0 → MQTTMSGSET0 → MCONFIG → MIPSTART → MCONNECT（120 秒 keepalive）→ MSUB（QoS0）。订阅确认后打印 `MQTT READY`。

每条消息用 `MPUBEX` 和明确字节长度发送；等提示符后输入二进制，等本地 OK 返回。**PUBACK 是模组下一次 QoS1 提交的流控条件，不清理应用池。只有完整身份/CRC 匹配的 DTA1 才释放应用条目。** 不再插入片间间隔。

UART 下行 `+MSUB` 按声明字节数接收，只将正确 ACK topic 的 40 字节内容交给 DTA1 解码器；二进制内 CR/LF、OK、提示符均不当作控制响应。等待 prompt/OK 时仍接收 ACK，但由外层发送结束后统一匹配池，避免释放正在发送的槽位。

已知命令模式下的连接故障允许重连，过期继续回收，不将普通网络压力锁存为 SD 故障。无法确认数据输入状态、短 payload 写入、超长/截断下行等情况隔离遥测并明确提示模组断电重启。没有硬件 RST 就不能保证任意模组卡死能自动恢复。收到关机预告时立即停止接纳/发送/重连，放弃池残留；不等待 PUBACK、DTA1 或 MQTT 断开完成，将时间留给 SD 收尾。

更精确地说：只有处于已知命令模式的连接错误/明确 ERROR，或已收到本地 OK 后缺少 PUBACK，才能自动重连。等待 prompt **或发送 payload 后等待本地 OK** 超时均视为数据输入状态不确定，禁止继续发送 AT；本地 UART 写入成功不等于模组收到完整长度。取消/解析隔离也优先于同批串口数据中较早出现的响应匹配。

池清理不等待模组 READY：过期队首先出队，再归还槽位，离线、等待 PUBACK 或隔离期间同样执行。为保留 FIFO 与生产者所有权，不移走尚未过期或正发布指针的队首；其后的过期条目等前项发送/过期后清理，不会再次发送。关机重置队列时转由 abort 清理，不误报池不变量错误。

## DTM1 线格式

每个 MQTT publication 是 **一条完整 DTM1**，小端；不含 fragment_index/count，不拆包，不跨 publication 拼接。

| 偏移 | 长度 | 内容 |
| --- | ---: | --- |
| 0 | 4 | ASCII `DTM1` |
| 4 | 1 | 版本 1 |
| 5 | 1 | 类型 1..5（沿用 DHR1/DGB1 类型） |
| 6 | 2 | flags，当前必须为 0 |
| 8 | 8 | source_id，非零 |
| 16 | 8 | mission_id，非零 |
| 24 | 4 | message_sequence |
| 28 | 4 | 内层 payload 字节数 |
| 32 | 4 | 完整内层 payload IEEE CRC32 |
| 36 | N | 原始 DHR1 或 DGB1 字节串 |
| 36+N | 4 | 前面全部 DTM1 字节的 IEEE CRC32 |

上限 4100 字节，超限拒绝而不是偷偷分片。711 点反射率内层 2233 字节，DTM1 总长 **2273 字节**；十条 GPS 内层 744 字节，总长 **784 字节**。DTA1 仍为 40 字节，其 message_crc32 指内层 CRC，不是最外层 CRC。

`fragments_sent` 等历史统计键暂保留以兼容摘要，在原生路径计数单次完整 publication；不是多个子片。`fragment_count=1` 仅为地面统一内存 API 兼容字段，不在线上传输。旧 DTF2 代码保留给历史工具/独立诊断，但本分支生产配置走 DTM1。

## 地面软件

使用本分支的 `ground_app`，将本地配置中的 MQTT broker/认证和 topics 与固件 JSON 对齐。`tools/mission_viewer` 保持原样，**不支持 DTM1**，不要用旧 validator 判断新固件是否送达。

新 API：`dji_h1_ground.telemetry.decode_message_envelope(bytes)` 返回统一 `ReassembledTelemetry` 对象，之后可用 `dji_h1_ground.messages.decode_message` 解码。`encode_message` 用于生成完整单包。网络 worker 自动识别 DTM1，在校验全部头部/长度/CRC 后进入原有持久日志、身份过滤、去重和 DTA1 流程；不创建重组状态。地图与 GUI 仍不在接收/确认路径上。老 DTF2 兼容路径仍可读取旧流量。

## 夜间不采光谱验证

```powershell
./build-m100m-test/venv/Scripts/python.exe tests/m100m_b2/verify_board_idle.py `
  --debug-port COM10 --emulator-port COM11 --reset --duration 30 `
  --output tests/m100m_b2/results/board-idle-unique-name
```

需安装 `tests/m100m_b2/requirements.txt`。先让地面 MQTT 订阅 READY，再复位 B 板；等模组 MQTT READY 后握手、连续发送 5 Hz 假 GPS，不发送 START。验证真实 GPS 合包/事件 MQTT、DTA1 回到 MCU、心跳、关机预告 ACK 和 safe 状态。日志和 SQLite/JSON 报告保留在被忽略的 results 路径。

该测试会在 SD 上创建一个真实任务目录并写入模拟 GPS/事件及摘要，不格式化、不清空卡，也不删除既有任务。成功仅证明夜间控制/传输/收尾链路；光谱采集质量、光谱大包生产负载、长航次和弱网恢复仍需后续资格测试。

## 2026-09-24 实机夜间验证结果

已构建生产 `DJI_H1` 并刷入 COM10。460800 下载在 native USB 波特率切换时失败，随后 115200 下载成功，全部写入区域通过 esptool hash 校验；这不改变 DTU UART 的 460800 工作速率。启动检测 16 MiB flash、8 MiB PSRAM，两台 H1 身份读取与曝光模式准备正常，未发送采集 START。

最终证据：`tests/m100m_b2/results/board-idle-20260924-04/`（本地忽略目录，含 board.log、summary.json、地面 SQLite）。本次 SD 任务目录为 `F_0006`。

| 项目 | 结果 |
| --- | --- |
| 模拟 A 板有效时段 | 30 秒，含 4 秒链路中断与重握手 |
| GPS 输入 / MQTT 完整恢复 | 129 / 120 条 |
| 完整上行消息 | 12 条 GPS 合包 + 4 条事件 = 16 |
| 地面 DTA1 / MCU 实际匹配 | 16 / 16 |
| 事件 | 握手两次、失联一次、恢复一次 |
| ACK RTT（本地提交完成后计时） | 均值 86.46 ms，最大 108.40 ms |
| 心跳错误码 | 全部为 0，capture/frame_count/session 始终为 0 |
| 地面 CRC/解码/存储/队列/QoS 错误 | 全部为 0 |
| DTF2 分片 / 重组残留 | 0 / 0 |
| 关机 ACK / safe | 成功 / 1 |
| 关机请求至 SD 收尾成功日志 | 125 ms |

9 条 GPS 位于未封口的遥测批次，在关机预告时按策略放弃，不是接收器漏解码。SD recorder 返回 `ESP_OK`；本次未将卡移到 PC 做文件内容读回，不能据此声称逐条 SD 内容已另行验证。关机事件本身不保证上传，因为关机预告要求优先停止遥测。

保留了三次前序日志：01 的硬件正常，但初版 verifier 把 type-5 GPS 批次错误计入 type-1 门槛而标失败；02 硬件及链路中断测试通过，报告布尔字段曾写为数值 1；03 在关机边界最后一包地面收到、MCU 未等到其 ACK 即按策略终止，被过严的“全部 ACK 必到”检查标失败。最终 verifier 修正了类型计数/布尔字段，并要求关机前 1 秒以上的已提交消息均有 MCU ACK，单独报告关机边界未确认条数。原日志未重写。

本次地面测试 87 项中 77 通过、10 跳过（当前运行时缺少可选 GUI 依赖）；DTU/配置测试 6 项通过。DTM1 C/Python 黄金向量已加入测试；C 端向量属于可选 boot self-tests，本次 production 配置未启用该启动测试组。真实大包谱线仍留待白天验证；4 Hz 上限未修改，未声称 5 Hz 生产任务已通过。

## 2026-09-24 代码审查修正：仅主机验证

本轮修正本地 OK 等待超时后的错误重连、离线队列过期槽位未释放，以及独立地面应用 API 示例只处理旧 DTF2 的问题。以上“夜间验证”是此前实机记录，不是修正后重测结果。

- `tests/native_firmware/` 直接编译生产 `m100m.c`、`telemetry.c`，用假 UART/时钟/队列测试九种模组成功/异常情形、79 种串口分块长度、重复离线回收、FIFO、生产者所有权及关机重置竞争。三个测试组全部通过；测试方法和依赖见该目录 README。
- `python -B tests/run_host_tests.py`：108 项，106 通过、2 跳过。
- `python -B -m unittest discover -s ground_app/tests -p "test_*.py"`：88 项，78 通过、10 跳过；包括直接执行中文 API 文档示例、同时验证 DTM1 和旧 DTF2。跳过项需要当前测试环境未安装的可选依赖。
- ESP-IDF v5.5.5 生产构建 `build-m100m-production` 成功；应用大小 `0x6adf0`，最小应用分区 `0x300000`，剩余约 86%。

本轮没有连接串口、刷机或发送 MQTT。确定性主机测试不等于实机并发和时序验证；须在平台重新连接后验证断网恢复、关机收尾与真实光谱负载。此前 USB 调试日志缺失的根因仍未确认，本轮未将其标为已修复。采集、SD 格式、A-B 协议、4 Hz 上限及应用层 ACK 策略均未因这些修正改变。
