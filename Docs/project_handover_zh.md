# DJI_H1 无人机载双光谱仪项目交接手册

> 文档状态：按 `main` 分支 DGB1 GPS 合包与 4 Hz 饱和遥测资格里程碑（2026-09-15）整理
> 适用对象：首次接手本项目的嵌入式、上位机、数据处理及联调工程师
> 目的：说明当前系统能做什么、如何运行、数据如何流动、如何验证，以及仍有哪些风险和待办事项

## 0. 五分钟接手路径

第一次打开仓库时，先在仓库根目录确认自己面对的版本和未提交改动。不要为了得到“干净状态”而丢弃他人的工作区修改：

```powershell
git status --short --branch
git log -5 --oneline
git remote -v
```

当前交接基线应位于 `main`，以“本文所在提交”为准；若 HEAD 更新，以当前源代码、测试和相应格式/协议文档为准，并在继续开发前更新本文的基线说明。随后按下面顺序建立最小可信环境：

1. 安装并激活 ESP-IDF v5.5.5；桌面工具需要 Python 3.10 或更新版本；
2. 运行 `python -B tests/run_host_tests.py`，确认纯主机协议、记录格式、模拟器、遥测和查看器测试通过；
3. 从仓库根目录运行 `idf.py -B build-review build`，确认构建的是生产 project `DJI_H1`，而不是 `tests/` 下会覆盖生产固件的独立诊断 project；
4. 依次阅读第 3 节列出的 A/B 协议、记录格式、时间戳、遥测格式和模拟飞行 SOP；
5. 只有在确认专用 N16R8 板、FAT32 SD 卡、两台 H1、实际 COM 端口和地面 validator 都就绪后，才进行刷写和硬件飞行。

对自动化 agent 的工作边界也应明确：先检查 Git 状态并保留用户改动；不得把 `sdkconfig` 当作受版本控制的配置源；本地 AK/MQTT/SIM 凭据只在用户明确要求相应联调时按需读取，绝不打印或提交；不得把诊断固件的参数写回生产默认值；没有用户明确授权时不要刷写硬件、删除任务数据、提交、推送或访问已取出的 SD 卡盘符。

## 1. 项目概述

DJI_H1 是一套无人机载双光谱仪采集与记录系统。当前 B 板以 ESP32-S3-WROOM-1U-N16R8 为主控，通过 SC16IS752 双 UART 扩展芯片同时连接两台 H1 光谱仪：

- SC16 通道 A / H1-A：地面（Ground）光谱仪；
- SC16 通道 B / H1-B：天空（Sky）光谱仪；
- B 板通过原生 UART0 与无人机侧 A 板通信；
- B 板从 A 板接收握手、定位、UTC 时间和采集控制命令；
- B 板在 SD 卡中保存原始光谱、表观反射率、航迹、事件和任务摘要；
- PC 端 Python/PyQt 工具可以解码任务目录、插值光谱测点位置，并显示航迹、事件和反射率曲线。

当前项目已经完成并在硬件上验证的主链路是：

```text
A 板模拟器 ──UART0──> B 板状态机
                        │
                        ├── SPI3 ──> SC16IS752 ──> H1-A（地面）
                        │                         └> H1-B（天空）
                        │
                        ├── 反射率计算
                        │       └──> PSRAM 遥测池 ──UART1──> 4G DTU ──> MQTT
                        │
                        └── SPI2 ──> FAT32 SD 卡（权威数据）
                                      ├── RAW_SPECTRA.BIN
                                      ├── REFLECTANCE.BIN
                                      ├── GPS_TRACK.BIN
                                      ├── EVENTS.JSONL
                                      └── MISSION.JSON
```

4G DTU/MQTT 实时上传链路已经接入：A 板 5 Hz GPS 在 SD 卡上仍逐条保存为 98 字节 DHR1，但遥测把最多 10 条相邻 GPS 无损压成一个 DGB1 消息（满 10 条或首条等待 2 秒即封包），从而把正常 GPS 消息和 DTA1 数量降到原来的约十分之一。反射率在 SD 卡保持全速记录，实时遥测每 250 ms 只选择最新一帧（上限 4 Hz）。两类消息共用 PSRAM 保留池，UART 连续发送，云端 DTA1 确认后才释放。测量上行保持 MQTT QoS 1；DTA1 下行使用 QoS 0，丢失的 ACK 由 B 板应用层重试和 10 秒最大驻留策略处理。网络拥塞不得阻塞采集或 SD 写入。2026-09-15 已在专用板、YY-M200 和真实 EMQX 链路上完成 DGB1 及 4 Hz 饱和长航次资格测试；最终高照度十分钟任务在地面完整恢复 2910 条 GPS 和全部 1277 条已选反射率，零 GPS 序号缺口、零未完成重组，并安全收尾。完整设计、线格式、计数器和实测数据见第 7.1 节。

## 2. 仓库和版本基线

- GitHub：`https://github.com/wanglius/DJI_H1.git`
- 主分支：`main`
- 本文整理时基线：本文所在的 DGB1 GPS 合包与 4 Hz 饱和遥测资格提交
- ESP-IDF：v5.5.5
- 主要语言：C、Python、Markdown
- 固件目标：ESP32-S3
- 桌面端：Windows + Python + PyQt6/PyQt6-WebEngine

接手后首先执行：

```powershell
git status --short --branch
git log -5 --oneline
```

本文中的提交号用于确认交接基线，不应被当作永远固定的版本号。若仓库已继续演进，应优先以当前代码和最新提交记录为准。

## 3. 重要文档及其权威性

| 文档 | 用途 | 说明 |
|---|---|---|
| [AB板串口通信协议_V1.0.md](AB板串口通信协议_V1.0.md) | A/B 板通信协议 | 协议字段、命令、重试和时序的首要依据 |
| [timestamp_architecture.md](timestamp_architecture.md) | 时间同步架构 | 说明 B 单调时钟、A 时钟、UTC 和同步状态 |
| [record_format_v01.md](record_format_v01.md) | SD 二进制记录格式 | 说明文件头、记录头、CRC 和各类负载 |
| [mqtt_telemetry_transport.md](mqtt_telemetry_transport.md) | MQTT 遥测线格式与可靠性 | DTF2、DTA1、发送调度、保留池及接收端契约的首要依据 |
| [emulator_test_sop.md](emulator_test_sop.md) | 模拟飞行与遥测联调 SOP | 地面 validator 必须先 READY，再允许模拟器起飞 |
| [A_board_controlled_acquisition.md](A_board_controlled_acquisition.md) | A 板控制采集的演进记录 | 含有历史阶段描述，部分“尚未实现”内容可能已过时 |
| [tests/ab_board_emulator/README.md](../tests/ab_board_emulator/README.md) | A 板模拟器测试说明 | 硬件联调入口 |
| [tools/mission_viewer/README.md](../tools/mission_viewer/README.md) | 任务查看器说明 | 安装、启动、API 和地图配置 |

不要用一个笼统优先级掩盖冲突，应按领域判断：

- **当前实现行为**：以当前源代码和自动化测试为证据；
- **A/B 对外通信契约**：以 A/B 协议原文为准；源代码不一致时应判定为实现缺陷，而不是静默修改协议解释；
- **SD v01 线格式**：以 `measurement_records.h`、`data_records.c` 和 `record_format_v01.md` 共同核对；三者不一致时停止写入新格式，先确定是否需要升版；
- **DTF2/DTA1 线格式**：以 `telemetry_transport.h/.c`、Python decoder 和 `mqtt_telemetry_transport.md` 的共同测试向量为准；
- **时间模型**：以 `clock_sync` 实现和 `timestamp_architecture.md` 共同核对；
- **历史演进文档**：只提供背景，不能覆盖现行接口和线格式。

发现上述任一契约与实现不一致时，应记录为待修问题并同时更新 C、Python、测试和文档；不能只挑其中一份作为“正确答案”后继续开发。

## 4. 硬件架构与接线

### 4.1 当前开发硬件

早期开发使用 LilyGO T8-S3，后续已经迁移到集成 SC16IS752 的专用 ESP32-S3-WROOM-1U-N16R8 板（16 MB Flash、8 MB Octal PSRAM）。当前及后续测试默认以专用板为准。启动代码会核对物理/配置 Flash 容量和 PSRAM 容量，不匹配时停止启动，防止错误的板型配置带病进入任务。

开发机上最近使用的端口分配为：

- `COM6`：ESP32-S3 原生 USB，用于下载固件和 USB Serial/JTAG 日志；
- `COM5`：USB 转 UART 桥，用于运行 A 板模拟器。

Windows 的 COM 编号可能随 USB 接口和设备枚举变化，因此脚本参数必须按设备管理器中的实际端口修改，不能把 COM5/COM6 当作硬件常量。

### 4.2 A/B 板 UART

定义位于 `components/board_support/include/dji_h1_board.h`：

| 信号 | ESP32-S3 GPIO | 当前用途 |
|---|---:|---|
| TX | GPIO43 | B 板向 A 板发送 |
| RX | GPIO44 | B 板接收 A 板数据 |
| UART | UART0 | A/B 协议链路 |
| 波特率 | 115200 | 8N1，无流控 |

电气层使用 3.3 V TTL，必须共地。ESP-IDF 控制台配置为 USB Serial/JTAG，因此 UART0 可以专用于 A/B 通信。更换 PCB 引脚时优先修改 `DJI_AB_UART_TX_GPIO`、`DJI_AB_UART_RX_GPIO` 和 `DJI_AB_UART_PORT`，不要在业务代码中散布数字常量。

### 4.3 SD 卡 SPI

专用板默认值集中在 `components/board_support/include/dji_h1_board.h`；`sd_card.h` 只定义可复用组件 API 和运行时配置结构：

| 信号 | GPIO |
|---|---:|
| CS | 10 |
| MOSI | 11 |
| SCLK | 12 |
| MISO | 13 |
| SPI 主机 | SPI2 |
| 时钟 | 10 MHz |
| 挂载点 | `/sdcard` |
| 最大同时打开文件数 | 8 |

