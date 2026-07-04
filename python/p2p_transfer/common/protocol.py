"""
P2P 有线传输协议模块

实现跨平台的 TCP 数据流协议封装，包括：
- 可靠的数据收发（recv_exact / send_all）
- JSON 消息序列化与长度前缀帧格式
- AES-256-GCM 透明加密层（自动查询 PeerKey）
- 传输通道的 1 字节类型标记协议
- 二进制分块数据的收发
- 各协议消息的构建函数（build_* 系列）

╔══════════════════════════════════════════════════════════════════╗
║ 有线格式规范（与 C++ 实现兼容）                                  ║
╠══════════════════════════════════════════════════════════════════╣
║ 信令通道（端口 8889，无标记字节）：                              ║
║   明文 JSON：  [4 字节 BE 长度][JSON UTF-8]                      ║
║   加密 JSON：  [4 字节 BE 总长][12B IV][密文][16B GCM Tag]       ║
║                                                                  ║
║ 数据传输通道（端口 8890，有 1 字节类型标记）：                   ║
║   'J' 明文 JSON：  标记 + [4B BE 长度][JSON]                     ║
║   'E' 加密 JSON：  标记 + [4B BE 总长][12B IV][密文][16B Tag]    ║
║   'C' 明文分块：   标记 + [16B 头][file_id][数据]                ║
║   'D' 加密分块：   标记 + [4B BE 总长][12B IV][加密chunk][16B Tag]║
║                                                                  ║
║ 分块二进制格式（网络字节序大端）：                               ║
║   [0-3]   chunk_index    (uint32 BE)                             ║
║   [4-7]   total_chunks   (uint32 BE)                             ║
║   [8-11]  data_size      (uint32 BE)                             ║
║   [12-15] file_id_len    (uint32 BE)                             ║
║   [16..]  file_id        (UTF-8 可变长)                          ║
║   [...]   数据载荷       (最大 65536 字节)                       ║
╚══════════════════════════════════════════════════════════════════╝
"""

import socket
import struct
import json
import time
from .types import MAX_MSG_SIZE, MAX_DATA_SIZE, SIGNALING_PORT
from .peer_key import PeerKey
from .utils import aes_gcm_encrypt, aes_gcm_decrypt

# ─── 传输通道 1 字节类型标记 ─────────────────────────────────────────────────
# JSON 明文消息标记
MARKER_JSON = ord("J")
# JSON 加密消息标记
MARKER_ENCRYPTED_JSON = ord("E")
# 二进制明文分块标记
MARKER_CHUNK = ord("C")
# 二进制加密分块标记
MARKER_ENCRYPTED_CHUNK = ord("D")


# ═══════════════════════════════════════════════════════════════════════════════
# 底层可靠收发函数
# ═══════════════════════════════════════════════════════════════════════════════

def recv_exact(sock: socket.socket, n: int) -> bytes:
    """从套接字精确读取 n 字节

    由于 TCP 是流式协议，单次 recv() 返回值可能少于请求量。
    此函数循环读取直到凑满 n 字节或发生错误。

    异常处理：
    - socket.timeout / BlockingIOError → 抛出 ConnectionError（不重试，避免死循环）
    - InterruptedError → 继续循环（被信号中断可恢复）
    - recv 返回 0 → 对端已关闭连接
    """
    data = b""
    while len(data) < n:
        try:
            chunk = sock.recv(n - len(data))
        except InterruptedError:
            continue                        # 被系统信号中断，可安全重试
        except socket.timeout:
            raise ConnectionError("recv timeout")  # 超时不可恢复
        except BlockingIOError:
            raise ConnectionError("recv would block")
        if not chunk:
            raise ConnectionError("Connection closed")  # 对端正常关闭
        data += chunk
    return data


