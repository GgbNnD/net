"""
文件分块 I/O 模块

提供发送端和接收端的随机访问文件块读写功能。

发送端模式 (is_sender=True)：
- 以 O_RDONLY 打开源文件
- 使用 pread() 进行线程安全的随机读取（每个线程独立 fd）

接收端模式 (is_sender=False)：
- 以 O_WRONLY | O_CREAT 打开 .tmp 临时文件
- 使用 pwrite() 进行线程安全的随机写入（多连接 Handler 共享 fd）
- 维护 received_bitmap 位图跟踪已接收的分块
- commit_received_file() 关闭 fd 并 rename(.tmp → 最终文件名)
- cancel_receive() 关闭 fd 并 delete .tmp

重要约束（与 C++ 保持一致）：
- 接收端必须先 create + ftruncate(.tmp) 再构造 FileChunkIO，
  因为构造函数会从磁盘读取 .tmp 文件大小来确定已接收状态
"""

import os
import math
from p2p_transfer.common.types import CHUNK_SIZE


class FileChunkIO:
    """随机访问文件块读写器

    发送端使用 pread 随机读取，接收端使用 pwrite 随机写入。
    位图跟踪已接收块，支持最大连续块检测和完成判断。
    """

    def __init__(self, file_path: str, is_sender: bool, file_id: str = ""):
        """初始化文件块 I/O 对象

        发送端：打开源文件，计算总块数
        接收端：检测 .tmp 文件状态，标记已接收的块

        Args:
            file_path: 文件路径（发送端为源文件，接收端为最终目标路径）
            is_sender: True 为发送方，False 为接收方
            file_id: 传输 UUID（用于日志标识）
        """
        self._file_path = file_path
        self._is_sender = is_sender
        self._file_id = file_id
        self._fd = -1
        self._total_chunks = 0
        self._chunk_size = CHUNK_SIZE
        self._received_bitmap: list[bool] = []   # 接收端的块完成位图
        self._file_size = 0

        if is_sender:
            # 发送端：打开源文件，计算大小和总块数
            self._file_size = os.path.getsize(file_path)
            self._total_chunks = max(
                1, math.ceil(self._file_size / self._chunk_size)
            )
            self._fd = os.open(file_path, os.O_RDONLY)
        else:
            # 接收端：打开 .tmp 临时文件
            tmp_path = file_path + ".tmp"
            if os.path.exists(tmp_path):
                # 检测已有的 .tmp 文件（支持续传场景）
                self._file_size = os.path.getsize(tmp_path)
                self._total_chunks = max(
                    1, math.ceil(self._file_size / self._chunk_size)
                )
                self._received_bitmap = [False] * self._total_chunks

                # 根据已有 .tmp 文件大小标记已完成的块
                written_chunks = 0
                for i in range(self._total_chunks):
                    chunk_offset = i * self._chunk_size
                    if chunk_offset + self._chunk_size <= self._file_size:
                        written_chunks = i + 1
                    elif i == self._total_chunks - 1 and self._file_size > chunk_offset:
                        written_chunks = i + 1
                    elif self._file_size <= chunk_offset:
                        break
                for i in range(written_chunks):
                    if i < len(self._received_bitmap):
                        self._received_bitmap[i] = True

            self._fd = os.open(tmp_path, os.O_WRONLY | os.O_CREAT, 0o644)

    @property
    def total_chunks(self) -> int:
        """文件总分块数（至少为 1）"""
        return self._total_chunks

    @property
    def chunk_size(self) -> int:
        """每块大小（默认 65536 字节）"""
        return self._chunk_size

    @property
    def file_size(self) -> int:
        """文件总字节数"""
        return self._file_size

    @property
    def is_sender(self) -> bool:
        """是否为发送端"""
        return self._is_sender

    def set_total_size(self, total_size: int, total_chunks: int):
        """接收端：设置文件总大小和总块数，初始化位图

        在创建 .tmp 并 ftruncate 之后调用，用于告知预期大小。
        检查已存在的 .tmp 文件字节数来恢复已接收块标记。
        """
        self._file_size = total_size
        self._total_chunks = total_chunks
        self._received_bitmap = [False] * total_chunks

        # 根据 .tmp 已有大小恢复已接收标记（续传支持）
        for i in range(total_chunks):
            offset = i * self._chunk_size
            end = min(offset + self._chunk_size, self._file_size)
            if offset < end and os.path.getsize(self._file_path + ".tmp") > offset:
                self._received_bitmap[i] = True

    def read_chunk(self, chunk_index: int) -> bytes | None:
        """发送端：读取指定索引的数据块

        使用 pread() 进行不带文件位置更改的随机读取，支持多线程并发。
        最后一块可能小于 chunk_size。

        Args:
            chunk_index: 块索引（0 起始）

        Returns:
            数据块字节串，超出范围返回 None
        """
        if not self._is_sender:
            return None
        offset = chunk_index * self._chunk_size
        end = min(offset + self._chunk_size, self._file_size)
        if offset >= self._file_size:
            return None
        length = end - offset
        return os.pread(self._fd, length, offset)

    def write_chunk(self, chunk_index: int, data: bytes):
        """接收端：将数据写入指定块位置

        使用 pwrite() 进行无文件指针改动的随机写入，支持多 Handler 线程并发。
        写入后自动标记位图中对应位置为 True。
        """
        if self._is_sender:
            return
        offset = chunk_index * self._chunk_size
        os.pwrite(self._fd, data, offset)
        if chunk_index < len(self._received_bitmap):
            self._received_bitmap[chunk_index] = True

    def is_chunk_received(self, chunk_index: int) -> bool:
        """检查指定块是否已接收"""
        if chunk_index < len(self._received_bitmap):
            return self._received_bitmap[chunk_index]
        return False

    def get_max_contiguous_chunk(self) -> int:
        """获取从块 0 开始的最大连续已完成块索引（用于进度报告）"""
        for i in range(len(self._received_bitmap)):
            if not self._received_bitmap[i]:
                return i - 1 if i > 0 else -1
        return len(self._received_bitmap) - 1

    def all_chunks_received(self) -> bool:
        """检查是否所有块都已接收"""
        return all(self._received_bitmap)

    def received_count(self) -> int:
        """返回已接收块的总数"""
        return sum(1 for b in self._received_bitmap if b)

    def commit_received_file(self):
        """提交接收完成的文件：关闭 fd → rename(.tmp → 最终文件)"""
        if self._is_sender:
            return
        if self._fd >= 0:
            os.close(self._fd)
            self._fd = -1
        tmp_path = self._file_path + ".tmp"
        if os.path.exists(tmp_path):
            os.rename(tmp_path, self._file_path)

    def cancel_receive(self):
        """取消接收：关闭 fd → 删除 .tmp 临时文件"""
        if self._is_sender:
            return
        if self._fd >= 0:
            os.close(self._fd)
            self._fd = -1
        tmp_path = self._file_path + ".tmp"
        if os.path.exists(tmp_path):
            os.unlink(tmp_path)

    def close(self):
        """关闭文件描述符（发送端完成读取后调用）"""
        if self._fd >= 0:
            os.close(self._fd)
            self._fd = -1