当前要求使用 FAT32。固件设置 `format_if_mount_failed = false`，挂载失败时不会自动格式化卡；但是正常任务会创建目录和写文件，所以仍不要用含有唯一重要数据的卡做联调。Windows 资源管理器通常不为 64 GB 卡提供 FAT32 格式化选项，需要使用可信的 FAT32 格式化工具，并在写入前备份数据。

### 4.4 SC16IS752 与 H1

专用板默认值集中在 `components/board_support/include/dji_h1_board.h`；`mission_control.c` 只把这些值装入 SC16 运行时配置：

| 信号/参数 | 当前值 |
|---|---|
| SC16 SPI 主机 | SPI3 |
| MOSI | GPIO2 |
| MISO | GPIO3 |
| SCLK | GPIO5 |
| CS | GPIO1 |
| RESET | GPIO6 |
| SPI 时钟 | 4 MHz |
| SC16 晶振 | 1.8432 MHz |
| 两路 UART | 115200, 8N1 |

SC16 初始化完成后，固件固定等待 500 ms，再准备两台 H1。该延时解决了“刷写固件后的第一次启动容易卡在 H1-A preparation timeout”的问题，目前不要随意删除或缩短。

若 PCB 改版，应先修改统一板级配置头，并同步检查 SD、A/B UART、DTU UART 和诊断 project。专用板原理图、BOM 和电源时序说明目前仍不在本仓库，是交接资料的一个缺口。

## 5. 固件启动流程

`main/main.c` 中的 `app_main()` 保持很薄，当前顺序为：

1. 输出固件启动横幅；
2. 核对 16 MB Flash 和 8 MB PSRAM，并运行 PSRAM 启动内存测试；
3. 调用 `startup_checks_run()`；正常生产配置中 `CONFIG_DJI_H1_BOOT_SELF_TESTS` 关闭，因此该调用不执行测试体；资格镜像显式开启该选项时才运行数据流水线、A/B 协议、遥测传输、时间同步和计算自检，并清除自检产生的假定位数据；
4. 启动 `clock_sync`；
5. 从 ESP32 出厂 MAC 生成遥测 `source_id`，初始化 UART1 和 512 项 PSRAM 遥测池；
6. 启动 `mission_control`；
7. 启动 `ab_link`。

固件上电后不会立即采集。`mission_control` 先挂载 SD 卡、创建本次飞行目录、初始化记录器、初始化 SC16 并准备 H1；完成后进入 READY，等待 A 板命令。

## 6. 软件模块划分

### 6.1 `main` 层

| 文件 | 职责 |
|---|---|
| `main/main.c` | 启动与自检编排 |
| `components/board_support/` | 量产板全部 UART、SPI、存储与内存配置 |
| `main/ab_link.c` | UART 收发、协议解析、握手、实时数据、控制命令、ACK、1 Hz 心跳 |
| `main/mission_control.c` | 飞行任务生命周期的唯一所有者，协调 SD、记录器、SC16/H1 和采集 |
| `main/acquisition.c` | 两路 H1 采集任务、SC16 RX 服务、采集统计和诊断日志 |

### 6.2 组件层

| 组件 | 职责 |
|---|---|
| `ab_protocol` | A/B 帧编解码、CRC、流式解析器，以及唯一的 30 字节实时数据解码定义 |
| `clock_sync` | A 时钟、B 单调时钟、UTC 的拟合与状态管理 |
| `drone_data` | 保存最近一次由 A 板发送的定位/飞行数据快照 |
| `sc16is752` | SPI 驱动双 UART 桥、双通道高优先级 RX 排空与错误统计 |
| `h1` | H1 命令协议、设备信息、曝光、单帧/流式帧、重同步和停止 |
| `calculation` | 用地面光谱和最近的因果天空光谱计算表观反射率 |
| `data_records` | 内存数据结构、线格式常量、格式版本和质量标志 |
| `gps_batch` | DGB1 GPS 遥测批次的组装、无损序列化、校验和单条 DHR1 重建 |
| `measurement_recorder` | 固定内存池、消息队列、单 SD 写任务、多文件记录、周期刷盘与收尾 |
| `sd_card` | SD 挂载、目录、文件、刷盘、修改时间和原子替换的串行化封装 |
| `telemetry_transport` | DTF2 分片、CRC、DTA1 编解码；与 UART、MQTT 和任务调度无关的纯传输线格式 |
| `telemetry` | 512 项 PSRAM 保留池、4 Hz 反射率选择、5 Hz GPS 的 10 条/2 秒批处理、UART1 独占发送、ACK 匹配、重试、过期和断电放弃 |

重要的不变量：`CONFIG_FATFS_FS_LOCK=0`，项目依赖 `sd_card` 组件内部互斥锁保护 FatFs。卡挂载后，其他模块不得绕过该组件直接调用 `fopen()`、`write()` 等访问 `/sdcard`，否则多任务访问可能破坏文件系统一致性。

## 7. RTOS 任务和并发模型

| 任务 | 优先级 | 栈 | 核 | 说明 |
|---|---:|---:|---:|---|
| `sc16_dual_rx` | 12 | 4096 | 1 | 最高优先级排空两路 SC16 FIFO |
| `h1_acq_a` / `h1_acq_b` | 8 | 6144 | 1 | 两路光谱采集 |
| `ab_link` | 8 | 4096 | 不固定 | A/B 收发和心跳 |
| `clock_sync` | 6 | 4096 | 不固定 | 时间观测处理 |
| `mission_control` | 5 | 6144 | 不固定 | 串行化生命周期变化 |
| `measurement_writer` | 4 | 8192 | 0 | 唯一 SD 数据写任务 |
| `h1_dual_log` | 4 | 4096 | 0 | 采集诊断日志 |
| `telemetry_tx` | 3 | 6144 | 0 | 唯一 DTU UART 所有者；发送、下行 ACK 解析、重试和过期回收 |

关键设计原则：

- SC16 硬件 FIFO 只有 64 字节，必须由高优先级 RX 服务及时搬运到每通道 8192 字节的软件流缓冲区；
- 两个 H1 采集任务互相独立，不由单任务轮询两台传感器；
- SD 写入统一交给一个 writer，避免多个文件线程争用 FatFs/SPI；
- 采集生产者提交记录时不等待慢 SD。内存池或队列满时明确计数并丢弃记录，保持采集和心跳存活；
- 生命周期操作只由 `mission_control` 执行，协议任务不直接初始化或销毁硬件；
- 当前 FreeRTOS tick 为 1000 Hz，因此 `vTaskDelay(pdMS_TO_TICKS(1))` 确实会让出约 1 ms；
- Task Watchdog 监视两个 CPU 的 idle task，采集启动时会按需要重配 TWDT。

采集连续出现 3 个帧错误时，系统停止两路采集并通过统一清理路径恢复到安全状态。H1 单帧接收超时为 5 s。停止时必须先停止所有读取者，再销毁 SC16 软件流缓冲区；不可随意调整该顺序。

### 7.1 4G DTU / MQTT 实时遥测

#### 7.1.1 定位和职责边界

遥测是 SD 记录之外的“实时观察副本”，不是任务数据的权威存储。当前上传两类测量语义：

- `GPS_BATCH`（消息类型 5）：最多 10 条已经完成的 v01 GPS 记录组成的 DGB1 无损批次；消息类型 1 只为兼容旧固件留下；
- `REFLECTANCE`（消息类型 3）：地面帧与因果天空帧计算完成后的完整反射率记录。

原始 A/B 光谱和操作事件目前只写 SD，不经 4G 上传。GPS 和反射率在 `measurement_recorder` 边界分叉成 SD 与遥测两条独立路径；两条路径不承诺先后次序，遥测副本进入池也不等于 SD 已经落盘。提交给 `telemetry` 的记录内容在其整个保留期内保持不可变。因此必须一直保持以下边界：

```text
A 板 5 Hz 定位 ──> measurement_recorder ──> GPS_TRACK.BIN（逐条 DHR1，权威）
                 └─> gps_batch（10 条或 2 秒）──> telemetry pool
                                                   └─> DTF2 ──> DTU ──> MQTT

H1-A/H1-B ──> calculation ──> measurement_recorder ──> REFLECTANCE.BIN（全速、权威）
                                  └─> latest-value 4 Hz ──> telemetry pool
```

网络拥塞、DTU 断线、ACK 丢失、池满或条目过期都不得阻塞 H1、`ab_link` 或 SD writer。SD 成功写入和 MQTT 成功送达是两个独立结论；任何地面软件都不得用“MQTT 中看到了数据”替代 SD 任务文件的完整性检查。

#### 7.1.2 硬件和 DTU 持久配置

生产板参数集中在 `components/board_support/include/dji_h1_board.h`：

| 参数 | 当前生产值 |
|---|---:|
| ESP32 UART | UART1 |
| ESP32 TX / RX | GPIO17 / GPIO18 |
| 串口格式 | 460800 baud，8N1，ESP32 端无硬件流控 |
| DTF2 单分片最大线长 | 1024 字节 |
| 应用分片间隔 | 0 ms，连续写入 UART |
| DTU UART 打包 | 1024 字节阈值、约 5 ms 空闲超时 |
| 上行 MQTT QoS | 1 |
| 下行 DTA1 发布 QoS | 0 |
| GPS 输入/遥测目标 | 5 Hz / 全部记录进入 DGB1，最多 10 条或 2000 ms 一包 |
| 反射率 SD/遥测 | 全速记录 / 每 250 ms 选最新一条，最高 4 Hz |
| PSRAM 保留池 | 512 项，约 1.6 MiB |
| 首次 ACK 超时 | 3000 ms |
| 正常重试 | 1 次；第二次等待名义上为 6000 ms |
| 最大池驻留 | 自接纳起 10000 ms |

DTU 是透明串口模块，MQTT broker、端口、client ID、用户名、上/下行 topic、QoS、UART 波特率和打包参数保存在 DTU 自身的持久配置里，而不是由飞行固件每次启动下发。当前开发 topic 为 `dji-h1/test/up` 和 `dji-h1/test/down`。受版本控制的配置模板是 `tests/dtu_uart_bridge/dtu_mqtt_config.example.json`；实际部署应复制为同目录的 `dtu_mqtt_config.local.json`，该名称已被局部 `.gitignore` 排除。broker 地址和认证信息属于部署配置，正式产品不得硬编码密钥或提交真实密码。更换 DTU、SIM、broker 或 UART 参数后，应先使用 `tests/dtu_uart_bridge/configure_dtu_mqtt.py` 和双向验证脚本单独确认，再刷回生产固件。

