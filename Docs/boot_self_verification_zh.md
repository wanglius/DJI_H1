# 生产固件每次启动的自动检查

## 目的与边界

生产 `main` 每次上电或 MCU 复位都执行硬件准备与健康检查，不依赖
`CONFIG_DJI_H1_BOOT_SELF_TESTS`。后者仍是可选的纯算法/协议资格测试，不能
替代真实硬件检查。本文对应 2026-09-25 新增实现，已做编译、主机测试及正常
60 秒实机任务。还完成了地面故意不发 ACK，以及 SD 卡和 H1-A 同时断开的故障测试；
SC16/DTU 断开等其他物理故障尚待验收，不能把这些通过扩大解释为所有故障均已验证。

不增加测试光谱、不开始采集、不格式化 SD、不覆盖历史任务目录、不自动重写 DTU
设置。SD 验证复用生产任务目录/文件创建和摘要写入，因此会像原先启动流程一样
创建一个新的 `F_xxxx`。不额外进行破坏性读写压力测试。

## 检查项与证据

| `boot_health` 名称 | 检查内容 | 失败后的行为 |
|---|---|---|
| `flash` | 配置/物理 Flash 都为 16 MiB | 不允许进入采集 READY |
| `psram` | PSRAM 初始化、8 MiB 容量、可用堆 | 不允许进入采集 READY |
| `sd_mount` | FAT32 挂载 | SD 错误，继续检查独立的 SC16/H1 |
| `sd_recorder` | 读容量、初始化记录器、创建任务文件和摘要 | SD 错误，不采集 |
| `sc16_bus` | SPI/驱动资源初始化与复位 | 两个通道和 H1 标记 blocked |
| `sc16_a` / `sc16_b` | 通道 scratchpad 读回与 UART 初始化 | 对应 H1 blocked，另一路仍检查 |
| `h1_ground` / `h1_sky` | 获取设备信息、设置并读回 AUTO 曝光模式 | 不允许进入采集 READY |
| `telemetry_uart` | 遥测池、队列、UART、任务建立 | 遥测降级，但不阻止 SD 记录 |
| `dtu_profile` | YY-M200 只读 AT 设置核对、确认退出 AT 模式 | 禁止本次启动的二进制遥测发送 |
| `dtu_network` | `AT+SOCKLK=1A` 返回 ON | 降级；正确配置且已退出 AT 时允许后续恢复 |
| `ground_ack` | 首次实际发送后收到身份/类型/序号/CRC 匹配的正 DTA1 | 未验证/超时明确报告，不阻塞采集或关机 |

`sc16_bus=pass` 本身不是芯片响应证据，需同时看通道读回结果。
H1 准备成功也不等于已经验证采样率、亮度或完整测量链路；这些仍需模拟飞行验证。

状态值为 `pending`（尚未完成）、`pass`、`fail`、`blocked`（前置条件失败）及
`waiting`（等待外部条件）。不能把 waiting 当作已通过。每项另保存 ESP 错误整数。
这些是启动时证据，不是持续可用性保证；运行期原有丢帧、写入、超时统计继续生效。

## 启动次序、超时与取消

1. `app_main` 记录 Flash/PSRAM 检查，建立时间服务和遥测任务，然后启动 mission/control
   与 A/B UART 任务。能运行到应用层的容量核对失败不再直接触发复位循环。
2. mission worker 检查 SD 和记录器，再独立检查 SC16/H1。保留 SC16 初始化后 500 ms
   延时；两路分别报告，不因 H1-A 失败而漏掉 H1-B。H1 沿用现有命令接收超时，
   命令之间响应采集/关机取消信号。
3. 遥测任务作为 UART1 唯一所有者，在发送二进制数据前完成 AT 检查。保留 1.2 秒
   发送静默期、`+++` 后等待 0.5 秒再发 `a`；命令接收上限 600 ms。配置/网络检查的
   总轮询预算为 20 秒，随后额外执行有界 AT EXIT 清理（正常情况下远小于一秒）。
4. 不扫描波特率、不自动恢复出厂、不重启 DTU、不写 broker/topic/UART/认证配置。
   无响应可能是电源、波特率、线序或模块类型问题，不能仅凭超时断言模块损坏。
5. 握手就绪会等有限 AT 检查结束，但不要求检查成功。SD/H1 或板型问题阻止 READY；
   仅遥测失败仍可完成握手并执行 SD 采集。
6. `ground_ack` 在无实际发送时一直 waiting；从第一次实际发送尝试开始计时，10 秒内
   没有匹配正 ACK 则 fail。后续有效 ACK 可将该项和网络项恢复为 pass。历史丢失、
   超时/失败计数不会因此清零，故心跳可能仍为 error 5。不会主动伪造 GPS/光谱做探针。
