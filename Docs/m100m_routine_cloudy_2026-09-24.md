# M100M 阴天十分钟常规模拟任务（2026-09-24）

## 结论

四条测线、GPS 路线、故障注入和安全关机流程完成，地面完整接收 **2910 条 GPS、336 条反射率及 27 条事件**。336 条光谱均为单个 **2273 字节 DTM1 publication**。本次云端接收未出现 CRC/解码/日志/队列错误。

**不是完整硬件资格 PASS。** COM10 的日志在约 165.216 秒后停止增长，而 COM11 心跳和 MQTT 持续正常至任务结束。自动硬件检查因此标记 `passed=false`，缺少后续三段采集/SD/时间同步及最终 shutdown 日志证据。原始结果保留，没有改成通过。日志停止的根因尚未定位；不能仅凭它断言 MCU 死机或 USB 拔出。

## 环境与方法

- 分支 `codex/m100m-b2-integration`；使用前次已刷写固件，不修改参数、不重新刷写。
- ESP32 USB COM10；模拟 A 板 COM11；M100M UART1 GPIO17/18，460800，无硬件 RST。
- 原有 600 秒 endurance 计划：四条测线、重复命令/丢 ACK、4.5 秒 A-B 失联、重握手、错误 CRC/截断帧、RTK 降级及恢复、返航、降落与关机预告。
- 实际模拟器在 590.641 秒收到 safe 心跳后结束模拟供电；没有真的切断硬件供电。
- 先通过独立 MQTT QoS1 loopback，再确认 `ground_app` 接收器订阅 READY，之后才复位/起飞。
- 上行 QoS1、DTA1 下行 QoS0；地面 SQLite 持久记录开启，不开启 GUI/地图。
- 4 Hz 最新值选择、GPS 十条/两秒批次、PSRAM 池及驻留参数不变。

## 完整地面证据

| 项目 | 数值 |
| --- | ---: |
| 不同 DTM1 消息 / MQTT publications | 658 / 658 |
| GPS 合包 / 恢复源记录 | 295 / 2910 |
| 反射率 | 336 |
| 重大事件 | 27 |
| 地面提交的 DTA1 | 658 |
| CRC/解码、QoS、持久日志错误 | 0 |
| 入口/ACK 队列溢出 | 0 |
| 入口队列峰值 | 3 |
| 重复消息 / 重组状态 | 0 / 0 |
| SQLite integrity_check | ok |

地面 DTA1 提交数不能等同于 MCU 实际清池数：COM10 截断后不能继续核对全部 MCU ACK。前段日志实际记录 186 次 DTA1 匹配，其中包括最初 98 条遥测反射率；该部分 RTT 均值约 133 ms、最大 250.5 ms，仅代表日志可见时段。

## 四段测量与记录

下表“SD 写入”来自地面收到的 `segment_end` 事件参数；第一段另有完整 USB recorder summary 交叉核对，后三段缺少 USB 详细错误计数。未拆卡进行二进制文件读回。

| 段 | raw_written | reflectance_written | 地面反射率 |
| --- | ---: | ---: | ---: |
| 1 | 561 | 100 | 98 |
| 2 | 477 | 79 | 79 |
| 3 | 480 | 80 | 80 |
| 4 | 475 | 79 | 79 |
| 总计 | 1993 | 338 | 336 |

缺少的反射率为计算序号 2、4，位于开始时的快速曝光调整阶段。第一段完整 TX 日志也没有这两条的发送尝试，池占用很低，无过期/溢出迹象；符合 250 ms 最新值采样替换，而非 MQTT 中丢失已发包。其后计算序号 5..338 全部到达。第一段 recorder 明确报告 dropped/rejected/write_errors/flush_errors/GPS drops/event drops 均为 0。

稳定阶段地面通道约 0.95 秒曝光、约 1 Hz；天空通道约 0.19 秒曝光、约 5 Hz。因此测试证明阴天常规采集+单消息上传工作流，但**不证明 4/5 Hz 持续大包压力资格**。源 GPS 模拟器总发送 2922 帧，地面记录 2910 条；没有 SD 读回与完整 USB 日志时，不将这 12 条差值直接归因为公网丢包。

## 控制、事件与关机

- 585 个心跳错误码全部为 0；四段地面测量完成数分别为 100、79、80、79。
- 模拟器协议 `failures=[]`，重连一次，四次 capture_result 均为成功。
- 地面事件包括：握手 3、段开始 4、stop_request 8、段结束 4、capture_result 4、失联/恢复各 1、CRC/超时事件各 1。
- 首次关机预告在 590.062 秒；首次成功 ACK 被故意丢弃，再发收到成功 ACK；590.641 秒心跳报告 `safe=1`，间隔约 579 ms（含心跳采样等待，不等于精确 SD flush 耗时）。
- 因 COM10 日志缺失，不能单独声称看到了最终 `Shutdown complete` 日志；实际 safe 状态由 A-B 协议确认。

## 证据与复现

本地忽略目录：`tests/m100m_b2/results/routine-cloudy-20260924-01/`。

- `flight.log`：不完整的 MCU USB 日志，截止约 165 秒。
- `emulator.log`、`flight.json`：完整模拟器控制、心跳与硬件检查结果。
- `summary.json`：完整地面计数及分项结论，保留失败标记。
- `GROUND_*.sqlite3`：完整 MQTT payload 和解码消息，已做 SQLite 完整性检查。

新增测试编排器 `tests/m100m_b2/run_routine_flight.py` 仅调用原有 endurance 模拟器和本分支地面接收器，不改变生产固件：

```powershell
./build-m100m-test/venv/Scripts/python.exe tests/m100m_b2/run_routine_flight.py `
  --debug-port COM10 --emulator-port COM11 `
  --hardware-python C:/Espressif/tools/python/v5.5.5/venv/Scripts/python.exe `
  --output tests/m100m_b2/results/NEW-UNIQUE-NAME
```

后续优先恢复/定位 USB 日志采集问题，读回 SD 任务目录，再在更亮条件下验证持续高反射率输入。不要用本次绿色地面接收计数覆盖不完整的硬件资格结论。
