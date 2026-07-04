"""
加密工具、压缩工具与常规辅助函数模块

功能清单：
- UUID v4 生成
- MD5 文件/数据校验（仅测试用，协议层面不再使用）
- 时间戳与格式化（文件大小、传输速度、ETA）
- Base64 编解码
- URL 百分号解码
- AES-256-GCM 加密/解密（每消息随机 12 字节 IV，16 字节 GCM Tag）
- ECDH 密钥交换（secp256r1 / NIST P-256，raw 32 字节共享密钥，无哈希）
- zlib 压缩/解压
"""

import os
import sys
import uuid
import zlib
import base64
import hashlib
import struct
import time
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.ciphers.aead import AESGCM


# ═══════════════════════════════════════════════════════════════════════════════
# 标识符与校验
# ═══════════════════════════════════════════════════════════════════════════════

def generate_uuid() -> str:
    """生成 UUID v4 随机标识符字符串"""
    return str(uuid.uuid4())


def md5_file(path: str) -> str:
    """计算文件的 MD5 哈希值（32 字符十六进制）

    使用 8KB 缓冲分块读取，避免大文件占用过多内存。
    注：MD5 仅用于单元测试场景，协议层面已迁移至 AES-256-GCM 完整性校验。
    """
    h = hashlib.md5()
    with open(path, "rb") as f:
        while True:
            chunk = f.read(8192)
            if not chunk:
                break
            h.update(chunk)
    return h.hexdigest()


def md5_data(data: bytes) -> str:
    """计算内存数据的 MD5 哈希值（32 字符十六进制）"""
    return hashlib.md5(data).hexdigest()


# ═══════════════════════════════════════════════════════════════════════════════
# 时间与格式化
# ═══════════════════════════════════════════════════════════════════════════════

def get_timestamp_ms() -> int:
    """获取当前 UTC 时间的毫秒级时间戳"""
    return int(time.time() * 1000)


def get_time_string() -> str:
    """获取当前时间的可读字符串格式：YYYY-MM-DD HH:MM:SS"""
    return time.strftime("%Y-%m-%d %H:%M:%S", time.localtime())


def format_file_size(size_bytes: int) -> str:
    """将字节数格式化为人类可读的文件大小（B/KB/MB/GB/TB，1 位小数）"""
    if size_bytes < 1024:
        return f"{size_bytes}B"
    elif size_bytes < 1024 * 1024:
        return f"{size_bytes / 1024:.1f}KB"
    elif size_bytes < 1024 * 1024 * 1024:
        return f"{size_bytes / (1024 * 1024):.1f}MB"
    elif size_bytes < 1024 * 1024 * 1024 * 1024:
        return f"{size_bytes / (1024 * 1024 * 1024):.1f}GB"
    else:
        return f"{size_bytes / (1024 * 1024 * 1024 * 1024):.1f}TB"


def format_speed(bytes_per_sec: float) -> str:
    """将字节/秒格式化为可读传输速率（如 1.5MB/s）"""
    return format_file_size(int(bytes_per_sec)) + "/s"


def format_eta(total: int, transferred: int, speed: float) -> str:
    """根据总量、已传输量和速率计算预计剩余时间（ETA）

    Args:
        total: 总字节数
        transferred: 已传输字节数
        speed: 当前速率（bytes/s）

    Returns:
        可读字符串："30s", "5m 20s", "2h 10m" 等
    """
    if speed <= 0 or transferred >= total:
        return "0s"
    remaining = total - transferred
    seconds = int(remaining / speed)
    if seconds < 60:
        return f"{seconds}s"
    elif seconds < 3600:
        return f"{seconds // 60}m {seconds % 60}s"
    else:
        return f"{seconds // 3600}h {(seconds % 3600) // 60}m"


# ═══════════════════════════════════════════════════════════════════════════════
# 文件路径与编码解码
# ═══════════════════════════════════════════════════════════════════════════════

def get_filename(path: str) -> str:
    """从完整文件路径中提取文件名（去除目录前缀）"""
    return os.path.basename(path)


def get_file_size(path: str) -> int:
    """获取文件大小的字节数"""
    return os.path.getsize(path)


def b64_decode(s: str) -> bytes:
    """Base64 字符串解码为原始字节"""
    return base64.b64decode(s)


def b64_encode(data: bytes) -> str:
    """原始字节编码为 Base64 字符串"""
    return base64.b64encode(data).decode()


def url_decode(s: str) -> str:
    """URL 百分号编码（percent-encoding）解码

    处理规则：
    - '+' → 空格
    - '%XX' → 对应 ASCII 字符（十六进制）
    - 其他字符原样保留
    """
    result = []
    i = 0
    while i < len(s):
        c = s[i]
        if c == "+":
            result.append(" ")
        elif c == "%" and i + 2 < len(s):
            try:
                result.append(chr(int(s[i + 1 : i + 3], 16)))
                i += 2
            except ValueError:
                result.append(c)
        else:
            result.append(c)
        i += 1
    return "".join(result)