def send_all(sock: socket.socket, data: bytes):
    """将全部数据写入套接字

    单次 send() 可能只发送部分数据，此函数循环直到全部发出。
    BlockingIOError / InterruptedError 为可恢复的错误，继续重试。
    """
    sent = 0
    while sent < len(data):
        try:
            n = sock.send(data[sent:])
        except (BlockingIOError, InterruptedError):
            continue                        # 可恢复，稍后重试
        if n < 0:
            raise ConnectionError("Send failed")
        sent += n


# ═══════════════════════════════════════════════════════════════════════════════
# 信令通道 JSON 消息收发（端口 8889，无标记字节）
# ═══════════════════════════════════════════════════════════════════════════════

def send_json_message_raw(sock: socket.socket, msg: dict):
    """以明文 JSON 格式发送一条协议消息（信令通道）

    有线格式：[4 字节大端长度][JSON UTF-8 字节串]
    """
    body = json.dumps(msg, ensure_ascii=False).encode("utf-8")
    length = struct.pack("!I", len(body))
    send_all(sock, length + body)


def recv_json_message_raw(sock: socket.socket) -> dict | None:
    """以明文 JSON 格式接收一条协议消息（信令通道）

    先读取 4 字节大端长度前缀，再按长度读取 JSON 正文并解析。
    若 JSON 非法或长度超过 10MB 上限，返回 None。
    """
    try:
        len_bytes = recv_exact(sock, 4)
    except ConnectionError:
        return None
    body_len = struct.unpack("!I", len_bytes)[0]
    if body_len > MAX_MSG_SIZE:
        return None
    try:
        body = recv_exact(sock, body_len)
    except ConnectionError:
        return None
    try:
        return json.loads(body.decode("utf-8"))
    except (json.JSONDecodeError, UnicodeDecodeError):
        return None


def send_encrypted_raw(sock: socket.socket, plaintext: bytes, secret: bytes):
    """以 AES-256-GCM 加密方式发送原始数据（信令通道）

    有线格式：[4B BE 加密数据总长度][12B IV][密文][16B GCM Tag]
    """
    encrypted = aes_gcm_encrypt(plaintext, secret)
    length = struct.pack("!I", len(encrypted))
    send_all(sock, length + encrypted)


def recv_encrypted_raw(sock: socket.socket, secret: bytes) -> bytes | None:
    """以 AES-256-GCM 加密方式接收原始数据（信令通道）

    先读取 4 字节长度前缀获取加密数据总长度，读取后解密。
    GCM Tag 验证失败或长度超限返回 None。
    """
    try:
        len_bytes = recv_exact(sock, 4)
    except ConnectionError:
        return None
    total_len = struct.unpack("!I", len_bytes)[0]
    if total_len > MAX_MSG_SIZE + 28:
        return None
    try:
        data = recv_exact(sock, total_len)
    except ConnectionError:
        return None
    return aes_gcm_decrypt(data, secret)


def send_json_message(sock: socket.socket, msg: dict, peer_ip: str):
    """智能发送 JSON 协议消息（信令通道）

    自动查询 PeerKey：若已有对端 ECDH 密钥，则加密发送；
    否则以明文 JSON 格式发送。
    此为信令通道的主要发送入口。
    """
    body = json.dumps(msg, ensure_ascii=False).encode("utf-8")
    key = PeerKey.get(peer_ip)
    if key:
        send_encrypted_raw(sock, body, key)
    else:
        send_json_message_raw(sock, msg)