当前 YY-M200 在实测中稳定使用 460800 baud。曾尝试 921600，但现有模块固件拒绝该 AT 参数，因此生产值不得仅按手册宣称修改。DGB1 十条 GPS 合包已经通过 C/Python 协议测试、生产固件构建、60 秒板级联调和 10 分钟真实 DTU/EMQX 全功能任务；当前室内模拟任务下不再表现为主要吞吐瓶颈。候选 Air780、单包超过 4 KB、选择性/位图 ACK 等仍只是后续方案，当前固件均未实现。

#### 7.1.3 身份、任务和序列号

每个 DTF2 分片携带完整的逻辑身份：

```text
(source_id, mission_id, message_type, message_sequence)
```

- `source_id`：启动时读取 ESP32 出厂 48 位 MAC，按网络可读顺序装入非零 `uint64_t`；它标识物理 B 板，不依赖 SD 卡或固件版本；
- `mission_id`：任务目录成功打开后，由两个 `esp_random()` 组合出新的非零 64 位值，并写入 `MISSION.JSON`；它不同于 `F_####`，换卡、格式化或目录编号重复也不应复用；
- `message_type`：旧版单条 GPS 为 1，反射率为 3，DGB1 GPS 批次为 5；1..4 与 `data_record_type_t` 对齐，5 是纯遥测扩展；
- `message_sequence`：反射率沿用 v01 内层记录 sequence；DGB1 使用批次第一条 GPS 的 record sequence。反射率经过 latest-value 选择后，地面看到序号间隙是正常现象，不能据此把未选择上传的帧误报成 SD 丢帧。

接收端去重、重组和 ACK 必须使用上述身份以及完整消息 CRC。仅按 sequence 去重会把不同设备、不同任务或不同类型的数据错误合并。

#### 7.1.4 反射率选择和 GPS 调度

GPS 不做 latest-value 覆盖：每个被 A/B 链路接受并成功封装的 5 Hz GPS 记录都先逐条交给 SD writer，同时尝试追加到当前 DGB1 遥测批次。批次从第一条的本地接纳时刻开始计时，并在以下任一条件出现时封包：记录数达到 10、首条等待达到 2000 ms、session/segment/公共 DHR 头变化、或正常任务结束。记录顺序和序号缺口均被保留。这样可恢复完整航迹，同时把通常约 2900 个十分钟 GPS 消息降至约 290 个批次。批次等待属于实时延迟预算的一部分，不会把 10 秒最大驻留向后平移。

反射率采用“时间格最新值”策略：

1. 每个有效计算结果都计入 `reflectance_offered`，并始终写入 SD；
2. 当前 250 ms 时间格内只在池中暂存最新候选；
3. 新结果替换尚未到发送时刻的旧候选时，增加 `reflectance_rate_limited`，这属于设计内降采样，不是故障；
4. 到达时间格边界时，只把当时最新候选送入可靠发送 FIFO；空时间格不补发，任务延迟后也不突发“补课”；
5. 被选中的记录保留原始测量时间戳、计算序号和地面/天空帧关联。

发送任务的优先次序是：新 GPS 批次、普通 ACK 超时重试、到期的恢复探针、最后是新反射率。GPS 批次优先是为了避免大反射率报文和旧重试淹没航迹。生产 10 秒驻留期限短于 30 秒恢复探针周期，因此正常生产条目会在进入探针阶段前过期；探针逻辑主要服务于禁用或放宽驻留期限的诊断配置。

#### 7.1.5 PSRAM 固定池和所有权

组件启动时一次性在 8 MB PSRAM 中分配 512 个 `telemetry_entry_t`，运行中不为单条消息 `malloc/free`。联合体每项既能容纳最大 1024 采样点的反射率，也能容纳 10 条 GPS；最大成员仍是反射率，因此 DGB1 不显著扩大 PSRAM 池。FreeRTOS 队列中只传 4 字节指针，不按值复制记录。三个队列分别保存空闲项、GPS 批次 ready 指针和反射率 ready 指针。第一条 GPS 到来时就从池中保留一项并进入 `STAGED`，后续九条原位追加；封包后内容不可再修改。

条目生命周期的主要状态转换为：

```text
FREE -> FILLING -> STAGED/ENQUEUING -> QUEUED -> SENDING
                                      └-> IN_FLIGHT
IN_FLIGHT --超时--> RETRY_DUE -> SENDING -> IN_FLIGHT/EXHAUSTED
IN_FLIGHT/EXHAUSTED --匹配的正 DTA1--> FREE
STAGED/QUEUED/IN_FLIGHT/RETRY_DUE/EXHAUSTED --10 s--> EXPIRED/RECLAIMING -> FREE
```

`STAGED` 用于尚未到时间格的反射率候选，也用于尚未封包的 GPS 批次；两个待定指针分别维护，不得混淆。`IN_FLIGHT` 表示完整消息已经离开 ESP32 UART，但还没有收到匹配的正 ACK；发送任务不会因此停下来等待，会继续发其他记录。队列中的过期指针不能立即回收到 free queue，否则旧指针仍在 ready queue 时会产生 use-after-recycle，所以先把它标成 `ENTRY_EXPIRED` tombstone，等唯一消费者取出该指针后再释放。正在由 producer 复制或发布指针的 `FILLING/ENQUEUING`，以及正在 UART 发送的 `SENDING`，也分别由原所有者完成安全交接后回收。

池满时新遥测副本立即返回 `ESP_ERR_NO_MEM` 并增加对应 overflow 计数。它不会把 `healthy` 锁死，不会阻止后续提交，也不会影响同一记录已经进入的 SD 路径。`healthy=false` 只保留给 UART、本地序列化/分片或池所有权不变量等基础设施故障。

#### 7.1.6 DTF2 分片线格式

反射率的内层 payload 是与 SD 相同的完整 DHR1 v01 记录。GPS 的生产内层 payload 改为 DGB1：它用一次公共 DHR1 前缀和一次批次 CRC 无损表示 1..10 条 GPS，但 GPS_TRACK.BIN 仍逐条写原来的 98 字节 DHR1。外层 DTF2 提供流式恢复、分片元数据、完整消息 CRC 和逐分片 CRC。所有多字节整数均显式小端编码，不能直接发送 C 结构体内存。

每个 DTF2 分片为：

| 偏移 | 长度 | 字段 |
|---:|---:|---|
| 0 | 4 | magic `DTF2` |
| 4 | 1 | 版本 2 |
| 5 | 1 | 消息类型 |
| 6 | 2 | 固定头长 44 |
| 8 | 8 | `source_id` |
| 16 | 8 | `mission_id` |
| 24 | 4 | 消息 sequence |
| 28 | 4 | 完整消息长度 |
| 32 | 4 | 完整消息 CRC32 |
| 36 | 2 | `fragment_index`，从 0 开始 |
| 38 | 2 | `fragment_count` |
| 40 | 2 | 本分片 payload 长度 |
| 42 | 2 | flags，当前为 0 |
| 44 | 0..976 | 分片 payload |
| 末尾 | 4 | 本分片 CRC32 |

最大 payload 为 `1024 - 44 - 4 = 976` 字节。旧版单条 GPS 为 98 字节，形成 146 字节 DTF2 分片；DGB1 大小公式为 `44 + 70 × count`，满 10 条为 744 字节，连同 DTF2 后是 792 字节，仍然只有一个分片。DGB1 的 16 字节头包含 magic `DGB1`、版本 1、头长、总长、条数和被重建 DHR1 的固定长度；随后是一次 24 字节公共 DHR1 前缀、每条 70 字节后缀和 4 字节批次 CRC。地面端重建每条原始 94 字节 DHR1 内容并重新计算其 4 字节单条 CRC。711 点反射率记录为 2233 字节，形成三个分片，线长分别为 1024、1024、329 字节。MQTT publication 边界不具有协议意义：接收端必须先恢复 DTF2，再校验和展开 DGB1/DHR1。

#### 7.1.7 DTA1 应用确认和重试语义

DTU 上行 QoS 1 只证明 DTU 与 broker 之间的 MQTT 行为，ESP32 看不到 broker PUBACK，不能据此释放 PSRAM 条目。因此地面在完整 DTF2 和内层 DHR1 或 DGB1 都验证通过后，发布固定 40 字节 `DTA1`。一个 DGB1 只发一个 ACK；必须先成功重建并校验全部所含 GPS 记录：

- ACK 回显 `source_id`、`mission_id`、消息类型、sequence 和完整消息 CRC；
- status 0 表示永久接纳，B 板可释放条目；
- 非零 status 表示永久拒绝，同样释放条目并记失败；
- 临时存储压力、限流或暂时不可用时不得发负 ACK，应保持沉默，让 B 板按超时策略重试；
- ACK 可以延迟、乱序或重复；不匹配和过时 ACK 只计数，不得释放别的条目。

生产 DTA1 下行发布使用 QoS 0。它是幂等的小消息，丢失后 B 板会重发原始不可变记录；让地面 ACK publisher 使用 QoS 1 会同步等待另一个 broker PUBACK，实测只带来很小收益，却会拖慢接收热路径。地面必须缓存最近完成的消息身份：若收到应用重传，不得重复写结果，但应再次发送相同 DTA1，帮助 B 板清池。

首次完整发送后的 ACK deadline 为 3 秒；超时后允许一次完整消息重传，第二次名义等待为 6 秒。10 秒驻留从“进入池”而不是“第一次发完”开始，因此排队时间较长时，最终 ACK 窗口会被绝对新鲜度期限截短。这是有意的实时性取舍，不是离线可靠重放。若要求断网后补齐全部数据，应另建 SD-backed replay 协议，不能无限扩大 RAM 池。

#### 7.1.8 驻留期限、降级和状态计数

遥测任务使用 ESP32 本地单调时间记录 `admitted_us`。条目年龄达到 10000 ms 后：

