"""
二进制文件分块序列化/反序列化模块

定义文件传输的数据分块二进制格式，与 C++ 实现有线兼容。

有线格式（网络字节序大端）：
┌─────────────┬──────────────┬───────────┬────────────┬──────────┬──────────┐
│ chunk_index │ total_chunks │ data_size │ file_id_len│ file_id  │ data     │
│  uint32 BE  │  uint32 BE   │ uint32 BE │  uint32 BE │ UTF-8    │ binary   │
│  4 bytes    │  4 bytes     │  4 bytes  │  4 bytes   │ variable │ variable │
└─────────────┴──────────────┴───────────┴────────────┴──────────┴──────────┘
                    ←──── 16 字节固定头部 (CHUNK_HEADER_SIZE) ────→

data_size 上限为 65536 (MAX_CHUNK_DATA_SIZE)
file_id_len 上限为 256
"""

import struct
from p2p_transfer.common.types import CHUNK_SIZE

# 单块数据载荷最大 64KB
MAX_CHUNK_DATA_SIZE = CHUNK_SIZE
# 固定头部 4 个 uint32 = 16 字节
CHUNK_HEADER_SIZE = 16
# 哨兵值，表示传输结束（与 C++ 0xFFFFFFFF 对应）
CHUNK_SENTINEL_INDEX = 0xFFFFFFFF


def serialize_chunk(chunk_index: int, total_chunks: int, file_id: str, data: bytes) -> bytes:
    """将分块数据序列化为有线格式字节串

    Args:
        chunk_index: 当前分块索引（从 0 开始）
        total_chunks: 文件总分块数
        file_id: 文件传输 UUID 标识
        data: 分块数据载荷（最大 65536 字节）

    Returns:
        完整的序列化分块字节串
    """
    file_id_bytes = file_id.encode("utf-8")
    # 构造 16 字节大端固定头
    header = struct.pack(
        "!IIII",
        chunk_index,           # [0-3] 当前块索引
        total_chunks,          # [4-7] 总块数
        len(data),             # [8-11] 数据长度
        len(file_id_bytes),    # [12-15] 文件 ID 长度
    )
    return header + file_id_bytes + data


def deserialize_chunk(raw: bytes) -> tuple[int, int, str, bytes] | None:
    """将有线格式字节串反序列化为分块数据

    Args:
        raw: 完整的序列化分块字节串 [16B 头部][file_id][数据]

    Returns:
        (chunk_index, total_chunks, file_id, data) 或 None（数据不完整）
    """
    if len(raw) < CHUNK_HEADER_SIZE:
        return None

    # 解析大端固定头
    chunk_index, total_chunks, data_size, file_id_len = struct.unpack("!IIII", raw[:16])

    # 完整性校验：数据长度必须与头声明一致
    if len(raw) < CHUNK_HEADER_SIZE + file_id_len + data_size:
        return None

    # 提取 file_id 字符串
    file_id = raw[16 : 16 + file_id_len].decode("utf-8", errors="replace")
    # 提取数据载荷
    data = raw[16 + file_id_len : 16 + file_id_len + data_size]

    return chunk_index, total_chunks, file_id, data