7. 关机请求取消 AT 后续检查/网络轮询，仅做有界 EXIT 清理；SD 关闭不等待遥测任务。
   原有遥测丢弃优先的关机策略保持不变。

总检查时间不是所有底层存储/驱动故障的硬实时保证；SD 驱动自身超时、SDK 启动失败
或资源耗尽仍需单独故障注入验证。发生在 `app_main` 之前的 Flash/PSRAM 故障无法
通过本应用上报；时钟服务、mission/A-B 任务无法建立等严重软件启动错误仍可能中止。

## DTU 配置来源与保密

生产检查与手工配置使用同一份 JSON，避免维护两套 broker/topic 常量。
构建时优先读取 `tests/dtu_uart_bridge/dtu_mqtt_config.local.json`（已忽略）；
不存在时使用 example 并显示 CMake 警告。example 中的 `mqtt.example.com` 不是
实际部署地址，直接使用示例构建会导致现场 DTU 核对失败。

```powershell
# 可显式指定本机部署 JSON；不要提交凭据
idf.py -B build-production -D DJI_DTU_BOOT_PROFILE=C:/private/dtu_mqtt_config.local.json build
```

`tools/generate_dtu_boot_profile.py` 生成 build 目录内的头文件，只含 UART、
broker/端口、上下行 topic/QoS、keepalive、RAW 模式、缓存/注册/心跳开关的读回值。
**不编译 client_id、username、password，不查询 MQAUTH，不打印 AT 原始响应**。
broker/topic 仍会进入固件镜像；它们不应被视为密码。
认证、消息路由和地面处理必须由真实 DTA1 验证。模块协议类型仅支持主分支 YY-M200，
不能拿此检查器验证 M100M。生产要求 offline cache OFF，避免过期数据在模块端积压。
修改本地 JSON 需重新构建，且这不会自动改变模块保存的配置；部署配置变更仍需人工授权。

若 AT 进入或退出确认失败，宁可关闭本次启动的遥测，也不把二进制数据送入未知模式。
排障后重新启动 B 板；没有运行中强制改写/重试 AT 探测，以免打断业务数据流。

## 对外报告与 A/B 协议兼容

- USB 调试端：`BOOT_HEALTH` 输出各项变化和汇总。`DTU_CHECK` 只输出失败的查询名，
  不输出配置值。`Initialization: ESP_OK` 仅表示 mission 的关键设备分支可用，
  不代表 modem/ground ACK 全部通过；必须同时阅读 BOOT_HEALTH。
- SD：`MISSION.JSON.boot_health` 是命名对象，例如
  `"h1_ground":["fail",263]`。沿用 summary schema 1 的可选扩展字段，不改变 DHR1。
  SD writer 在原有周期 flush 边界检查健康变化，只在变化时写摘要；无 A 板握手也能保存。
  初始 JSON 可能含 pending，之后更新，正常最终关闭时再保存最新结果。缺卡无法写 SD。
- A 板：不改帧长度或新增错误码。仍用 `b_ready=0/1`、`b_state` 和 error 1（SD）、
  2（初始化）、5（遥测/数据降级）；error 3/4 原义不变。
- **协议限制**：未完成 READY 握手时本实现不发送正常心跳。关键启动故障只能以
  `b_ready=0` 告诉 A “未就绪”，不能在这个 5 字节握手回包里塞具体故障原因。
  具体原因从 USB 或可写 SD 查看。如需 A 在初始化阶段识别缺卡/H1/SC16，必须与
  A 板团队约定协议扩展，不能擅用 reserved 字节。
- 尚未为每条 boot check 新增 MQTT 事件类型；建立链路后的原有状态/事件机制不变。
  DTU 或地面链路失败时也不可能保证通过该失败链路报告自身故障。

## 测试与实机验收

```powershell
python -B tests/run_host_tests.py
# 可选编译器，用于执行实际 C 检查器的模拟 UART 故障测试
python -m pip install ziglang
python -B -m unittest discover -s tests/boot_health -v
```

Native 测试覆盖正常读回、错误 UART 设置、网络离线/延迟上线、AT 无响应、退出失败、
取消、过长响应、板配置不匹配以及 JSON/故障分类边界。它们不能替代实际 YY-M200
固件对 AT 转义、回包时序的验证。

### 2026-09-25 实机结果

ESP32 COM10、模拟 A 板 COM11、被动监听 COM8/COM9，YY-M200 保持原有持久配置。
首轮启动暴露了新检查器的解析错误：模块 `+++` 后发出的 `a` 提示没有换行，组合
响应实际为 `a+ok`，不能只匹配单独的 `+ok` 行。已修正精确 token 识别，新增含提示符
和大小写变体的 native C 回归（合计 12 个情形），重新构建并刷写。