- `STAGED` 候选从采样器解除并回收；
- `QUEUED` 指针先变 tombstone，再由队列消费者安全释放；
- `IN_FLIGHT/RETRY_DUE/EXHAUSTED` 直接解除保留并回收；
- 新鲜 ACK 总是在本轮过期扫描前处理，因此边界时刻已经到达 UART RX 的 ACK 优先；
- `SENDING` 不在半个分片序列中强行释放，发送函数返回后下一轮再处理。

关键 `telemetry_status_t` 字段应按下表理解：

| 字段 | 含义 | 是否表明数据退化 |
|---|---|---|
| `gps_submitted` | 成功追加到 staged/ready DGB1 的 GPS 源记录数 | 否 |
| `gps_batches_submitted` / `gps_partial_batches` | 进入可靠 ready 路径的 DGB1 数 / 未满 10 条的批次数 | 否 |
| `reflectance_submitted` | 成功进入可靠 ready 路径的反射率数 | 否 |
| `reflectance_offered` | 计算模块提供的有效结果 | 否 |
| `reflectance_rate_limited` | 在 250 ms 时间格内被更新候选替换 | 否，设计内降采样 |
| `gps_sent` / `reflectance_sent` | 收到正 DTA1 后清除的 GPS 源记录数 / 反射率数 | 否 |
| `gps_batches_sent` | 收到正 DTA1 后清除的完整 DGB1 数 | 否 |
| `*_queue_overflows` | 池或 ready 路径无容量，新遥测副本被丢弃 | 是 |
| `gps_expired` / `reflectance_expired` | 超过 10 秒驻留后丢弃的 GPS 源记录数 / 反射率数 | 是 |
| `gps_batches_expired` | 超过驻留期限的 DGB1 容器数 | 是 |
| `messages_failed` | 正常重试耗尽或收到永久负 ACK 的逻辑消息 | 是 |
| `messages_retried` / `acknowledgement_timeouts` | 传输和 ACK 延迟诊断 | 视结果判断 |
| `acknowledgements_mismatched` | 迟到、重复或属于其他任务/消息的 ACK | 诊断；大量出现需调查 |
| `pool_used` / `pool_high_watermark` | 当前/峰值占用 | 容量诊断 |
| `messages_in_flight` | 已发出但未确认的条目 | 延迟诊断 |
| `serialization_errors` / `reflectance_pool_errors` / `uart_errors` | 本地基础设施故障 | 是，并锁存 `healthy=false` |
| `messages_abandoned_shutdown` | 断电预告时主动放弃的池项 | 设计内收尾 |
| `gps_records_abandoned_shutdown` | 上述被放弃 DGB1 中包含的 GPS 源记录数 | 设计内收尾 |

`gps_expired`、`reflectance_expired`、overflow、`messages_failed` 或基础设施故障都会使 A/B 心跳报告错误码 5，但不会关闭 recorder 或拒绝未来遥测提交。`MISSION.JSON.telemetry.delivery_degraded` 汇总可恢复的交付损失和 drain timeout；基础设施故障另由 `infrastructure_healthy=false` 以及 serialization/pool/UART 计数表示。最终摘要还保存池峰值、ACK RTT、分片/字节数、重试、下行字节、断电放弃数和 `max_residency_ms`，用于把“采集问题、SD 问题、DTU 问题、地面 ACK 问题”分开诊断。

#### 7.1.9 断电优先级

收到 A 板 `0x30` 断电预告时，`mission_control_power_off()` 立即调用 `telemetry_abort_mission()`：关闭接纳、丢弃尚未封包的部分 GPS 批次、置终态 abort latch、清空 GPS/反射率 ready queue，并要求 worker 放弃 staged、queued、in-flight、retry 和 exhausted 项。这里故意不为了“凑满十条”或发送 partial batch 等待；正在写 UART 的分片之间会检查 abort，已经交给 UART 硬件的字节可能继续移出，但不会延迟 SD 收尾。

这个 abort 在本次上电内不可恢复，符合“一次上电一个飞行目录，`0x30` 后等待物理切电”的生命周期。`telemetry.shutdown_aborted=true` 不是故障；它表示系统按设计牺牲尚未完成的云副本，给 SD writer barrier、flush、文件关闭、最终摘要和卸载让出确定的时间预算。

#### 7.1.10 地面 validator 和起飞门禁

当前参考接收器是 `tests/dtu_uart_bridge/monitor_telemetry.py`。它建立独立的 uplink subscriber 和 ACK publisher，完成 SUBACK 与双连接 PING 后才打印 `TELEMETRY READY`。它同时接受旧版类型 1 单条 GPS 和新版类型 5 DGB1；DGB1 全包通过后展开成普通 GPS，摘要中的 `gps` 仍按源记录计数，另报告 `gps_batches` 和 `gps_partial_batches`。模拟飞行必须在看到 READY 且进程仍运行后才能启动；MQTTX 只适合人工观察，不能替代 CRC 校验、重组、去重和 DTA1。

十分钟模拟任务的通用命令模板如下；broker 和用户名应从本地部署配置取得，不要把密码写进命令历史或本文档：

```powershell
python -u -B tests/dtu_uart_bridge/monitor_telemetry.py `
  --host <broker-host> --mqtt-port 1883 --username <device-user> `
  --topic dji-h1/test/up --ack-topic dji-h1/test/down `
  --duration 720 --expect-qos 1 --ack-qos 0 `
  --expect-gps-min 2900 --expect-reflectance-min 300
```

上面的 `300` 是允许低照度长曝光的功能回归门槛，不是 4 Hz 吞吐资格门槛。要重新资格 4 Hz，必须先从心跳/SD 计数确认 H1 源速率持续高于 4 Hz，把 `--expect-reflectance-min` 提高到 1200，并同时核对板端已选数等于地面完整数、零 expiry/overflow、池峰值有界和最终 `inflight=0`、`buffered=0`。

10 分钟任务的典型顺序见 [emulator_test_sop.md](emulator_test_sop.md)：先跑 broker probe，再启动 720 秒 validator，最后启动 A 板 endurance 模拟器。validator 退出、`TELEMETRY INVALID`、未达到最低计数、仍有未完成重组或 B 板池持续增长，都应判为遥测资格测试失败。测试日志和任务报告必须使用新的前缀保存，不能只截取终端最后几行。

#### 7.1.11 已验证结论和当前瓶颈

2026-09-15 使用专用 ESP32-S3-WROOM-1U-N16R8 板、两台真实 H1、SD 卡、YY-M200、开发 EMQX broker 和 PC A 板模拟器，先对 DGB1 + 2 Hz 基线做两级资格测试。测试前均先通过 broker loopback，并在地面 validator 打印 `TELEMETRY READY` 后才复位/起飞。

| 指标 | 60 秒正常任务 | 10 分钟 endurance 任务 |
|---|---:|---:|
| 飞行 runner | PASS | PASS |
| 心跳 | 58，全部 error 0 | 585，全部 error 0 |
| A 模拟器导航帧 | 286 | 2924 |
| 地面恢复 GPS 源记录 | 275 | 2910 |
| DGB1 批次 | 30（26 满包、4 partial） | 295（287 满包、8 partial） |
| GPS 源序号缺口 | 0 | 0 |
| 地面完整反射率 | 34 | 644 |
| MQTT/DTU 分片 | 132 | 2227 |
| MQTT/DTU 字节 | 102828 | 1761628 |
| validator 收尾 | inflight 0、buffered 0 | inflight 0、buffered 0 |
| 安全断电 | PASS | PASS |

60 秒正常场景只有约 19 秒处于采集状态，因此 2 Hz 反射率最多只能选择约 38 条。第一次 validator 命令沿用 `--expect-reflectance-min 40`，虽然 B 板发出的 34 条全部到达并 ACK，进程仍因门槛不合理退出 1；这属于测试门槛假阴性，不是链路丢失。SOP 已把该场景的保守下限修正为 30，长测的 300 条门槛不变。

10 分钟任务的四段地面光谱/反射率计数为 388、336、336、336，共 1396；天空光谱共 2177，SD 原始光谱共 3573。四段 recorder 均报告 `dropped=0`、`rejected=0`、`write_errors=0`、`flush_errors=0`，writer queue 峰值为 2；最大一次 FAT flush 为 55.52 ms，没有阻塞采集。两台 H1 的 frame error、硬件 overrun、软件 drop 和 shutdown overrun 均为 0。

长测同时覆盖了每次控制命令首个 ACK 丢失、同 SEQ 重发、4.5 秒 A/B 链路中断与重新握手、状态同步、坏 CRC、截断帧、RTK 降级/恢复、四次采集启停和终态断电重发。所有预期故障均恢复，B 心跳未进入错误态，最终 `safe=1`。收到 `0x30` 后，最后尚未封包/确认的 GPS 尾项按设计放弃，SD 副本不受影响；地面最后完整确认的满批次覆盖 GPS 记录 2901..2910。

从长测 UART 诊断统计得到：GPS 批次从首条接纳到发送的平均 queue 时间约 1.799 s，这是 10 条/2 秒聚合等待的预期结果，不应误判为链路拥塞；GPS 批次 UART 发送平均约 18.3 ms，DTA1 RTT 平均约 195 ms、最大约 2.24 s。反射率 queue 平均约 205 ms、最大约 495 ms，UART 发送平均约 57.8 ms，DTA1 RTT 平均约 285 ms、最大约 2.71 s。发送日志观察到的池占用峰值仅 8，远低于 512 项容量；没有 telemetry warning/error、重试耗尽、过期或未完成地面重组。

与逐条发送 2910 个 146 字节 GPS DTF2 相比，本次 295 个 DGB1 将 GPS 消息和 ACK 数减少约 89.9%，GPS UART 字节数减少约 45.7%。若仍逐条发 GPS，同一任务约需 4842 个 MQTT/DTU 分片；实际为 2227 个，整体分片负担约下降 54%。这说明先前“大量短 GPS 消息挤占链路”的问题已被实质缓解。

在确认 DGB1 留出明显链路余量后，把反射率采样间隔从 500 ms 改为 250 ms。60 秒初测的 58 条已选反射率全部到达；第一次十分钟复测也完整送达 654 条，但后半程光照下降使 H1 源速率低于 2 Hz，不能作为 4 Hz 饱和资格。随后把设备移到高照度区域，源速率稳定在约 6.5–6.7 Hz，再完整飞行十分钟，得到决定性结果：

