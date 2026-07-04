"""
平台相关网络工具模块

提供跨平台的底层网络操作：
- 本地 IP 地址枚举（netifaces / ip 命令回退）
- 主网卡 IP 选择（UDP connect 技巧）
- 非阻塞/阻塞模式切换
- SO_REUSEADDR 设置
- BBR 拥塞控制尝试启用（Linux 内核 4.9+）
- 可恢复的网络错误判断（EAGAIN/EWOULDBLOCK/EINPROGRESS）
"""

import socket
import struct
import errno
import fcntl
import os
from .types import MULTICAST_ADDR, SIGNALING_PORT


# 可恢复的网络错误码集合：
# EWOULDBLOCK - 非阻塞套接字无数据可读/写
# EAGAIN - 同 EWOULDBLOCK（Linux 下两者等值）
# EINPROGRESS - 非阻塞 connect() 正在进行中（必须纳入判断，否则 connect 会误判为失败）
SOCKET_ERR = (
    errno.EWOULDBLOCK,
    errno.EAGAIN,
    errno.EINPROGRESS,
)


def get_local_ips() -> list[str]:
    """获取本机所有非回环 IPv4 地址

    优先使用 netifaces 库，若未安装则调用 ip 命令解析输出，
    最终回退到 127.0.0.1。
    """
    ips = []
    # 方案 1：使用 netifaces 库（可靠的跨平台方案）
    try:
        import netifaces
        for iface in netifaces.interfaces():
            addrs = netifaces.ifaddresses(iface)
            if netifaces.AF_INET in addrs:
                for addr in addrs[netifaces.AF_INET]:
                    ip = addr.get("addr", "")
                    if ip and not ip.startswith("127."):
                        ips.append(ip)
        return ips if ips else ["127.0.0.1"]
    except ImportError:
        pass

    # 方案 2：调用 ip 命令（Linux 专用，带 3 秒超时防止卡死）
    try:
        import subprocess
        import re
        output = subprocess.check_output(
            ["ip", "-4", "addr", "show"], text=True, timeout=3
        )
        for line in output.splitlines():
            m = re.search(r"inet\s+(\d+\.\d+\.\d+\.\d+)", line)
            if m:
                ip = m.group(1)
                if not ip.startswith("127."):
                    ips.append(ip)
        if ips:
            return ips
    except Exception:
        pass

    # 方案 3：全部回退无效，返回回环地址
    return ["127.0.0.1"]


def select_local_ip() -> str:
    """选择本机主网卡 IPv4 地址

    使用 UDP connect 技巧：创建一个临时的 UDP 套接字并尝试连接到
    1.1.1.1:53（Cloudflare DNS，不实际发送数据），然后通过 getsockname()
    获取内核路由选择出的源 IP，即为主网卡 IP。
    失败时回退到 get_local_ips() 的第一个结果。
    """
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("1.1.1.1", 53))          # 触发内核路由选择，不发送实际 UDP 包
        ip = s.getsockname()[0]
        s.close()
        if ip and ip != "0.0.0.0":
            return ip
    except:
        pass

    ips = get_local_ips()
    return ips[0] if ips else "127.0.0.1"


def set_nonblocking(sock: socket.socket):
    """将套接字设置为非阻塞模式（O_NONBLOCK）"""
    fd = sock.fileno()
    flags = fcntl.fcntl(fd, fcntl.F_GETFL)
    fcntl.fcntl(fd, fcntl.F_SETFL, flags | os.O_NONBLOCK)


def set_blocking(sock: socket.socket):
    """将套接字恢复为阻塞模式（清除 O_NONBLOCK 标志）"""
    fd = sock.fileno()
    flags = fcntl.fcntl(fd, fcntl.F_GETFL)
    fcntl.fcntl(fd, fcntl.F_SETFL, flags & ~os.O_NONBLOCK)


def set_reuse_addr(sock: socket.socket):
    """设置 SO_REUSEADDR 选项，允许端口复用和快速重启"""
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)


def enable_bbr(sock: socket.socket) -> bool:
    """尝试为 TCP 套接字启用 BBR 拥塞控制算法

    TCP_CONGESTION = 13（Linux 内核常量）。
    需要 Linux 内核 4.9+，若系统不支持则静默回退到默认算法（通常 cubic）。
    """
    try:
        sock.setsockopt(socket.IPPROTO_TCP, 13, b"bbr")
        return True
    except (OSError, AttributeError):
        return False


def is_would_block(err: int) -> bool:
    """判断 errno 值是否为可恢复的"会阻塞"错误

    包括 EWOULDBLOCK、EAGAIN（资源暂时不可用）和 EINPROGRESS（连接中）。
    用于非阻塞 socket 操作后的错误处理。
    """
    return err in SOCKET_ERR
