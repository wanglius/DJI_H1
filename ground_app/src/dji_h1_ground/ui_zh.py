"""Chinese presentation labels only; never translate stored/API protocol values."""

EVENT_NAMES = {
    "handshake": "握手", "segment_start": "测量段开始",
    "stop_request": "停止请求", "segment_end": "测量段结束",
    "power_off_request": "断电预告", "protocol_crc_error": "协议 CRC 错误",
    "protocol_timeout": "协议超时", "clock_observation_drop": "时钟观测丢弃",
    "reflectance_rejected": "反射率结果被拒绝", "capture_result": "采集结果",
    "flight_closed": "飞行记录关闭", "drone_identity_mismatch": "飞行器身份不一致",
    "ab_link_lost": "A-B 链路丢失", "ab_link_restored": "A-B 链路恢复",
    "storage_error": "存储错误", "write_error": "写入错误",
    "flush_error": "刷盘错误", "acquisition_error": "采集错误",
    "rx_overrun": "接收溢出", "frame_drop": "帧丢弃",
}

_VALUES = {
    "unknown": "未知", "none": "无", "disabled": "已禁用",
    "starting": "正在启动", "connecting": "正在连接",
    "connected": "已连接", "subscribing": "正在订阅",
    "subscribed": "已订阅", "reconnecting": "正在重连",
    "stopped": "已停止", "failed": "失败", "connect failed": "连接失败",
    "subscribe failed": "订阅失败", "disconnected": "已断开",
    "receiving": "正在接收", "replaying": "正在回放",
    "replay complete": "回放完成", "open": "已打开", "closed": "已关闭",
    "idle": "空闲", "recording": "正在记录", "active": "进行中",
    "live": "实时", "offline": "离线", "empty": "无任务",
    "info": "信息", "warning": "警告", "error": "错误", "critical": "严重",
    "exact": "精确命中", "interpolated": "插值", "wide_gap": "大间隔插值",
}

_EVENT_FIELDS = {
    "sequence": "记录序号", "event_code": "事件码", "severity": "严重程度",
    "session_id": "会话号", "segment_id": "测量段号",
    "b_monotonic_us": "B 板单调时间（微秒）", "a_monotonic_ms": "A 板单调时间（毫秒）",
    "utc_ms": "UTC 时间（毫秒）", "sync_state": "同步状态码",
    "sync_generation": "同步代次", "time_valid_flags": "时间有效位",
    "argument0": "参数 0", "argument1": "参数 1", "reason": "原因码",
}


def display_value(value: object) -> str:
    text = str(value)
    return _VALUES.get(text, text)


def event_name(value: object) -> str:
    text = str(value)
    return EVENT_NAMES.get(text, f"未识别事件（{text}）")


def event_field(value: str) -> str:
    return _EVENT_FIELDS.get(value, value)