# ═══════════════════════════════════════════════════════════════════════════════
# AES-256-GCM 对称加密
# ═══════════════════════════════════════════════════════════════════════════════

def aes_gcm_encrypt(plaintext: bytes, key: bytes) -> bytes:
    """使用 AES-256-GCM 对明文进行认证加密

    GCM 模式同时提供机密性和完整性保护，每条消息使用随机 IV。

    Args:
        plaintext: 要加密的原始数据
        key: 32 字节 AES-256 密钥（由 ECDH 协商产生）

    Returns:
        有线格式字节串：[12 字节随机 IV][密文（与明文等长）][16 字节 GCM Tag]
    """
    iv = os.urandom(12)                     # 每消息随机 12 字节 nonce
    aesgcm = AESGCM(key)
    ct_with_tag = aesgcm.encrypt(iv, plaintext, None)  # 返回 ciphertext || tag
    return iv + ct_with_tag


def aes_gcm_decrypt(data: bytes, key: bytes) -> bytes | None:
    """使用 AES-256-GCM 对密文进行认证解密

    Args:
        data: 有线格式字节串 [12B IV][密文][16B GCM Tag]
        key: 32 字节 AES-256 密钥

    Returns:
        解密后的明文，若 GCM Tag 验证失败（数据被篡改或密钥不匹配）则返回 None
    """
    if len(data) < 28:                      # 最小有效长度 = 12(IV) + 0(空密文) + 16(Tag)
        return None
    iv = data[:12]                          # 提取前 12 字节为 IV
    ct_with_tag = data[12:]                 # 剩余部分为 密文 || GCM Tag
    aesgcm = AESGCM(key)
    try:
        return aesgcm.decrypt(iv, ct_with_tag, None)
    except Exception:
        return None


# ═══════════════════════════════════════════════════════════════════════════════
# ECDH 密钥协商（secp256r1 / NIST P-256）
# ═══════════════════════════════════════════════════════════════════════════════

def generate_ecdh_keypair() -> tuple[str, str]:
    """生成 ECDH 密钥对（secp256r1 曲线）

    公私钥以 DER SubjectPublicKeyInfo / PKCS8 格式编码后 Base64 输出，
    与 C++ 实现（OpenSSL EVP_PKEY_derive）有线兼容。

    Returns:
        (公钥 Base64 字符串, 私钥 Base64 字符串)
    """
    private_key = ec.generate_private_key(ec.SECP256R1())
    public_key = private_key.public_key()

    # 公钥输出为 DER SubjectPublicKeyInfo 格式
    public_der = public_key.public_bytes(
        serialization.Encoding.DER,
        serialization.PublicFormat.SubjectPublicKeyInfo,
    )
    # 私钥输出为 DER PKCS8 格式（无密码加密）
    private_der = private_key.private_bytes(
        serialization.Encoding.DER,
        serialization.PrivateFormat.PKCS8,
        serialization.NoEncryption(),
    )

    return b64_encode(public_der), b64_encode(private_der)


def compute_ecdh_shared(
    local_private_b64: str, remote_public_b64: str
) -> bytes | None:
    """计算 ECDH 共享密钥

    使用本地私钥和远端公钥执行 ECDH 密钥交换，输出 32 字节原始共享密钥，
    直接用作 AES-256 密钥（不进行哈希处理）。

    Args:
        local_private_b64: 本地 ECDH 私钥（Base64 编码的 DER PKCS8 格式）
        remote_public_b64: 远端 ECDH 公钥（Base64 编码的 DER SubjectPublicKeyInfo）

    Returns:
        32 字节共享密钥，失败返回 None
    """
    try:
        private_der = base64.b64decode(local_private_b64)
        public_der = base64.b64decode(remote_public_b64)

        private_key = serialization.load_der_private_key(private_der, password=None)
        peer_public_key = serialization.load_der_public_key(public_der)

        # cryptography 版本兼容处理：
        # 旧版需要 ec.ECDH() 算法参数，新版直接 exchange(peer_public_key)
        try:
            shared = private_key.exchange(ec.ECDH(), peer_public_key)
        except TypeError:
            shared = private_key.exchange(peer_public_key)

        return shared
    except Exception:
        return None


# ═══════════════════════════════════════════════════════════════════════════════
# zlib 压缩与解压
# ═══════════════════════════════════════════════════════════════════════════════

def compress_data(data: bytes) -> bytes:
    """使用 zlib 压缩数据（deflate 算法，默认压缩级别）"""
    return zlib.compress(data)


def decompress_data(data: bytes) -> bytes:
    """解压 zlib 压缩的数据"""
    return zlib.decompress(data)