| 4 Hz 高照度长测指标 | 结果 |
|---|---:|
| 飞行 runner / 心跳 | PASS / 585 条全部 error 0 |
| A 模拟器导航帧 | 2924 |
| 四段地面反射率 | 587 + 521 + 521 + 522 = 2151 |
| SD 原始光谱 | 4328 |
| 4 Hz 已选反射率 | 1277（324 s 有效采集内约 3.94 Hz） |
| 设计内 `reflectance_rate_limited` | 874 |
| 地面完整反射率 | 1277/1277 |
| 地面 GPS | 2910/2910，295 个 DGB1，零序号缺口 |
| MQTT/DTU 分片与字节 | 4126 / 3266269 |
| 遥测池峰值 | 7/512 |
| 地面收尾 | inflight 0、buffered 0 |
| 安全断电 | PASS |

高照度长测的反射率 queue 平均约 171 ms、p95 约 248 ms、最大约 808 ms；反射率 DTA1 RTT 平均约 286 ms、p95 约 404 ms、最大约 779 ms。GPS DTA1 RTT 平均约 184 ms、p95 约 325 ms、最大约 650 ms。全程没有 telemetry warning/error、ACK timeout、重试、过期、overflow 或未完成地面重组。四段 recorder 都是 `dropped=0`、`rejected=0`、`write_errors=0`、`flush_errors=0`，证明 4 Hz 遥测没有反向干扰全速 SD 记录。

这些结果取代了“2 Hz 是当前保守生产上限”的旧结论，但不抹去历史压力测试：2.5 Hz 反射率配合逐条 5 Hz GPS、QoS 1 DTA1 或 fire-and-forget 曾在 YY-M200 上出现明显拥塞和整条多分片消息丢失；原始比较见 [dtu_ack_qos_stress_2026-09-14.md](dtu_ack_qos_stress_2026-09-14.md)。当前可靠基线是 460800 baud、DGB1 10 条/2 秒、反射率 4 Hz、上行 QoS 1、DTA1 QoS 0 和 10 秒驻留。下一阶段仍须在真实 A 板和实际作业区域蜂窝网络中复测，不能把受控开发网络的全送达直接外推到所有覆盖、时延和丢包条件。

#### 7.1.12 代码地图和修改联动

| 文件/目录 | 遥测职责 | 修改时必须联动检查 |
|---|---|---|
| `components/board_support/include/dji_h1_board.h` | UART、速率、ACK、池、驻留、GPS 批次和 4 Hz 生产常量 | DTU 持久配置、SOP、压力测试参数 |
| `main/main.c` | 生成 `source_id`，显式选择 application-ACK 并启动组件 | 不得让诊断 delivery mode 成为生产默认 |
| `components/telemetry/include/telemetry.h` | 对 recorder/mission control 的 API 和统计契约 | `MISSION.JSON` 字段、心跳降级条件、注释 |
| `components/telemetry/telemetry.c` | 固定池、调度、UART owner、ACK、重试、过期和 abort | 所有权测试、栈/PSRAM、竞态、shutdown deadline |
| `components/gps_batch/` | DGB1 v01 组装、序列化、校验和 DHR1 重建 | Python decoder、类型 5、格式文档、边界/CRC 测试 |
| `components/telemetry_transport/` | DTF2/DTA1 纯线格式和 CRC | C 自检、Python decoder、格式文档、测试向量 |
| `components/measurement_recorder/measurement_recorder.c` | GPS/反射率分叉、随机 mission ID、最终摘要 | SD 不能受遥测返回值阻塞；摘要字段需保持可解释 |
| `main/mission_control.c` | error 5、`0x30` 时同步 abort | A/B 协议、心跳和安全断电回归 |
| `tools/mission_viewer/dji_h1_viewer/telemetry.py` | Python 分片解码、重组和 ACK 编码 | 与 C 头字段、大小、CRC 和版本完全一致 |
| `tests/dtu_uart_bridge/monitor_telemetry.py` | 当前地面参考接收器/ACK publisher | 去重、重 ACK、keepalive、broker QoS、长任务门禁 |
| `tests/dtu_telemetry_stress/` | 不依赖 H1/SD/A 板的板级传输压力镜像 | 与生产构建隔离；每次测试使用唯一 mission ID |

任何 DTF2/DTA1 字段、DGB1 字段、最大分片、消息身份或 CRC 变化，都应视为协议版本变化：先更新 C 编解码和自检，再更新 Python、文档和双端测试，最后才允许更改 DTU/地面部署。只改一端会表现为大量 `acknowledgements_mismatched`、重试和 10 秒过期，而不是显式编译错误。

#### 7.1.13 诊断固件隔离和待办

`tests/dtu_telemetry_stress` 是独立 ESP-IDF project，会复用生产 serializer、DTF2、pool 和 UART owner，但不会被根目录生产 `CMakeLists.txt` 编进 `DJI_H1.bin`。`DTU_STRESS_FIRE_AND_FORGET`、无上限反射率输入和固定假数据只在该测试 project 的 `main` 中选择；生产 `main/main.c` 明确写死 `TELEMETRY_DELIVERY_APPLICATION_ACK`、250 ms sampler 和 10 秒驻留。刷写 stress image 会替换生产应用，测试后必须重新刷 `build-review`，并从启动横幅确认不是 `DTU_STRESS`。

最近一次审查留下三项非阻塞待办，后续修改时应优先处理：

1. 地面 validator 对已完成消息的缓存 ACK 目前只在重复消息的 `fragment_index==0` 时重发；应改为对任意首个已验证重复分片按消息限频重发，避免重传的 0 号分片恰好丢失时 B 板无谓过期；
2. 512 项池的 expiry、ACK deadline 和 retry 扫描目前在 20 ms worker 循环中多次全表遍历；可将 10 秒 expiry 降到 100–250 ms 维护周期或合并扫描，减少共享锁竞争；
3. `residency_policy_self_test()` 只覆盖时间比较，尚缺 STAGED、QUEUED tombstone、IN_FLIGHT、RETRY_DUE、EXHAUSTED 和 abort 的确定性所有权测试。

## 8. A/B 板通信协议摘要

完整定义以 [AB板串口通信协议_V1.0.md](AB板串口通信协议_V1.0.md) 为准。

### 8.1 帧格式

```text
AA 55 | LEN | CMD | SEQ | PAYLOAD(0..247) | CRC16 low | CRC16 high
```

- 多字节字段：小端；
- CRC：CRC16-CCITT-FALSE，多项式 `0x1021`，初值 `0xFFFF`；
- CRC 覆盖 `LEN + CMD + SEQ + PAYLOAD`；
- 字节间超时：100 ms；
- 解析器支持垃圾字节重同步、截断帧超时恢复、CRC 拒绝和连续帧。

### 8.2 主要命令

| CMD | 方向 | 用途 |
|---:|---|---|
| `0x20` | A → B | 握手；建链前 A 每秒发送 |
| `0xA0` | B → A | 握手响应和版本协商 |
| `0x01` | A → B | 30 字节实时定位/UTC 数据，通常 5 Hz |
| `0x10` | A → B | 开始采集 |
| `0x11` | A → B | 停止采集 |
| `0x30` | A → B | 准备断电/结束飞行 |
| `0x81` | B → A | B 状态心跳，1 Hz |
| `0x90` | B → A | ACK |

### 8.3 ACK、重传和幂等

A 板等待动作 ACK 的超时是 200 ms。重传必须沿用原始 CMD、SEQ 和 payload。B 板保存最近动作命令，在 1 s 重试窗口内遇到相同命令会重新发送 ACK，但不会再次执行动作。

采集 session 还有额外幂等规则：

- 同一 session 的重复 START 不会清零计数或重启任务；
- STOP 后再次采集必须使用新的 session ID；
- 已完成 session 使用长度 64 的启动期内历史环缓存抑制重放；
- B 板重启后该历史丢失，因此跨重启的全局幂等需要 A 板配合或未来增加非易失记录；
- `0x30` 完成后固件进入终态，必须复位才能开始下一次飞行。

ACK 仅表示命令已经被接纳，不代表物理动作已经完成。A 板必须以随后心跳中的真实状态为准。

协议允许 A 板早期握手携带空序列号；B 板会等待并锁定本次任务中第一条非空序列号。此后不同的非空序列号不会覆盖任务身份，而会产生 `drone_identity_mismatch` 事件和心跳错误码 5。`MISSION.JSON` 中的 `drone_serial_hex` 是规范身份，可读但经过字符替换的 `drone_serial` 只用于界面展示。

### 8.4 心跳语义

B 板每秒发送一次状态，主要包含：

- B 板状态：初始化、就绪或故障；
- 实际采集状态；
- B 板错误码；
- SD 剩余空间百分比；
- `frame_count`：当前 session 成功取得的地面 H1-A 帧数，不是 A/B 两路之和；
- 正在采集时的 session ID，否则为 0；
- 只有记录器完成排空、文件关闭且 SD 已卸载后，才报告可安全断电。

当前错误码约定：

| 错误码 | 含义 |
|---:|---|
| 0 | 正常 |
| 1 | SD/存储初始化或容量问题 |
| 2 | SC16/H1 初始化问题 |
| 3 | 采集或生命周期错误 |
| 4 | 停止/断电收尾错误 |
| 5 | 数据质量/交付退化，例如采集错误、记录器丢弃或拒绝、A 板身份冲突、遥测 overflow/过期/失败 |

心跳发送位于高优先级 `ab_link` 任务内，而不是单独任务。其 UART 读取超时为 10 ms，并避免在延迟后连发“补课式”心跳。维护时不要在协议解析循环内增加大量同步日志或阻塞操作。

## 9. 飞行任务状态与生命周期

一个 SD 任务目录代表一次 B 板从上电到收到安全断电命令的完整飞行生命周期，不等同于一次 START 的 session。

### 9.1 上电准备