def recv_json_message(sock: socket.socket, peer_ip: str = "") -> dict | None:
    """智能接收 JSON 协议消息（信令通道）

    同时支持明文和加密两种格式的向后兼容接收：
    1. 读取 4 字节长度前缀 + 指定长度数据
    2. 若有对端密钥，优先尝试 AES-GCM 解密
    3. 解密成功 → 解析明文 JSON 并返回
    4. 解密失败或无密钥 → 将原始数据尝试作为明文 JSON 解析

    此设计允许从未加密到加密的平滑过渡。
    """
    key = PeerKey.get(peer_ip) if peer_ip else None

    # 第一步：读取可变长度帧
    try:
        len_bytes = recv_exact(sock, 4)
    except ConnectionError:
        return None
    total_len = struct.unpack("!I", len_bytes)[0]
    if total_len > MAX_MSG_SIZE + 28:
        return None
    try:
        raw_data = recv_exact(sock, total_len)
    except ConnectionError:
        return None

    # 第二步：优先尝试加密解密
    if key:
        plaintext = aes_gcm_decrypt(raw_data, key)
        if plaintext is not None:
            try:
                return json.loads(plaintext.decode("utf-8"))
            except (json.JSONDecodeError, UnicodeDecodeError):
                return None

    # 第三步：解密失败/无密钥 → 明文 JSON 回退
    try:
        return json.loads(raw_data.decode("utf-8"))
    except (json.JSONDecodeError, UnicodeDecodeError):
        return None


# ═══════════════════════════════════════════════════════════════════════════════
# 传输通道消息收发（端口 8890，带 1 字节类型标记）
# ═══════════════════════════════════════════════════════════════════════════════

def send_json_with_marker(sock: socket.socket, msg: dict, peer_ip: str):
    """在传输通道上发送 JSON 消息（带类型标记）

    明文：'J' + [4B BE 长度][JSON]
    加密：'E' + [4B BE 加密总长][12B IV][密文][16B GCM Tag]
    """
    body = json.dumps(msg, ensure_ascii=False).encode("utf-8")
    key = PeerKey.get(peer_ip)
    if key:
        sock.sendall(bytes([MARKER_ENCRYPTED_JSON]))
        encrypted = aes_gcm_encrypt(body, key)
        length = struct.pack("!I", len(encrypted))
        send_all(sock, length + encrypted)
    else:
        sock.sendall(bytes([MARKER_JSON]))
        length = struct.pack("!I", len(body))
        send_all(sock, length + body)


def recv_json_any(sock: socket.socket, peer_ip: str = "") -> dict | None:
    """在传输通道上接收 JSON 消息（自动判别明文/加密）

    读取 1 字节标记，根据标记类型选择解密或直接解析。
    非 JSON 标记返回 None。
    """
    try:
        marker_byte = recv_exact(sock, 1)
        marker = marker_byte[0]
    except ConnectionError:
        return None

    if marker in (MARKER_JSON, MARKER_ENCRYPTED_JSON):
        if marker == MARKER_ENCRYPTED_JSON:
            key = PeerKey.get(peer_ip) if peer_ip else None
            if not key:
                return None
            try:
                len_bytes = recv_exact(sock, 4)
            except ConnectionError:
                return None
            total_len = struct.unpack("!I", len_bytes)[0]
            if total_len > MAX_MSG_SIZE + 28:
                return None
            try:
                raw_data = recv_exact(sock, total_len)
            except ConnectionError:
                return None
            plaintext = aes_gcm_decrypt(raw_data, key)
            if plaintext is None:
                return None
            try:
                return json.loads(plaintext.decode("utf-8"))
            except (json.JSONDecodeError, UnicodeDecodeError):
                return None
        else:
            # 明文 JSON 标记 'J'
            try:
                len_bytes = recv_exact(sock, 4)
            except ConnectionError:
                return None
            body_len = struct.unpack("!I", len_bytes)[0]
            if body_len > MAX_MSG_SIZE:
                return None
            try:
                body = recv_exact(sock, body_len)
            except ConnectionError:
                return None
            try:
                return json.loads(body.decode("utf-8"))
            except (json.JSONDecodeError, UnicodeDecodeError):
                return None

    return None


def send_chunk_data(sock: socket.socket, chunk_bytes: bytes, peer_ip: str):
    """在传输通道上发送二进制分块数据（带类型标记）

    明文分块：'C' + [完整序列化分块字节]
    加密分块：'D' + [4B BE 加密总长][12B IV][加密分块][16B GCM Tag]
    """
    key = PeerKey.get(peer_ip)
    if key:
        sock.sendall(bytes([MARKER_ENCRYPTED_CHUNK]))
        encrypted = aes_gcm_encrypt(chunk_bytes, key)
        length = struct.pack("!I", len(encrypted))
        send_all(sock, length + encrypted)
    else:
        sock.sendall(bytes([MARKER_CHUNK]))
        send_all(sock, chunk_bytes)


