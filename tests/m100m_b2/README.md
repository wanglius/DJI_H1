# M100M-B2 / Air780EPM 独立 MQTT 大包测试

本目录是 PC 侧 scratch，不修改 ESP32 固件、原 mission viewer 或 ground_app。
COM8 接 USB-TTL 适配器的 RX/TX/GND。2026-09-23 实机 `ATI` 为
`AirM2M_780EPM_V2007_LTE_AT`，不是银尔达 `config,...` 透传固件。

## 测试内容与边界

- 5 Hz 生成合成、CRC 正确的 711 点 DHR1 反射率；每条 **2233 字节**。
- 每次 `AT+MPUBEX` 发送整条二进制记录，恰好一个 MQTT publication；无 DTF2 分片。
- 上行 QoS 1、不 retain；地面订阅收到 SUBACK 后才启动模组连接与负载。
- 地面按完整记录解码并逐字节比对，然后按原有编码器发送 **40 字节 DTA1、QoS 0**。
- ACK 必须通过 broker → 蜂窝模组 → COM8 返回，设备模拟端按设备/任务/类型/序号/CRC 匹配后才释放池条目。broker PUBACK 不能清除应用池。
- 512 项保留池；3 秒 ACK 超时、最多一次应用重试；自应产生时刻起 10 秒寿命。新数据不中断接纳，池满/过期明确计数。
- 该 AT 固件实测在前条 QoS-1 发布未完成时再次发 MPUBEX 会报 767；因此依据 PUBACK 做模组流控，不用固定 sleep，也不等待地面 DTA1 才发送下一条。
- 队列持续积压或发送错过时间格将导致资格失败；不能把停止产生后慢慢排空称为“持续 5 Hz”。
- 当前只测反射率，不含 GPS、事件、ESP32 CPU、采集、SD 或 GUI 负载。地面日志是诊断证据，不声称具备生产 recorder 的掉电持久性。

无 DTF2 时身份由本次独立 topic、随机任务号、记录内 session 和确定性期望值绑定；DTA1 仍包含完整身份。**这是隔离测试协议，不可直接给生产接收器使用，也不是新的生产线格式。**

## 运行

```powershell
python -m venv build-m100m-test/venv
./build-m100m-test/venv/Scripts/python.exe -m pip install -r tests/m100m_b2/requirements.txt
```

复制 `config.example.json` 为同目录 `config.local.json`，填写自己的 MQTT broker/认证。
local 配置与 results 目录已忽略，不提交凭据。脚本每次产生新的独立 topic 和客户端 ID，不挤掉生产连接；控制台会给出上行 topic。只有这个 scratch 接收端对它发送 DTA1。

```powershell
./build-m100m-test/venv/Scripts/python.exe -m unittest discover -s tests/m100m_b2 -v
./build-m100m-test/venv/Scripts/python.exe tests/m100m_b2/run_test.py --config tests/m100m_b2/config.local.json --output tests/m100m_b2/results/run-01 --duration 60
```

输出目录必须不存在，避免覆盖证据。默认 60 秒 / 300 条，支持 1～600 秒。先做短 smoke test，再做 60 秒；初步成功后再考虑长时间资格测试。

UART 出厂实测为 115200。脚本通过 `AT+IPR` 调到 460800；不发 `AT&W`，但下一次运行前必须把 `initial_baud` 改成模组当前值，不能假设关闭 COM 会重置波特率。脚本关闭回显，不升级、清空或恢复出厂；结束关闭 MQTT 会话并释放 COM。发生数据阶段错误时只释放 COM，避免将清理命令误当成待发送 payload；应先确认 AT 状态再手动恢复。重试前用 `AT+MDISCONNECT`、`AT+MIPCLOSE` 清理已建立的测试连接。

## 报告解读

本次实机结果见 [2026-09-23 测试记录](RESULTS_2026-09-23.md)：60 秒 300/300 完整送达并收到串口 DTA1，平均约 5 Hz，但严格周期滞后检查未通过。

`summary.json`：

- `received_unique`：地面验证成功的不同记录数。
- `acknowledged_on_uart`：完整 DTA1 真正返回 COM8 并匹配成功的记录数。
- `mqtt_payload_sizes`：broker 实际交付的单条 payload 大小；应全部为 2233。
- `ack_rtt_ms`：首次发送命令开始到串口 ACK 返回，含发送、网络和地面处理。
- `at_submit_ms`：本地提示符及数据输入到 OK 的耗时，不是云端送达时间。
- `schedule_lag_ms`：首次提交相对应产生时间的滞后。
- `pool_peak`、`unsent_peak`、`awaiting_ack_peak` 区分尚未首次发送和正在等 ACK。
- `pubacks` 是模组 QoS-1 流控计数，与 DTA1 清池计数独立。
- `passed` 要求所有记录收到并在 UART 确认，池清空，无身份/CRC ACK 错误、溢出或过期，最大调度滞后小于一个采样周期。重试次数另外报告，不隐瞒。

`events.jsonl` 记录 AT 控制流程、地面接收、串口 ACK、提交耗时和过期。MCONFIG 凭据不记录；不含真实测量/地理位置。报告失败应区分未连通、单包失败、模组流控不足、地面 ACK 失败和持续吞吐不足。

## 资料依据

- [M100M-B2 硬件与固件入口](https://yinerda.yuque.com/yt1fh6/4gdtu/vcrk7q9k93f2r1nn)
- [银尔达原生 AT MQTT 测试](https://yinerda.yuque.com/yt1fh6/4gdtu/rq1k2ege1avsqga8)
- [合宙 MQTT AT 示例](https://docs.openluat.com/air780e/at/app/command/mqtt/)
- [MPUBEX 语法参考（同族型号；容量需实测确认）](https://docs.openluat.com/air780eeu/at/app/Command_List/MQTT/MPUBEX/)
- [合宙发送忙/767 说明，第 70 项](https://docs.openluat.com/faq/2026-08-17/)

不要将硬件型号、透传固件命令和原生 AT 固件命令混用；本次结论以实机版本及原始日志为准。
