"""
UDP 组播设备发现模块

功能：
- 周期性地通过 UDP 组播发送 DEVICE_BROADCAST 心跳广播
- 监听组播地址，接收其他设备的广播消息
- 自动加入所有非回环网卡的组播组，确保多网卡环境下可靠发现

关键实现细节：
- 发送线程发送间隔为 BROADCAST_INTERVAL（3 秒），以 100ms 片段休眠以响应停止信号
- 接收线程使用 settimeout(0.5) 的阻塞 recvfrom，超时后检查运行标志
- 广播消息中的 port 字段填写 SIGNALING_PORT（8889），而非 DISCOVERY_PORT（8888）
- 收到广播后，使用数据包的源 IP 覆盖 JSON 中的 ip 字段（比 JSON 字段更可靠）
- 忽略本机发出的消息（通过 device_id 比对）
"""

import socket
import struct
import json
import threading
import time
from p2p_transfer.common.types import (
    DISCOVERY_PORT,
    SIGNALING_PORT,
    BROADCAST_INTERVAL,
    MULTICAST_ADDR,
    DeviceInfo,
    MsgType,
)
from p2p_transfer.common.platform import (
    select_local_ip,
    set_reuse_addr,
    get_local_ips,
)


class DeviceDiscovery:
    """UDP 组播设备发现服务

    维护两个后台线程：
    - _send_loop：每 3 秒向组播组发送 DEVICE_BROADCAST
    - _recv_loop：持续监听组播消息并通知回调

    停止时发送 3 次 DEVICE_OFFLINE 尽力通知其他设备。
    """

    def __init__(
        self,
        device_id: str,
        device_name: str,
        port: int = DISCOVERY_PORT,
        on_device_found=None,
    ):
        """初始化设备发现服务

        Args:
            device_id: 本机唯一标识（UUID）
            device_name: 本机显示名称
            port: UDP 监听端口（默认 8888）
            on_device_found: 发现设备时的回调函数 callback(DeviceInfo)
        """
        self._device_id = device_id
        self._device_name = device_name
        self._port = port
        self._running = False

        # 套接字与线程管理
        self._sock: socket.socket | None = None
        self._send_thread: threading.Thread | None = None
        self._recv_thread: threading.Thread | None = None

        # 自动选择主网卡 IP 作为本机局域网地址
        self._local_ip = select_local_ip()
        self._on_device_found = on_device_found

    def set_on_device_found(self, callback):
        """设置设备发现回调函数（在 start() 之前或之后均可调用）"""
        self._on_device_found = callback

    @property
    def local_ip(self) -> str:
        """获取自动选择的本机局域网 IP 地址"""
        return self._local_ip

    def start(self):
        """启动发现服务：创建套接字并启动收发线程"""
        self._sock = self._create_socket()
        self._running = True
        self._send_thread = threading.Thread(target=self._send_loop, daemon=True)
        self._recv_thread = threading.Thread(target=self._recv_loop, daemon=True)
        self._send_thread.start()
        self._recv_thread.start()

    def stop(self):
        """停止发现服务：发送离线通知、关闭套接字、等待线程退出"""
        self._running = False

        # 尽力通知其他设备（UDP best-effort 3 次发送）
        for _ in range(3):
            try:
                msg = json.dumps(
                    {"type": MsgType.DEVICE_OFFLINE, "device_id": self._device_id}
                )
                self._sock.sendto(
                    msg.encode("utf-8"), (MULTICAST_ADDR, self._port)
                )
            except:
                pass

        # 关闭套接字以唤醒阻塞在 recvfrom 的接收线程
        sock = self._sock
        self._sock = None
        if sock:
            try:
                sock.close()
            except:
                pass

        # 等待两个线程退出（最多 2 秒）
        if self._send_thread:
            self._send_thread.join(timeout=2)
        if self._recv_thread:
            self._recv_thread.join(timeout=2)

    def _create_socket(self) -> socket.socket:
        """创建并配置 UDP 组播套接字

        配置步骤：
        1. 创建 UDP 套接字并设置 SO_REUSEADDR
        2. 绑定到 0.0.0.0:{port}
        3. 加入组播组（默认接口 + 所有非回环接口）
        4. 设置 TTL = 64（局域网范围）
        5. 设置组播出口接口为主网卡 IP
        """
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        set_reuse_addr(sock)
        sock.bind(("0.0.0.0", self._port))

        # 加入组播组：IP_ADD_MEMBERSHIP 参数为 (组播地址, 本地接口IP)
        # 首先加入默认接口（0.0.0.0 表示内核自动选择）
        mreq = struct.pack(
            "!4s4s",
            socket.inet_aton(MULTICAST_ADDR),
            socket.inet_aton("0.0.0.0"),
        )
        sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)

        # 为每一个非回环网卡独立加入组播组（多网卡兼容）
        for ip in get_local_ips():
            if ip != self._local_ip and not ip.startswith("127."):
                try:
                    mreq2 = struct.pack(
                        "!4s4s",
                        socket.inet_aton(MULTICAST_ADDR),
                        socket.inet_aton(ip),
                    )
                    sock.setsockopt(
                        socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq2
                    )
                except:
                    pass

        # 组播 TTL = 64，限制在同一局域网内
        sock.setsockopt(
            socket.IPPROTO_IP,
            socket.IP_MULTICAST_TTL,
            struct.pack("B", 64),
        )
        # 指定组播出口接口
        try:
            sock.setsockopt(
                socket.IPPROTO_IP,
                socket.IP_MULTICAST_IF,
                socket.inet_aton(self._local_ip),
            )
        except:
            pass

        return sock

    def _send_loop(self):
        """发送线程：定期广播 DEVICE_BROADCAST

        每 BROADCAST_INTERVAL 秒广播一次。
        休眠以 100ms 为单位分段执行，确保对 _running 标志的响应延迟不超过 100ms。
        **重要**：广播消息中的 port 字段为 SIGNALING_PORT (8889)，而非 m_port (8888)。
        """
        while self._running:
            msg = {
                "type": MsgType.DEVICE_BROADCAST,
                "device_id": self._device_id,
                "device_name": self._device_name,
                "ip": self._local_ip,
                "port": SIGNALING_PORT,            # 关键：告知接收方使用信令端口 8889 连接
                "timestamp": time.time(),
            }
            data = json.dumps(msg).encode("utf-8")
            try:
                if self._sock:
                    self._sock.sendto(data, (MULTICAST_ADDR, self._port))
            except:
                pass

            # 分段休眠以响应停止信号
            for _ in range(int(BROADCAST_INTERVAL * 10)):
                if not self._running:
                    break
                time.sleep(0.1)

    def _recv_loop(self):
        """接收线程：持续监听组播消息

        使用 0.5 秒超时的 recvfrom，超时后检查 _running 标志。
        解析收到的 JSON 消息，提取设备信息并触发回调。
        用数据包的源 IP 覆盖 JSON 中的 ip 字段，避免因 NAT 或误配置导致的 IP 错误。
        忽略本机发出的消息（通过 device_id 比对）。
        """
        while self._running:
            if self._sock is None:
                break
            try:
                self._sock.settimeout(0.5)
                data, addr = self._sock.recvfrom(2048)
            except socket.timeout:
                continue
            except:
                if self._running:
                    continue
                break

            # 解析 JSON 消息
            try:
                msg = json.loads(data.decode("utf-8"))
            except (json.JSONDecodeError, UnicodeDecodeError):
                continue

            msg_type = msg.get("type", "")
            # 仅处理广播和离线两种消息
            if msg_type not in (MsgType.DEVICE_BROADCAST, MsgType.DEVICE_OFFLINE):
                continue

            # 提取发送方消息
            sender_id = msg.get("device_id", "")
            sender_name = msg.get("device_name", "")
            # 使用数据包的源 IP（比 JSON 中的 ip 字段更可靠）
            sender_ip = addr[0]
            sender_port = msg.get("port", SIGNALING_PORT)
            timestamp = msg.get("timestamp", time.time())

            # 忽略本机发出的广播
            if sender_id == self._device_id:
                continue

            if msg_type == MsgType.DEVICE_OFFLINE:
                sender_id = msg.get("device_id", "")
                sender_ip = addr[0]

            # 触发回调通知 DeviceManager
            if self._on_device_found:
                info = DeviceInfo(
                    id=sender_id,
                    name=sender_name,
                    ip=sender_ip,
                    port=sender_port,
                    last_seen=timestamp,
                )
                self._on_device_found(info)