# 分块二进制头常量
CHUNK_HEADER_SIZE = 16     # [chunk_index][total_chunks][data_size][file_id_len]
MAX_CHUNK_DATA_SIZE = 65536  # 单块数据载荷最大 64KB


def recv_chunk_data(sock: socket.socket, peer_ip: str = "") -> bytes | None:
    """在传输通道上接收二进制分块数据（自动判别明文/加密）

    读取 1 字节标记：
    - 'D' 加密分块：读取 4B 长度 → 读取加密数据 → AES-GCM 解密 → 返回序列化分块
    - 'C' 明文分块：读取 16B 头部 → 解析 data_size/file_id_len → 读取剩余 → 返回完整分块
    - 'J'/'E' JSON 标记：返回特殊哨兵 b"__JSON_SENTINEL__"（表示控制消息到来）

    Returns:
        完整序列化分块字节串、哨兵标记或 None（连接错误）
    """
    try:
        marker_byte = recv_exact(sock, 1)
        marker = marker_byte[0]
    except ConnectionError:
        return None

    if marker == MARKER_ENCRYPTED_CHUNK:
        # 加密分块：长度前缀 + IV + 密文 + GCM Tag
        key = PeerKey.get(peer_ip) if peer_ip else None
        if not key:
            return None
        try:
            len_bytes = recv_exact(sock, 4)
        except ConnectionError:
            return None
        total_len = struct.unpack("!I", len_bytes)[0]
        if total_len > MAX_DATA_SIZE:
            return None
        try:
            raw_data = recv_exact(sock, total_len)
        except ConnectionError:
            return None
        return aes_gcm_decrypt(raw_data, key)

    elif marker == MARKER_CHUNK:
        # 明文分块：自描述头部 + file_id + 数据
        try:
            header = recv_exact(sock, CHUNK_HEADER_SIZE)
        except ConnectionError:
            return None
        # 解析头部获取 file_id 和数据长度
        _, _, data_size, file_id_len = struct.unpack("!IIII", header)
        if data_size > MAX_CHUNK_DATA_SIZE or file_id_len > 256:
            return None
        try:
            body = recv_exact(sock, file_id_len + data_size)
        except ConnectionError:
            return None
        return header + body

    elif marker in (MARKER_JSON, MARKER_ENCRYPTED_JSON):
        # JSON 控制消息（如 RANGE_DONE），通知调用方切换到控制消息处理
        return b"__JSON_SENTINEL__"

    return None


# ═══════════════════════════════════════════════════════════════════════════════
# JSON 协议消息构建函数（build_* 系列）
# ═══════════════════════════════════════════════════════════════════════════════

def build_device_broadcast(
    device_id: str, name: str, ip: str, port: int, ts: float
) -> dict:
    """构建设备发现广播消息 DEVICE_BROADCAST

    由 DeviceDiscovery 发送线程周期性地通过 UDP 组播发出。
    注意：port 字段填写的是信令端口（8889），而非发现端口（8888）。
    """
    return {
        "type": "DEVICE_BROADCAST",
        "device_id": device_id,
        "device_name": name,
        "ip": ip,
        "port": port,
        "timestamp": ts,
    }


def build_device_offline(device_id: str) -> dict:
    """构建设备离线通知消息 DEVICE_OFFLINE

    在服务停止时通过 UDP 组播发出 3 次（best-effort）。
    """
    return {"type": "DEVICE_OFFLINE", "device_id": device_id}


