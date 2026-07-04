"""
数据类型与常量定义模块

定义整个 P2P 文件传输工具的核心数据结构、枚举和默认配置常量。
协议消息类型、传输状态、设备信息、文件元信息等均在此定义。
"""

from dataclasses import dataclass, field
from enum import Enum
import time

# ─── 端口与网络常量 ───────────────────────────────────────────────────────────
# UDP 组播设备发现端口，用于局域网内自动发现其他设备
DISCOVERY_PORT = 8888
# TCP 信令控制通道端口，短连接模式，处理握手、消息、文件请求
SIGNALING_PORT = 8889
# TCP 文件数据传输端口，多连接并发传输
TRANSFER_PORT = 8890
# HTTP Web UI + REST API 服务端口
HTTP_PORT = 8891
# 设备离线判定超时（秒），超过此时间未收到心跳则标记为离线
DEVICE_TIMEOUT = 10
# UDP 组播广播间隔（秒），每 3 秒发送一次 DEVICE_BROADCAST
BROADCAST_INTERVAL = 3
# 文件传输分块大小，64KB（65536 字节）
CHUNK_SIZE = 65536
# 默认并行传输线程数，每个线程打开一个独立 TCP 连接
NUM_THREADS = 4
# UDP 组播地址，239.255.255.250 为本地管理范围组播
MULTICAST_ADDR = "239.255.255.250"
# 单条 JSON 消息最大大小 10MB，超过则拒绝接收
MAX_MSG_SIZE = 10 * 1024 * 1024
# 数据通道单次接收最大大小 100MB
MAX_DATA_SIZE = 100 * 1024 * 1024


class TransferState(Enum):
    """文件传输任务的7种生命周期状态"""
    IDLE = "idle"               # 初始空闲
    NEGOTIATING = "negotiating" # 协商中（双方确认传输参数）
    TRANSFERRING = "transferring" # 正在传输
    PAUSED = "paused"           # 已暂停
    COMPLETED = "completed"     # 传输完成
    FAILED = "failed"           # 传输失败
    CANCELLED = "cancelled"     # 已被取消


@dataclass
class DeviceInfo:
    """局域网中一个 P2P 设备的信息快照

    关键字段说明：
    - manual: 手动添加的设备标记为 True，永远不会被超时清理
    - last_seen: 最后一次收到该设备消息的时间戳，用于在线判定
    - first_seen: 首次发现时间，保留设备"年龄"信息
    """
    id: str = ""                # 设备唯一标识（UUID v4）
    name: str = ""              # 设备显示名称
    ip: str = ""                # 设备 IPv4 地址
    port: int = 0               # 信令端口（通常 8889）
    last_seen: float = 0.0      # 最后活跃时间戳
    first_seen: float = 0.0     # 首次发现时间戳
    manual: bool = False        # 是否手动添加（手动设备永不过期）


@dataclass
class FileMeta:
    """待传输文件的元信息

    包含文件标识、大小、分块参数、压缩标记。
    接收方根据此信息创建 .tmp 文件并分配接收位图。
    """
    file_id: str = ""           # 文件传输任务唯一 ID（UUID）
    filename: str = ""          # 原始文件名
    file_size: int = 0          # 文件总字节数
    chunk_size: int = CHUNK_SIZE  # 每块大小（默认 64KB）
    total_chunks: int = 0       # 总分块数
    compression: str = ""       # 压缩算法标记："zlib" 或空


@dataclass
class TransferTask:
    """传输任务的完整运行时状态

    由 TransferManager 管理，供 Web UI REST API 查询进度。
    window_size / retry_count 为旧版滑动窗口协议遗留字段，当前不使用。
    """
    meta: FileMeta = field(default_factory=FileMeta)  # 文件元信息
    target: DeviceInfo = field(default_factory=DeviceInfo)  # 目标设备
    state: TransferState = TransferState.IDLE  # 当前传输状态
    progress_chunk: int = 0     # 已完成分块数
    bytes_sent: int = 0         # 已发送字节数
    speed: float = 0.0          # 传输速率（bytes/s）
    window_size: int = 16       # 遗留：滑动窗口大小
    retry_count: int = 0        # 遗留：重试次数
    local_file_path: str = ""   # 本地文件路径（发送方）
    is_sender: bool = True      # 是否为本方发起的发送任务


@dataclass
class TransferRecord:
    """传输历史记录（已完成、失败或取消的任务）"""
    file_id: str = ""
    filename: str = ""
    device_name: str = ""       # 对端设备名
    file_size: int = 0
    is_sender: bool = True      # 本方是发送方还是接收方
    final_state: str = ""        # 最终状态字符串
    timestamp: float = 0.0      # 完成时间戳


class MsgType:
    """协议消息类型常量集合

    无线协议使用 JSON 字符串进行类型判别。
    分为四类：
    - 发现：DEVICE_BROADCAST, DEVICE_OFFLINE
    - ECDH 握手：DEVICE_HELLO, DEVICE_HELLO_ACK
    - 即时通信：TEXT_MESSAGE, TEXT_ACK
    - 文件传输控制：FILE_RANGE, RANGE_DONE, RANGE_ACK 等
    """
    # ── 设备发现 ──
    DEVICE_BROADCAST = "DEVICE_BROADCAST"   # UDP 组播心跳广播
    DEVICE_OFFLINE = "DEVICE_OFFLINE"       # 设备主动离线通知

    # ── ECDH 密钥交换 ──
    DEVICE_HELLO = "DEVICE_HELLO"           # 探测握手，携带发送方 ECDH 公钥
    DEVICE_HELLO_ACK = "DEVICE_HELLO_ACK"   # 握手应答，携带服务端 ECDH 公钥

    # ── 文本消息 ──
    TEXT_MESSAGE = "TEXT_MESSAGE"           # 加密文本消息
    TEXT_ACK = "TEXT_ACK"                   # 文本消息确认回执

    # ── 文件传输信令 ──
    FILE_REQUEST = "FILE_REQUEST"           # 文件传输请求
    FILE_RESPONSE = "FILE_RESPONSE"         # 文件传输应答（接受/拒绝）
    FILE_HEADER = "FILE_HEADER"             # 旧版单连接文件头
    FILE_RANGE = "FILE_RANGE"               # 新版多连接分块范围声明
    RANGE_DONE = "RANGE_DONE"               # 单个连接分块发送完毕
    RANGE_ACK = "RANGE_ACK"                 # 单个连接分块接收确认
    CHUNK_ACK = "CHUNK_ACK"                 # 旧版分块确认
    TRANSFER_CANCEL = "TRANSFER_CANCEL"     # 取消传输
    TRANSFER_PAUSE = "TRANSFER_PAUSE"       # 暂停传输
    TRANSFER_RESUME = "TRANSFER_RESUME"     # 恢复传输
    TRANSFER_DONE = "TRANSFER_DONE"         # 传输完成通知
    TRANSFER_ERROR = "TRANSFER_ERROR"       # 传输错误通知
    CONTROL_ACK = "CONTROL_ACK"             # 控制消息确认回执


class Status:
    """文件传输应答状态常量"""
    ACCEPT = "ACCEPT"   # 接受传输请求
    REJECT = "REJECT"   # 拒绝传输请求
    PAUSE = "PAUSE"     # 暂停传输