1. 挂载 SD 卡并读取容量；
2. 初始化记录器；记录器先运行 flight-index codec 和地面/天空因果配对策略的轻量自检；
3. 从带 CRC 的根目录 `FLIGHT.IDX` 读取下一编号并确认目录未占用；缓存缺失或损坏时安全扫描 `F_0001` 至 `F_9999`；
4. 打开任务记录文件、创建初始摘要、开始遥测 mission 并启动 writer；
5. 初始化 SC16；
6. 等待 500 ms；
7. 查询并准备 H1-A、H1-B；
8. 进入 READY，接受 A 板握手和控制。

成功创建任务后通过 `FLIGHT.TMP/FLIGHT.BAK` 更新索引。缓存只是加速提示，永远不能绕过目录存在性检查。目录编号当前不会循环复用；达到 `F_9999` 后需要人工归档/清卡或扩展命名策略。

### 9.2 START

- `mission_control` 接纳新 session；
- 记录 segment 开始；
- 先启动天空 H1-B；
- 必须在 5 s 内取得一帧天空光谱作为 priming；
- 然后启动地面 H1-A；
- 两路持续采集，地面帧到达时计算反射率；
- 心跳反映真实采集状态和地面帧计数。

### 9.3 STOP 与重启

- STOP 即使在准备阶段到达也会被锁存；
- 停止两个采集任务；
- 按安全顺序停止 H1 流和 SC16 RX 服务；
- writer barrier 排空已接纳的数据并刷盘；
- SD 保持挂载，飞行中的 GPS 和事件仍可继续记录；
- 后续可用新的 session ID 再次 START，segment 编号递增。

### 9.4 飞行结束和断电

收到 `0x30` 后：

1. 立即关闭遥测接纳，清空尚未发送的遥测副本，并取消正在进行的分片/ACK/重试；
2. 若仍在采集则先停止；
3. 记录 flight closed 事件；
4. 排空 SD writer 队列、刷盘并关闭全部文件；
5. 完成 `MISSION.JSON` 最终检查点，其中 `telemetry.shutdown_aborted=true` 表示按设计放弃遥测；
6. 若 UTC 有效，尽力修正任务文件和目录的 FAT 修改时间；
7. 卸载 SD；
8. 心跳置安全断电标志；
9. 进入终态，等待 A 板切电或人工复位。

A 板负责实际断电。B 板把 `grace_sec` 转换为绝对单调时钟 deadline，
限制采集任务和 writer barrier 的等待时间，并为最终 FAT 关闭操作预留时间；
重复的 `0x30` 不得延长最初 deadline。已经开始的 FAT flush/close 不可强制中断，
只有所有文件关闭且 SD 成功卸载后才报告 `safe_power_off=1`。

正常飞行期间，遥测池不是永久重放队列。每条 GPS 或反射率记录从进入池开始最多
保留 10 秒；超过期限仍未收到云端应用 ACK 的记录会被丢弃并分别计入
`gps_expired` 或 `reflectance_expired`。这样在 4G 链路拥塞时优先恢复实时数据，
避免旧数据淹没池。过期会通过心跳错误码 5 和 `MISSION.JSON` 的
`delivery_degraded` 暴露，但不会停止采集或 SD 卡完整记录。

## 10. 双光谱采集与反射率计算

### 10.1 原始光谱

H1 目前使用自动曝光。典型设备提供 711 个采样点，约覆盖 340–1050 nm；代码的最大容量为 1024 点，因此不要把“711”硬编码到新模块。

每帧保存：

- 传感器通道；
- session 与 segment；
- 帧序号；
- 曝光时间和量程/尺度信息；
- 采样点数与原始 uint16 数据；
- 饱和、欠曝、恢复后首帧等质量标志；
- 统一时间戳。

当前帧时间戳取在完整帧接收完成时，而不是曝光开始或曝光中点。后续若做高精度空间配准，需要评估 H1 曝光和传输延迟，并考虑增加可校准的时间偏置。

### 10.2 天空帧匹配

每个地面帧只使用“时间上不晚于该地面帧”的最新天空帧，避免使用未来数据。当前天空历史深度为 8 帧，并要求：

- 地面、天空采样点数一致；
- 天空帧年龄不超过 500,000 μs；
- 天空原始计数至少为 4，否则该采样点无效。

### 10.3 当前计算公式

```text
reflectance_percent =
    100 × ground_raw / sky_raw
        × sky_exposure_us / ground_exposure_us
        × 10^(sky_scale - ground_scale)
```

计算结果限制在 0–100%，以 `uint16` 保存，单位为 0.01%，即 0–10000。每个采样点同时保存有效、低端截断、高端截断、天空过小、地面过曝、天空过曝等标志。反射率记录时间戳继承地面帧时间戳。

这里得到的是工程上的“表观反射率”，不是经过完整辐射定标的科学反射率。当前尚未加入：

- 暗电流/暗场校正；
- 两台 H1 的辐射响应一致性校正；
- 精确波长标定和重采样；
- 入射几何、姿态和大气校正；
- 标准白板或参考源标定。

任何科学解释前都应在可控光源、标准反射板和实际日照条件下建立标定流程。

## 11. 时间戳架构

时间设计的原则是：记录排序永远依赖 B 板单调时钟，UTC 用于跨设备和现实时间定位，不能用可能跳变的系统墙钟控制采集顺序。

### 11.1 统一时间戳

内存中的 `record_time_t` 为 32 字节，包含：

| 字段 | 长度 | 含义 |
|---|---:|---|
| `b_monotonic_us` | 8 | B 板启动后的单调微秒，记录排序主键 |
| `a_monotonic_ms` | 8 | 扩展后的 A 板单调毫秒 |
| `utc_ms` | 8 | Unix UTC 毫秒 |
| `sync_age_ms` | 4 | 最近时间观测年龄 |
| `sync_generation` | 2 | 时间模型代次 |
| `sync_state` | 1 | 未同步/捕获/锁定/保持/失效 |
| `flags` | 1 | 字段有效性标志 |

A 板发送 `utc_sec`（uint32）和 `utc_msec`（uint16，0–999），B 板组合为：

```text
utc_ms = utc_sec × 1000 + utc_msec
```

### 11.2 同步模型

- 观测队列深度：16；
- 拟合窗口：最近 32 个样本；
- 至少 5 个样本后进入 LOCKED；
- 超过 1.5 s 没有新观测进入 HOLDOVER；
- 超过 5 s 进入 INVALID；
- 拟合残差超过 50 ms 时重建模型；
- UTC 跳变超过 1 s 时重建模型并增加 generation；
- UTC 偏移使用中位数抑制异常值。

任务目录创建时通常还没有 UTC。`MISSION.JSON` 会在第一次得到有效投影时冻结任务起始 UTC，并保存 `started_sync_generation`；以后同步模型即使重建，也不会用新 generation 静默改写起始时间。更新时间则同时保存自己的 `updated_sync_generation`，允许两者不同。

每个实时数据包到达 B 板 UART 时尝试提交时间观测，但协议中的 `mono_ms` 是经纬度采样时刻而不是报文发送时刻。相同 `mono_ms` 的重复定位报文仍照常写入 GPS 文件并进入遥测，时间拟合器则忽略它们，且不刷新同步年龄；这避免合法的旧定位复用被误判成时钟跳变。不同定位样本从采样到发送仍可能有可变延迟，因此现有 A/B 映射属于受延迟限制的估计；若后续要求精密同步，协议必须增加明确的报文发送时间戳或独立授时交互。系统只在每个锁定 generation 中设置一次 POSIX 墙钟，避免反复调整系统时间。

协议没有 PPS，因此绝对 UTC 精度预计只能达到约 ±100–200 ms，不能宣称亚毫秒同步。科学数据中的 `utc_ms` 始终是 Unix UTC 毫秒。任务本地时区由 `CONFIG_DJI_H1_TIMEZONE_NAME` 与 `CONFIG_DJI_H1_TIMEZONE_OFFSET_MINUTES` 配置，默认 `Asia/Shanghai`、UTC+08:00；`MISSION.JSON` 保存该配置，`EVENTS.JSONL` 每条事件也保存名称和分钟偏移。FAT 时间戳只有 2 s 分辨率且不携带时区，因此固件按该固定偏移写入本地日历字段，使同一时区的 Windows 正确显示修改时间。

## 12. SD 卡任务目录与文件

每次 B 板启动创建一个任务目录：

```text
/sdcard/F_0001/
├── RAW_SPECTRA.BIN
├── REFLECTANCE.BIN
├── GPS_TRACK.BIN
├── EVENTS.JSONL
└── MISSION.JSON
```

卡根目录还保存 `FLIGHT.IDX`（以及原子替换期间可能出现的 `FLIGHT.TMP/FLIGHT.BAK`），用于 O(1) 定位下一任务编号。它不是任务数据文件，丢失或 CRC 损坏只会触发完整目录扫描。

运行中还可能短暂出现：

- `MISSION.TMP`：正在写入的新检查点；
- `MISSION.BAK`：替换现有摘要前保留的上一份检查点。

`MISSION.JSON` 在握手、segment 边界和飞行结束时更新，用于记录任务身份、文件名、计数、状态、错误以及开始/更新时间。`EVENTS.JSONL` 每行一个 JSON 事件，便于追加和断电后的局部恢复。

### 12.1 二进制格式 v01

格式常量集中在 `components/data_records/include/measurement_records.h`。当前版本号为 `0x01`，全部多字节字段为小端。

- 文件头：16 字节，magic 为 `DHF1`；
- 记录头线格式：60 字节，magic 为 `DHR1`；
- 每条记录末尾带 CRC32 IEEE；
- RAW 文件中 A/B 两路各自形成独立记录；
- REFLECTANCE 文件包含地面/天空帧关联和逐点结果；
- GPS 文件包含 A 板 30 字节实时数据语义、协议 SEQ 和统一时间戳。

不要把 C 结构体直接 `fwrite()` 到卡中。结构体可能受对齐、编译器和平台 ABI 影响；当前 writer 显式按线格式序列化，这是格式稳定性的关键。

### 12.2 写入可靠性和背压

- 固定原始记录内存池：24；
- writer 消息队列：64；
- 序列化缓冲区：4096 字节；
- 周期刷盘：1500 ms；
- segment 停止和飞行结束时强制 barrier/flush；
- 临时长时间 SD 阻塞会耗尽内存池并产生可计数的丢帧，但不会阻塞 H1 采集任务；
- 持续写入或刷盘错误被视为致命存储故障；
- 数据丢弃、反射率拒绝等会通过事件、统计和心跳错误码 5 暴露。

