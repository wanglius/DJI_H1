# 源码来源与维护边界

本子项目从仓库提交 `37bb19b` 的已验证地面代码复制开始，保留算法和协议测试，并只在新目录内做适配。

| 原来源 | 本项目副本 |
|---|---|
| tools/mission_viewer/dji_h1_viewer/decoder.py | src/dji_h1_ground/decoder.py |
| 同目录 telemetry.py | DTF2 / DGB1 / DTA1 协议及重组 |
| 同目录 mission.py / geolocation.py / presentation.py | 离线任务、插值和地图数据模型 |
| 同目录 live.py / live_transport.py | 网络/解码/ACK 分离的接收基础 |
| 同目录 ui.py / baidu_map.py / resources | GUI、离线图、百度卫星图模板 |
| tests/mission_viewer / record_format / telemetry_transport | tests 下独立副本 |

新增 config.py、messages.py、journal.py、receiver.py、map_panel.py、cli.py、中文文档及 ground_app 集成测试。复制副本统一使用 `dji_h1_ground` 命名空间，既有 `dji_h1_viewer` 的任何模块都不被本包导入，也没有软链接到原代码。

维护约定：

1. Python 包的接口从 `__init__.py` 导出；GUI 为可选依赖，核心模块不得导入 Qt。
2. 网络回调仅入队；格式验证、持久化和记录接收后才能创建成功 DTA1。
3. 地图通过数据模型显示，不拥有接收器、文件句柄、任务生命周期或 ACK。
4. 地图失败路径只切换地图面板；HTTP 查询和 receiver 消费接口独立运行。
5. 所有真实配置、接收数据库和生成物按子项目 .gitignore 排除。
6. 后续固件协议变化需要显式移植到此独立副本，并更新测试向量；不会自动随原 viewer 变化。

目录说明：`src/dji_h1_ground` 是可安装库，`tests` 可在独立文件夹运行，`config` 提供安全模板，`docs` 提供中文操作和集成说明，`requirements.txt` 安装完整桌面依赖，`pyproject.toml` 定义核心/可选依赖和命令入口。
