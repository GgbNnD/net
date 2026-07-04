"""
TCP 信令客户端模块

提供两种使用模式：
1. send_request()：通用请求-应答模式，发送一条消息并等待响应
2. test_connect()：设备探测 + ECDH 密钥交换模式

非阻塞连接实现：
- connect_to() 使用经典的非阻塞 connect + select 超时模式
- 先设置 O_NONBLOCK → connect() → select() 等待 writable → 检查 SO_ERROR
- 连接成功后恢复为阻塞模式并设置超时

ECDH 握手流程（test_connect）：
1. 非阻塞 TCP 连接到目标设备的信令端口
2. 发送 DEVICE_HELLO（携带本机 ECDH 公钥）
3. 接收 DEVICE_HELLO_ACK（提取远端的 ECDH 公钥）
4. 计算共享密钥并存入 PeerKey
"""

import socket
import select
import errno
import time
from p2p_transfer.common.types import SIGNALING_PORT, MsgType
from p2p_transfer.common.platform import (
    set_nonblocking,
    set_blocking,
)
from p2p_transfer.common.protocol import (
    send_json_message,
    recv_json_message,
    build_device_hello,
)
from p2p_transfer.common.peer_key import PeerKey
from p2p_transfer.common.utils import compute_ecdh_shared


class SignalingClient:
    """短连接 TCP 信令客户端

    所有方法均为静态方法，无需实例化。
    """

    @staticmethod
    def connect_to(target_ip: str, target_port: int, timeout_ms: int = 5000) -> socket.socket | None:
        """非阻塞 TCP 连接到目标设备

        使用非阻塞 connect + select 超时模式：
        1. 创建 TCP 套接字
        2. 设置为非阻塞模式（O_NONBLOCK）
        3. 调用 connect()（预期返回 BlockingIOError 或 EINPROGRESS）
        4. 使用 select() 等待套接字变为可写
        5. 通过 getsockopt(SO_ERROR) 确认连接成功
        6. 恢复为阻塞模式并设置超时

        Args:
            target_ip: 目标 IP 地址
            target_port: 目标端口
            timeout_ms: 连接超时时间（毫秒）

        Returns:
            已连接的阻塞套接字，失败返回 None
        """
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        set_nonblocking(sock)

        # 非阻塞 connect，预期抛出 BlockingIOError（正常的 EINPROGRESS）
        try:
            sock.connect((target_ip, target_port))
        except BlockingIOError:
            pass                                    # 正常的连接进行中状态
        except OSError as e:
            sock.close()
            return None

        # 使用 select() 等待连接完成
        _, writable, _ = select.select([], [sock], [], timeout_ms / 1000.0)
        if not writable:
            sock.close()
            return None

        # 检查 SO_ERROR 确认连接成功
        err = sock.getsockopt(socket.SOL_SOCKET, socket.SO_ERROR)
        if err != 0:
            sock.close()
            return None

        # 连接成功，恢复阻塞模式
        set_blocking(sock)
        sock.settimeout(timeout_ms / 1000.0)
        return sock

    @staticmethod
    def send_request(
        target_ip: str,
        target_port: int,
        request: dict,
        timeout_ms: int = 5000,
    ) -> dict | None:
        """发送一条请求消息并等待应答

        完整的短连接流程：connect → send → recv → close。
        send/recv 自动根据 PeerKey 决定是否加密。

        Args:
            target_ip: 目标 IP
            target_port: 目标端口
            request: 要发送的 JSON 协议消息
            timeout_ms: 超时毫秒

        Returns:
            解析后的 JSON 应答字典，失败返回 None
        """
        sock = SignalingClient.connect_to(target_ip, target_port, timeout_ms)
        if sock is None:
            return None
        try:
            send_json_message(sock, request, target_ip)      # 自动加密
            response = recv_json_message(sock, target_ip)     # 自动解密
            return response
        except Exception as e:
            return None
        finally:
            try:
                sock.close()
            except:
                pass

    @staticmethod
    def test_connect(
        target_ip: str,
        target_port: int,
        device_id: str,
        device_name: str,
        local_ip: str,
        public_key: str,
        private_key: str,
        timeout_ms: int = 5000,
    ) -> bool:
        """设备探测连接 + ECDH 密钥交换

        此方法由主程序的探测线程调用，用于定期检查设备在线状态并完成 ECDH 握手。

        流程：
        1. 连接到目标设备信令端口
        2. 发送 DEVICE_HELLO（携带本机 ECDH 公钥）
        3. 接收 DEVICE_HELLO_ACK
        4. 提取远端的 ECDH 公钥
        5. 使用本地私钥和远端公钥计算共享密钥
        6. 将 32 字节共享密钥存入 PeerKey

        Args:
            target_ip: 目标设备 IP
            target_port: 目标信令端口（通常 8889）
            device_id: 本机 UUID
            device_name: 本机名称
            local_ip: 本机 IP
            public_key: 本机 ECDH 公钥（Base64）
            private_key: 本机 ECDH 私钥（Base64）
            timeout_ms: 超时毫秒

        Returns:
            握手是否成功
        """
        sock = SignalingClient.connect_to(target_ip, target_port, timeout_ms)
        if sock is None:
            return False

        try:
            sock.settimeout(timeout_ms / 1000.0)

            # 发送 DEVICE_HELLO 探测消息（携带本机 ECDH 公钥）
            hello = build_device_hello(
                device_id, device_name, local_ip, target_port, public_key
            )
            send_json_message(sock, hello, target_ip)

            # 接收 DEVICE_HELLO_ACK 应答
            response = recv_json_message(sock, target_ip)
            if response is None:
                return False

            msg_type = response.get("type", "")
            if msg_type != MsgType.DEVICE_HELLO_ACK:
                return False

            # 提取远端公钥并计算 ECDH 共享密钥
            remote_public = response.get("public_key", "")
            if remote_public:
                shared = compute_ecdh_shared(private_key, remote_public)
                if shared and len(shared) == 32:
                    # 将 32 字节共享密钥存入 PeerKey，后续通信自动加密
                    PeerKey.store(target_ip, shared)
                    return True

            return True

        except Exception:
            return False
        finally:
            try:
                sock.close()
            except:
                pass