def build_device_hello(
    device_id: str, name: str, ip: str, port: int, public_key: str = ""
) -> dict:
    """构建 ECDH 握手探测消息 DEVICE_HELLO

    public_key 为发送方的 ECDH 公钥（Base64 编码 DER SubjectPublicKeyInfo），
    若为空串则不进行密钥交换。
    """
    return {
        "type": "DEVICE_HELLO",
        "device_id": device_id,
        "device_name": name,
        "ip": ip,
        "port": port,
        "public_key": public_key,
    }


def build_device_hello_ack(public_key: str = "") -> dict:
    """构建 ECDH 握手应答消息 DEVICE_HELLO_ACK

    携带服务端的 ECDH 公钥。
    """
    return {"type": "DEVICE_HELLO_ACK", "public_key": public_key}


def build_text_message(text: str) -> dict:
    """构建文本聊天消息 TEXT_MESSAGE"""
    return {"type": "TEXT_MESSAGE", "text": text}


def build_file_request(
    file_id: str,
    filename: str,
    file_size: int,
    checksum: str,
    chunk_size: int,
    total_chunks: int,
) -> dict:
    """构建文件传输请求消息 FILE_REQUEST

    固定设置 compression 为 "zlib"，接收方据此决定是否需要解压。
    """
    return {
        "type": "FILE_REQUEST",
        "file_id": file_id,
        "filename": filename,
        "file_size": file_size,
        "checksum": checksum,
        "chunk_size": chunk_size,
        "total_chunks": total_chunks,
        "compression": "zlib",
    }


def build_file_response(
    file_id: str, status: str, resume_from_chunk: int = 0, reason: str = ""
) -> dict:
    """构建文件传输应答消息 FILE_RESPONSE

    status 取值：ACCEPT / REJECT / PAUSE
    resume_from_chunk 用于断点续传，指定从哪个分块继续。
    """
    msg = {
        "type": "FILE_RESPONSE",
        "file_id": file_id,
        "status": status,
        "resume_from_chunk": resume_from_chunk,
    }
    if reason:
        msg["reason"] = reason
    return msg


def build_chunk_ack(file_id: str, max_contiguous_chunk: int) -> dict:
    """构建分块确认消息 CHUNK_ACK（旧版单连接协议）"""
    return {
        "type": "CHUNK_ACK",
        "file_id": file_id,
        "max_contiguous_chunk": max_contiguous_chunk,
    }


def build_control_message(
    msg_type: str, file_id: str, extra: str = ""
) -> dict:
    """构建通用控制消息（TRANSFER_CANCEL / PAUSE / RESUME 等）"""
    msg = {"type": msg_type, "file_id": file_id}
    if extra:
        msg["extra"] = extra
    return msg


def build_file_range(
    file_id: str,
    filename: str,
    file_size: int,
    total_chunks: int,
    chunk_size: int,
    start_chunk: int,
    end_chunk: int,
    conn_index: int,
    total_connections: int,
    compression: str = "zlib",
) -> dict:
    """构建多连接文件传输范围声明消息 FILE_RANGE

    每个发送线程在建立 TCP 连接后，首先发送此消息声明自己负责的分块范围。
    conn_index 和 total_connections 告知接收方当前连接的编号和总连接数，
    接收方据此跟踪所有连接的完成状态。
    """
    return {
        "type": "FILE_RANGE",
        "file_id": file_id,
        "filename": filename,
        "file_size": file_size,
        "total_chunks": total_chunks,
        "chunk_size": chunk_size,
        "start_chunk": start_chunk,
        "end_chunk": end_chunk,
        "conn_index": conn_index,
        "total_connections": total_connections,
        "compression": compression,
    }


def build_range_done(file_id: str, conn_index: int) -> dict:
    """构建分块范围发送完成消息 RANGE_DONE"""
    return {
        "type": "RANGE_DONE",
        "file_id": file_id,
        "conn_index": conn_index,
    }


def build_range_ack(file_id: str, conn_index: int) -> dict:
    """构建分块范围接收确认消息 RANGE_ACK"""
    return {
        "type": "RANGE_ACK",
        "file_id": file_id,
        "conn_index": conn_index,
    }