突然掉电仍可能损坏或丢失文件尾部，CRC 使桌面解码器可以定位并隔离损坏记录，但不能恢复从未落盘的数据。真正外场使用仍应保证 A 板遵循 `0x30` 安全断电流程。

## 13. 地理位置与光谱测点插值

A 板实时数据通常为 5 Hz，而光谱帧与定位包不同步。桌面端为每个光谱结果寻找时间戳前后两条 GPS 点，假设两点之间速度恒定并做线性插值。

插值优先级：

1. 同一时间同步 generation 内使用 A 板单调时间；
2. 条件不满足时退回 B 板单调时间；
3. 不做区间外推；
4. 前后定位间隔大于 500 ms 时标记为宽间隔，提示定位可信度下降；
5. API 可进一步设置最大允许间隔。

这只是空间配准的第一版模型。急转弯、加减速、悬停抖动和 GPS 异常时，恒速直线假设可能产生误差；后续可加入姿态、速度矢量或轨迹滤波。

## 14. 桌面解码与可视化工具

工具位于 `tools/mission_viewer`，提供 Python API、回环 HTTP API 和 PyQt GUI。

### 14.1 安装与运行

```powershell
python -m pip install -e tools/mission_viewer
python tools/mission_viewer/run_viewer.py D:\F_0003
```

GUI 可以：

- 打开一个任务目录；
- 显示任务摘要和数据健康状态；
- 解析 GPS、原始光谱、反射率和事件；
- 在航迹图中显示有测量点、无测量轨迹点和关键事件；
- 鼠标滚轮缩放；
- 点击测点后显示对应反射率曲线；
- 使用离线米制平面图，或选择百度卫星图。

当前没有波长标定表，光谱横轴仍是采样点序号，不应误标为精确 nm。

### 14.2 Python API 和 HTTP API

Python 代码可以直接使用包中公开的 `open_mission()` 和 `MissionService`。GUI 默认还可在 `127.0.0.1:8765/api/v1` 暴露只监听本机的 HTTP API；使用 `--no-api` 可关闭。

解码器首次扫描时验证记录 CRC；用户选择某条光谱时会再次验证对应记录，避免文件在扫描后被替换或损坏。若掉电只损坏文件尾部，默认查看模式保留并展示损坏点以前通过 CRC 的记录，同时报告偏移量、恢复条数和丢弃尾部字节数；严格批处理仍可选择拒绝整个损坏产品。`MISSION.JSON` 缺失或损坏时会尝试读取上一份原子检查点 `MISSION.BAK`。位置和相对高度分别遵守协议中的独立有效位，横向位置有效但高度无效时返回空高度而不是伪造插值值。

### 14.3 百度地图凭据和隐私

真实 AK 必须只放在：

```text
tools/mission_viewer/credentials/baidu_map.local.json
```

该文件已被 `.gitignore` 排除。仓库只跟踪 `baidu_map.example.json`，交接文档和提交记录中都不应出现真实 AK。

百度地图模式会把航迹坐标提供给远程网页/API，因此 GUI 在加载前要求用户确认。Qt WebEngine 的本地文件访问权限已收紧。测点详情明确显示定位使用 A 单调时钟及其同步代次，或 B 单调时钟回退。当前尚未实现 WGS84 到 BD-09 坐标转换，在中国境内叠加百度底图时预计会出现系统性位置偏移；在正式展示或测绘使用前必须补上并验证转换。

## 15. 构建、下载和日志

### 15.1 初始化 ESP-IDF 环境

当前开发机使用 ESP-IDF v5.5.5。PowerShell 示例：

```powershell
. 'C:/Espressif/tools/Microsoft.v5.5.5.PowerShell_profile.ps1'
idf.py -B build-review build
```

### 15.2 下载到当前专用板

确认 ESP32 当前枚举端口后，例如：

```powershell
. 'C:/Espressif/tools/Microsoft.v5.5.5.PowerShell_profile.ps1'
idf.py -B build-review -p COM6 flash
```

若芯片进入不了下载，应检查 BOOT/EN、USB 数据线、电源、电平和串口占用。`invalid header: 0xffffffff` 通常说明目标闪存中没有有效应用、flash 连接/供电异常或启动模式错误，本身不是业务固件日志。

### 15.3 监视日志

```powershell
. 'C:/Espressif/tools/Microsoft.v5.5.5.PowerShell_profile.ps1'
idf.py -B build-review -p COM6 monitor
```

串口是独占资源。如果下载、monitor 或模拟器提示端口占用，应先关闭其他终端、串口助手、旧 Python 进程和 VS Code 监视任务。终止进程前要确认 PID/命令行，避免误杀无关程序。

## 16. 测试方法

### 16.1 主机单元测试

```powershell
python -B tests/run_host_tests.py
python -m pip check
```

统一入口会发现每个 `tests/*/test_*.py` 测试组，并在任一测试组发现数为 0 时失败。DGB1 基线共通过 81 个 Python 测试：mission viewer 18（含无 Chromium/不触网的离屏 GUI 冒烟测试）、A 板模拟器 29、记录格式 6、telemetry transport 18、DTU 工具 10。测试数量会随代码演进变化，应以统一入口输出为准。

### 16.2 A 板模拟器硬件飞行

当前常用命令：

```powershell
python -B tests/ab_board_emulator/run_hardware_flight.py `
  --port COM5 `
  --debug-port COM6 `
  --reset `
  --report-prefix build-review/mission-YYYYMMDD-normal
```

可按测试目的增加：

- `--faults`：协议和飞行异常注入；
- `--probe`：控制/协议探针；
- `--endurance`：约 10 分钟完整飞行。

部分脚本中的默认 debug port 仍可能是旧硬件时期的 COM4，因此在当前专用板上应显式传 `--debug-port COM6`。每次使用新的 `--report-prefix`，脚本不会覆盖已有报告。

推荐完整回归顺序：

1. 卡已插入且为 FAT32；
2. 两台 H1 均已连接并供电；
3. COM5 接 A 板模拟器，COM6 接 ESP32；
4. 构建并下载；
5. 启动 30 s 正常任务，检查握手、GPS 5 Hz、START/STOP、心跳和安全断电；
6. 启动 faults 任务，确认坏 CRC、截断帧、丢 ACK、通信中断不会重复执行动作；
7. 启动 endurance 任务，检查多航线、多次采集、4.5 s 链路黑障、RTK 降级和恢复；
8. 安全断电标志出现后复位，开始下一任务；
9. 取卡查看新增 `F_xxxx`，用 mission viewer 打开；
10. 核对 `FLIGHT.IDX` 指向下一编号，再复位一次确认不会从 `F_0001` 线性扫描；
11. 核对任务摘要计数、航迹、事件、光谱选择、CRC 和文件修改时间。

### 16.3 SD 卡故障注入

`tests/sdkconfig.stalled_sd` 可启用一次性 flush stall，用于模拟 SD 长延迟。预期行为：采集不中断、内存池耗尽后发生有计数的记录丢弃、心跳显示错误 5、SD 恢复后 writer 继续工作并最终安全收尾。

故障注入配置绝不能误用于正式飞行固件。测试结束后要确认构建目录采用正常 `sdkconfig.defaults`。

## 17. 已验证的里程碑

以下结论来自开发过程中的实际板级测试：

- 两台 H1 在 SC16 两通道上可同时高速流式采集；
- 明亮光源导致短曝光/高帧率时，早期 RX overrun 与错误停止流程有关，调整停止顺序和排空机制后稳定；
- 单字节丢失不会再无限破坏后续帧，H1 解析支持边界重同步和连续错误退出；
- A 板模拟器已验证握手、版本拒绝、实时数据、动作 ACK、相同 SEQ 重传幂等和 1 Hz 心跳；
- A 板可以真实控制开始、停止、使用新 session 重启和结束飞行；
- 专用 ESP32-S3-WROOM-1U-N16R8 板已经成功运行双光谱仪、SD 卡和 PSRAM 遥测池；
- 10 分钟模拟巡测在专用板上完成：4 条航线、5 Hz 假定位、多次动作、丢 ACK、坏 CRC、截断帧、4.5 s 断链/重连和 RTK 降级均可恢复并安全结束；
- SD 8 s 刷盘停顿故障注入产生了有界丢弃和错误上报，恢复后可继续并安全结束；
- 4G 遥测已验证 DTF2 分片/重组、DHR1 双层 CRC、DGB1 十条/2 秒 GPS 合包、4 Hz latest-value 反射率、乱序/延迟/重复 DTA1、10 秒过期、池满降级和断电立即放弃；
- 2026-09-15 的 60 秒板级联调在地面恢复 275 条 GPS 和 34 条反射率，GPS 零序号缺口，全部 34 条板端已发送反射率均被接收和确认；
- 同日 10 分钟真实 YY-M200/EMQX 全功能任务恢复 2910 条 GPS（295 个 DGB1）和 644 条反射率，GPS 零序号缺口、零未完成重组、遥测池最终清空，并在多段采集和故障注入后安全结束；板端 SD 同时保留 3573 条原始光谱和 1396 条全速反射率，记录错误和丢弃均为零；
- 把设备移至高照度区域后，4 Hz 十分钟饱和资格任务在约 6.5–6.7 Hz 光谱源下完整接收全部 1277 条已选反射率和 2910 条 GPS；反射率 ACK p95 约 404 ms、最大约 779 ms，池峰值 7/512，零超时、重试、过期、溢出或未完成重组；
- mission viewer 已由用户在真实任务目录上运行，百度卫星图、航迹和光谱联动达到预期。

需要区分：本文基线的 460800 baud、DGB1 十条/2 秒、反射率 4 Hz、10 秒驻留组合已经通过高照度真实 DTU/EMQX 十分钟饱和资格测试，但仍应在真实 A 板和实际作业区域蜂窝网络中重新做长航次资格测试。当前结果证明该组合在本次受控网络条件下可以完整传送所有进入遥测发送路径的数据，同时保持采集和 SD 记录独立；它不是对所有公网覆盖、时延和丢包条件的无条件保证。反射率源记录中未被 4 Hz latest-value 策略选中的帧属于设计内降采样，不是遥测丢包，更不是 SD 数据丢失。