- 正常任务 `F_0030`：DTU 配置/网络在启动约 2.72 秒通过，ground ACK 在约 3.00 秒通过；
  地面收到 275 条 GPS（30 批）、57 条被选中的反射率、13 条事件。100 个逻辑消息均有
  匹配 ACK，无过期。SD 两段共记录 165 条 raw、111 条反射率，零 raw/GPS/event 丢弃、
  零计算拒绝、零写入/flush 错误；遥测反射率少于 SD 是 4 Hz 选帧，不是 SD 丢失。
  心跳 error 0，时钟锁定，正常关闭并卸载。COM8/9 与参考字节流一致，无协议 CRC 错误。
- 故意不发 DTA1 的任务 `F_0031`：启动 profile/network 正常，约 12.86 秒报告
  `ground_ack=fail/ESP_ERR_TIMEOUT`（距首发约 10 秒），采集中持续 error 5，仍完成两段
  采集和 SD 记录：165 raw、111 反射率，零记录丢弃、拒绝、写入/flush 错误。
  关机不等待残留 ACK，最终 `safe_power_off=1`、`ESP_OK`。这是预期降级，不是正常
  遥测交付通过；日志中保留的 timeout/error 5 不应删除或当作正常运行。
- 日志位于本机忽略的 `build-main-yy-m200/sniffer-20260925-155120/` 和
  `build-main-yy-m200/no-ack-20260925-155326/`。首个失败的纯启动探测创建 `F_0029`，
  未启动采集，未把它计入通过的飞行记录。未把 SD 卡接入 PC 读回 JSON/二进制，
  SD 结论来自固件写入、flush、close 和 unmount 的成功报告。

### 缺 SD 卡 + H1-A 断开（同日 16:03）

只复位并观察 30 秒，通过 COM11 每秒请求握手，不发送 START 或定位数据。
SD 挂载约 656 ms 报 timeout，记录器 blocked；H1-A/ground 约 2182 ms 报 timeout
（1000 ms 内收到 0 字节）。两路 SC16 寄存器检查均通过，H1-B/sky 约 2258 ms 通过；
DTU 配置/网络约 2725 ms 通过。证明 SD 故障不会遮蔽 H1 故障，H1-A 故障也不会
阻止检查 H1-B。无任务消息时 ground ACK 正确保持 waiting，不误报成功或超时。

29 次握手全部得到版本/SEQ 正确的 `b_ready=0` 回复。仅出现一次预期启动，无 panic、
看门狗或反复重启，未开始采集。按协议未建立 READY 链路，故不发送正常心跳，具体
故障仍需 USB 查看；缺卡不能保存本次诊断到 SD。COM10/11 已释放。
日志：`build-main-yy-m200/missing-devices-20260925-160348/`。
临时报告解析器最初遗漏含数字的 `sc16`/`h1` 名称，修正为允许数字后重放保存日志并通过；
没有因此修改固件或伪装成第二次硬件测试。

### 恢复连接后的回归（同日 16:10）

用户恢复 SD/H1-A 连接后，使用相同固件复位并执行 60 秒正常任务 `F_0033`。
地面 validator 先 READY，再操作 COM10/COM11；按用户要求完全不打开或探测 COM8/9，
监听器由用户自行检查，自动报告中 `sniffer_passed=null`（未测，不能填通过）。

13 项检查全部恢复 pass；SD 挂载约 701 ms、H1-A 约 1578 ms、H1-B 约 1654 ms、
DTU 设置/联网约 2721 ms、ground ACK 约 3042 ms 通过。两段 SD 共记录 171 raw、
113 反射率，零 raw/GPS/event 丢弃、计算拒绝、写入/flush 错误；心跳 error 0，
时钟锁定，关闭/卸载成功且 safe_power_off=1。地面收到 275 GPS（30 批）、58 反射率、
13 事件，最终无未完成重组。日志：`build-main-yy-m200/recovery-20260925-161057/`。
此测试验证重新启动后的恢复，不是运行中热插拔自动恢复；未重刷或改写 DTU 配置。

下一次硬件验收建议：先地面 validator READY，再正常冷启动，确认各项 pass（ground ACK
允许先 waiting），做 60 秒任务并检查 SD 正常关闭。随后逐项断电改线/取卡后重新上电：
缺卡、单 H1 缺失、DTU 缺失、地面 ACK receiver 停止。核对明确的 fail/blocked、
遥测故障不阻止 SD、关机不等待 ACK，最后恢复全套并做长任务。不在运行中热拔硬件。
