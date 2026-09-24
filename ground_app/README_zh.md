# DJI H1 独立地面站

M100M 分支增加 DTM1 完整消息支持：每个 MQTT publication 直接校验和解码，不经过分片重组；DTA1、持久日志和地图隔离语义不变。仍兼容旧 DTF2。新固件默认部署模板使用 `dji-h1/m100m/up` / `dji-h1/m100m/down`；请将本地地面配置与固件配置对齐，勿沿用旧 topic 而误判无数据。详见项目 `Docs/m100m_integration_zh.md`。

本子项目用于接收 MQTT 遥测、校验 DTM1 完整消息（兼容旧 DTF2 分片重组）、展开 DGB1 GPS 批次、解码 DHR1 v01 数据、保存地面接收日志，以及通过 PyQt GUI / Python / 本机 HTTP 查询飞行数据。

可以把整个 `ground_app` 文件夹交给同事，或复制到另一台电脑单独安装。运行不依赖上级固件工程、ESP-IDF、`tools/mission_viewer`、原有 `tests` 目录或硬件串口。当前 Python 包名为 `dji_h1_ground`，发行版本为 `0.1.0`。

## 1. 首次安装（Windows PowerShell）

安装 Python 3.10 或更新版本，然后进入本文件所在的 `ground_app` 文件夹：

```powershell
python -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r requirements.txt
Copy-Item config/ground_app.example.json config/ground_app.local.json
notepad config/ground_app.local.json
```

复制模板只做一次，已有配置不要覆盖。编辑 MQTT 的 `host`、`port`、用户名、密码和 topic；`host` 填主机名，不带 `http://`、端口或管理后台路径。可选填写地图 AK。默认地图关闭，使用离线坐标图。配置解析后不会主动访问任何服务。

```powershell
.\.venv\Scripts\python.exe run_ground_app.py --check-config
```

命令输出配置有效后，按需要启动：

```powershell
# 实时 MQTT GUI
.\.venv\Scripts\python.exe run_ground_app.py --live

# 打开 SD 卡上的离线任务目录
.\.venv\Scripts\python.exe run_ground_app.py --mission D:\F_0003

# 先开窗口，再用按钮选择任务或实时配置
.\.venv\Scripts\python.exe run_ground_app.py

# 无 GUI 接收器：解码消息以 JSONL 输出到 stdout，状态提示在 stderr
.\.venv\Scripts\python.exe run_ground_app.py --headless
```

默认读取当前目录下的 `config/ground_app.local.json`。从其他目录启动时，用 `--config C:\...\ground_app.local.json` 指定绝对路径。保存目录和 TLS CA 路径始终相对于配置文件所在目录解析。

## 2. 只安装 API，不安装 GUI

```powershell
python -m pip install -e .
```

核心只有 `paho-mqtt` 外部依赖；协议解码、SQLite、HTTP 都使用标准库。可分别添加：

```powershell
python -m pip install -e ".[gui]"       # PyQt6，离线地图
python -m pip install -e ".[gui,map]"   # 再添加 Qt WebEngine / 百度卫星图
```

安装后提供 `dji-h1-ground` 命令，也可以 `python -m dji_h1_ground --live --config ...`。接收 API 的导入不加载 Qt 或地图模块。

## 3. 本机已有的凭据

仓库只提供 `config/ground_app.example.json`。真实配置使用 `config/ground_app.local.json`，该路径由本子项目的 `.gitignore` 排除。密码和百度 AK 还可分别通过 `DJI_H1_MQTT_PASSWORD`、`DJI_H1_BAIDU_AK` 环境变量覆盖。

检查是否被 Git 忽略：

```powershell
git check-ignore config/ground_app.local.json
```

配置切勿粘贴进提交或诊断截图。浏览器 AK 是浏览器使用的凭据，即使不进 Git，也不能对运行浏览器的用户保密；需在百度控制台设置适当的使用限制。

## 4. 数据与确认语义

支持类型：GPS(1)、原始光谱(2)、反射率(3)、主要操作事件(4)、GPS 批次(5)。现有生产固件遥测主要发送 GPS 批次、反射率和主要事件；完整原始光谱、`MISSION.JSON`、完整心跳和全部诊断日志仍以 SD 卡为准。本应用不会从缺失的遥测信息伪造这些文件。

生产参数模板沿用上行订阅 QoS 1、地面 DTA1 发布 QoS 0。MQTT PUBACK 是链路确认，DTA1 是应用层接受确认。开启地面日志时，完整消息经校验、日志提交和内存任务接收后才排队 DTA1。GUI/地图不参与确认条件。

一次启动创建一个 `GROUND_时间_随机后缀.sqlite3`。数据库内同时保存入口 MQTT payload（包括无法解码的内容）和成功解码的消息。磁盘故障计数可见，受影响的消息不发成功 DTA1；发送端仍受自身最大存活时间限制。队列溢出的入口数据无法写入日志，因此不能宣称地面接收绝对零丢失。

## 5. 日志回放和导出

```powershell
# 不连接 broker、不发 ACK，以最快速度重新重组/解码原始接收记录
python run_ground_app.py --replay data/GROUND_xxx.sqlite3

# 原时间间隔回放，结果仍输出到命令行
python run_ground_app.py --replay data/GROUND_xxx.sqlite3 --speed 1

# 导出当时成功解码的消息，每行一个 JSON；目标已存在则拒绝覆盖
python run_ground_app.py --export data/GROUND_xxx.sqlite3 --output decoded.jsonl
```

本版本回放通过 CLI/Python API 使用；GUI 支持实时与 SD 目录模式。JSONL 可交给同事的系统或数据分析程序。SQLite 不是 SD 任务目录格式，不要直接把数据库文件当成 `--mission` 参数。

## 6. 开发者入口

```python
from dji_h1_ground import GroundReceiver

with GroundReceiver.from_config("config/ground_app.local.json") as receiver:
    for message in receiver.iter_messages():
        # 在自己的应用工作线程中消费；此处的慢逻辑不会阻塞 MQTT 回调。
        print(message.message_type, message.message_sequence)
        for record in message.records:
            print(record.header.utc_ms)
```

详细说明见：

- [配置文件说明](docs/配置文件说明_zh.md)
- [Python 与 HTTP API 使用说明](docs/API使用说明_zh.md)
- [解码数据结构说明](docs/解码数据结构说明_zh.md)
- [GUI 使用说明](docs/GUI使用说明_zh.md)
- [故障诊断及验证](docs/故障诊断_zh.md)
- [源码来源及维护边界](docs/源码来源_zh.md)

## 7. 自测与分发

无需硬件即可生成一分钟示例航迹、60 条光谱和一个事件：

```powershell
python examples/make_demo.py data/demo_mission
python run_ground_app.py --mission data/demo_mission --config config/ground_app.example.json
```

示例生成器拒绝覆盖已有目录。`examples/receive_messages.py` 提供最小实时集成入口，先安装本包再运行。

```powershell
python -m unittest discover -s tests -v
python -m pip wheel --no-deps --no-build-isolation . -w dist
```

测试使用合成记录和假的 MQTT 传输，不连生产 broker，不发送飞行指令，不需要地图 AK。GUI 测试在装有 PyQt6 时以 offscreen 模式运行；未装 Qt 时会明确跳过。

本子项目使用独立副本，既有固件、原 mission viewer、原测试脚本保持原样。新增功能应在 `ground_app` 内开发，并保留协议测试向量。当前不附带 Windows 单文件 exe、WebSocket 推送服务或自动磁盘清理；这些可在稳定 API 的基础上另行交付。