## 18. 已知限制和风险

### 18.1 固件与硬件

- 当前 16 MB 分区表已预留 factory、`ota_0`、`ota_1`、otadata、coredump 和 storage，但 OTA 下载、镜像验证、切换、回滚和安全升级业务尚未实现；
- 8 MB Octal PSRAM 已启用并在启动时验证；遥测池依赖 PSRAM，PSRAM 容量或初始化失败会阻止系统进入任务；
- `sdkconfig` 已不跟踪，量产默认值由 `sdkconfig.defaults` 和 `partitions.csv` 管理；旧工作区仍可能保留本地 `sdkconfig`，构建异常时应清理或核对；
- 专用板原理图、BOM、版本号、关键电源时序和测试点说明尚未入库；
- H1 时间戳为接收完成时间，并非曝光中心；
- 真实 A 板尚未参与完整联调，PC 模拟器只是协议参考实现；
- `F_0001..F_9999` 目录耗尽后没有自动处理策略；根目录索引可避免日常线性扫描，但不能扩展编号空间；
- session 去重历史只存在 RAM 中，复位后丢失；
- 当前稳定设备身份使用芯片出厂 eFuse MAC 生成遥测 `source_id`；独立的产品序列号、烧录工装、权限控制和追溯数据库尚未实现，不能把目录编号或 A 板序列号当作 B 板产品序列号；
- A/B CRC 错误和字节间超时会写入 `EVENTS.JSONL`，但“成功记录了一次协议错误”本身目前不会直接把心跳置为错误码 5；若要求 A 板实时获知链路错误率，需要增加有界计数和明确的降级阈值。

### 18.2 数据和算法

- 表观反射率未做辐射、暗场、波长、姿态和大气标定；
- 地理插值假定相邻 GPS 点之间匀速直线运动；
- 无 PPS，UTC 精度受 A 板报文周期和串口延迟限制；
- 当前查看器横轴是光谱采样点编号；
- WGS84→BD-09 未实现，百度底图可能有偏移；
- 断电仍可能损失最后一个刷盘周期的数据。

### 18.3 查看器中仍待处理的次要问题

- 同一坐标上的多个测量点目前只能按最近点选中一个；
- 百度地图上的事件标记未像离线图一样明显区分 warning/critical。

这些问题目前不阻塞数据采集，但应在对外发布查看器前处理。

## 19. 后续开发建议

建议按以下顺序推进：

1. **首轮室外资格飞行**：在真实阳光和实际作业区域蜂窝网络下复现十分钟以上任务，核对光谱动态范围、SD 完整性、DGB1 序号、反射率送达率、池峰值和安全收尾；
2. **真实 A 板联调**：逐项核对电平、握手版本、5 Hz 数据、动作重试、心跳、grace deadline 和断电；
3. **光谱标定**：建立暗场、白板、两传感器响应和波长映射流程，明确“表观反射率”升级为科学产品的条件；
4. **4G DTU/MQTT**：受控 DGB1 及 4 Hz 饱和真实 DTU 资格测试已经完成；下一步先把相同 validator 与计数口径用于室外/真实 A 板任务，再评估地面 cached-ACK 边界、池维护扫描、Air780/更大单包或选择性 ACK，最后补充 SD 回放、TLS、正式设备身份和常驻地面接收服务；
5. **数据格式兼容策略**：保留 v01 解码器，新格式只能增加新版本，不能静默改变已有字段含义；
6. **部署可靠性**：规划 OTA 双分区、回滚、固件签名、看门狗复位记录和掉电保护；
7. **查看器完善**：备份摘要恢复、事件分类、重叠点选择、BD-09 转换和真实波长轴；
8. **工程资料归档**：将允许公开/提交的专用板原理图、BOM、接线图、4G DTU 手册和实测报告纳入 `Docs`。

当前 MQTT 数据流：

```text
calculation ──> SD recorder queue ──> SD（权威本地副本）
       │
       └──────> PSRAM telemetry pool ──> UART task ──> 4G DTU
```

网络任务不得持有采集缓冲区，不得直接调用 H1，不得阻塞 writer，也不得把“已发到 MQTT”当作“已可靠记录”的替代。实现细节、接收端契约和待办见第 7.1 节；收到断电预告时始终以 SD 刷写、关闭和卸载优先。

## 20. 代码修改守则

- A/B 30 字节实时 payload 只能由 `ab_protocol` 解码，禁止在其他模块复制字段偏移；
- 协议变化必须同时更新协议文档、固件 self-test、模拟器和模拟器测试；
- 线格式变化必须增加格式版本并更新 C/Python 两端测试向量；
- 不要在 FreeRTOS 队列中按值复制约 2–4 KB 的整帧结构，使用固定池指针或在生产者侧序列化；
- 不要从多个任务直接访问 FatFs；
- 不要让 `ab_link`、采集任务或 SC16 RX 服务执行慢文件/网络操作；
- 停止顺序、barrier 和 safe-power-off 是数据完整性逻辑，修改时必须做异常测试；
- 不要提交真实地图 AK、MQTT 密钥、SIM/DTU 凭据或生产设备证书；
- 不要提交临时构建目录、串口日志和真实飞行数据，除非经过脱敏并明确作为测试夹具；
- 每个里程碑先跑主机测试、再构建、再做硬件任务，最后才提交和推送。

## 21. 交接验收清单

新同事可以独立完成以下事项时，可认为基本接手成功：

- [ ] 能说明 H1-A/H1-B 分别代表地面和天空；
- [ ] 能找到并修改 A/B UART 以及 SC16/SD 引脚；
- [ ] 能构建并向专用板下载固件；
- [ ] 能用 COM5 模拟 A 板并在 COM6 查看 B 板日志；
- [ ] 能解释握手、实时数据、START、STOP、POWER-OFF、ACK 和心跳；
- [ ] 能说明相同 CMD/SEQ 重试为何不能重复执行动作；
- [ ] 能完成一次多 segment 模拟飞行并看到安全断电；
- [ ] 能从 SD 卡找到五类任务文件并解释其内容；
- [ ] 能运行 mission viewer，查看航迹、事件和反射率；
- [ ] 能解释 B 单调时间、A 时间、UTC 和同步 generation；
- [ ] 能解释反射率公式、天空帧因果匹配和 500 ms 新鲜度限制；
- [ ] 能解释 DHR1、DTF2、DTA1、MQTT QoS 与“已写 SD/已离开 UART/已被地面接纳”四种不同完成语义；
- [ ] 能启动地面 validator，确认 `TELEMETRY READY` 后再起飞，并从计数器区分设计内 4 Hz 选择、池溢出、10 秒过期和基础设施故障；
- [ ] 能区分已验证能力、工程假设和待标定能力；
- [ ] 能运行全部主机测试、固件构建和至少一次硬件回归；
- [ ] 能在改协议或数据格式时同步更新固件、模拟器、查看器和文档。

## 22. 故障排查速查

| 现象 | 优先检查 |
|---|---|
| 刷写后第一次卡在 H1-A preparation timeout | SC16 复位/供电、500 ms 稳定延时是否仍存在、H1 电源和接线 |
| `invalid header: 0xffffffff` 循环 | Flash 是否有有效镜像、下载模式、供电、flash 焊接/配置 |
| COM 端口占用 | VS Code monitor、串口助手、旧 Python/IDF 进程 |
| SD 能识别但创建文件失败 | 是否 FAT32、写保护、目录/文件系统损坏、卡质量 |
| 明亮光源下 RX overrun | SC16 RX 服务优先级、SPI 时钟、任务阻塞、停止顺序、软件流缓冲统计 |
| 一次坏帧后持续解析失败 | H1 重同步逻辑、原始字节日志、错误阈值和流停止/重启 |
| START 重传导致计数重置 | 动作缓存、相同 CMD/SEQ/payload 去重和 session 幂等 |
| 心跳正常但文件少 | recorder drop/reject 统计、EVENTS、错误码 5、SD flush 延迟 |
| 任务结束后文件时间错误 | UTC 是否 LOCKED、是否收到并完成 `0x30`、FAT/Windows 时区显示 |
| MQTTX 有消息但 B 板仍重试 | MQTTX 不发 DTA1；检查 validator、down topic、DTA1 QoS、DTU 下行订阅和身份/CRC 是否匹配 |
| 遥测池持续增长或频繁过期 | broker/蜂窝延迟、validator 是否 READY、DTU 460800/1024 字节/5 ms 配置、ACK topic、`*_expired` 和 RTT |
| GPS 正常而反射率很少 | 多分片丢失、DTU 转发容量、4 Hz 选择计数、反射率是否实际产生；不要只看 MQTT publication 数 |
| 百度地图轨迹整体偏移 | 尚未做 WGS84→BD-09 转换 |
| 光谱点没有位置 | GPS 前后点不足、跨同步 generation、定位间隔过宽或时间无效 |

## 23. 最小交接演示

建议交接会议现场完成一次端到端演示：

1. 展示专用板、两台 H1、SD 卡、COM5 模拟链路和 COM6 USB；
2. 从干净构建目录编译并下载；
3. 先启动 MQTT broker probe 和地面 validator，确认 `TELEMETRY READY`；
4. 启动带 GPS 的模拟飞行；
5. 演示握手、开始、停止、使用新 session 重启；
6. 注入一次丢 ACK 或坏 CRC，确认不重复执行且能继续；
7. 发送 `0x30`，等待心跳安全断电；
8. 等待 validator 汇总，解释地面接纳数、重试、过期和池峰值；
9. 取卡并打开新 `F_xxxx`；
10. 用 GUI 展示航迹、事件、无测量点和测量点；
11. 点击一个测点查看反射率；
12. 查看 `MISSION.JSON`、`EVENTS.JSONL` 和解码健康信息；
13. 跑全部主机测试并保存测试输出；
14. 说明尚未完成的真实 A 板、科学标定、户外遥测资格测试和正式地面接收服务。

完成这条演示链路后，新同事不仅能运行现有系统，也能理解后续修改最容易破坏的协议、实时性和数据完整性边界。
