"""
ECDH 共享密钥存储模块

全局、线程安全的 IP → 共享密钥映射表。
在 ECDH 握手之后，将 peer 的 IP 与计算出的 32 字节 AES-256 密钥关联存储。

使用场景：
- protocol.py：发送/接收消息时自动查询密钥以决定是否加密
- signaling_client.py：test_connect() 握手成功后存储密钥
- main.py：DEVICE_HELLO 回调中存储密钥
"""

import threading


class PeerKey:
    """线程安全的对端 ECDH 共享密钥存储

    使用类级别静态字典 + Lock 实现线程安全。
    密钥是 ECDH 输出的原始 32 字节，直接用作 AES-256-GCM 密钥。
    """

    # 类级别线程锁（保护 _secrets 字典）
    _lock = threading.Lock()
    # IP 地址字符串 → 32 字节 AES 密钥 的映射
    _secrets: dict[str, bytes] = {}

    @classmethod
    def store(cls, ip: str, shared_secret: bytes):
        """存储某个 IP 的共享密钥（覆盖已有的旧密钥）"""
        with cls._lock:
            cls._secrets[ip] = shared_secret

    @classmethod
    def get(cls, ip: str) -> bytes | None:
        """获取某个 IP 的共享密钥，无密钥则返回 None"""
        with cls._lock:
            return cls._secrets.get(ip)

    @classmethod
    def has(cls, ip: str) -> bool:
        """检查是否已持有某个 IP 的共享密钥"""
        with cls._lock:
            return ip in cls._secrets

    @classmethod
    def remove(cls, ip: str):
        """删除某个 IP 的密钥记录（对应设备被移除时调用）"""
        with cls._lock:
            cls._secrets.pop(ip, None)
