"""
文件传输任务管理器模块

提供线程安全的传输任务注册、查询、进度更新和完成标记。
维护传输历史记录供 Web UI 查询。

注意：pause / resume / cancel 目前仅修改状态枚举值，
不实际控制线程执行。线程控制需要额外实现。
"""

import threading
import time
from p2p_transfer.common.types import (
    TransferTask,
    TransferState,
    TransferRecord,
    CHUNK_SIZE,
)


class TransferManager:
    """传输任务管理器

    以 file_id 为键管理 TransferTask 字典。
    支持进度更新、完成标记和历史记录查询。
    """

    def __init__(self):
        self._lock = threading.Lock()
        # file_id → TransferTask 活动任务映射
        self._tasks: dict[str, TransferTask] = {}
        # 传输历史记录列表（已完成/失败/取消的任务）
        self._records: list[TransferRecord] = []
        self._on_complete = None               # 全局完成回调

    def set_on_complete(self, callback):
        """设置任务完成回调 callback(file_id, success)"""
        self._on_complete = callback

    def add_task(self, task: TransferTask):
        """注册一个新的传输任务"""
        with self._lock:
            self._tasks[task.meta.file_id] = task

    def get_task(self, file_id: str) -> TransferTask | None:
        """根据 file_id 获取传输任务"""
        with self._lock:
            return self._tasks.get(file_id)

    def get_all_tasks(self) -> list[TransferTask]:
        """获取所有活动任务列表的副本"""
        with self._lock:
            return list(self._tasks.values())

    def update_progress(self, file_id: str, progress_chunk: int, bytes_sent: int, speed: float):
        """更新任务的进度信息（分块数、字节数、速率）"""
        with self._lock:
            task = self._tasks.get(file_id)
            if task:
                task.progress_chunk = progress_chunk
                task.bytes_sent = bytes_sent
                task.speed = speed

    def mark_complete(self, file_id: str, success: bool):
        """标记任务为完成或失败，并记录到历史中

        触发全局完成回调（若设置）。
        """
        with self._lock:
            task = self._tasks.get(file_id)
            if task:
                task.state = TransferState.COMPLETED if success else TransferState.FAILED
                record = TransferRecord(
                    file_id=task.meta.file_id,
                    filename=task.meta.filename,
                    device_name=task.target.name,
                    file_size=task.meta.file_size,
                    is_sender=task.is_sender,
                    final_state=task.state.value,
                    timestamp=time.time(),
                )
                self._records.append(record)
                cb = self._on_complete
                if cb:
                    cb(file_id, success)
            else:
                cb = self._on_complete
                if cb:
                    cb(file_id, success)

    def pause_task(self, file_id: str):
        """标记任务为暂停状态（仅状态变化，不暂停线程）"""
        with self._lock:
            task = self._tasks.get(file_id)
            if task:
                task.state = TransferState.PAUSED

    def resume_task(self, file_id: str):
        """标记任务为恢复传输状态"""
        with self._lock:
            task = self._tasks.get(file_id)
            if task:
                task.state = TransferState.TRANSFERRING

    def cancel_task(self, file_id: str):
        """标记任务为已取消并记录到历史

        仅修改状态枚举值，不实际终止线程。
        """
        with self._lock:
            task = self._tasks.get(file_id)
            if task:
                task.state = TransferState.CANCELLED
                record = TransferRecord(
                    file_id=task.meta.file_id,
                    filename=task.meta.filename,
                    device_name=task.target.name,
                    file_size=task.meta.file_size,
                    is_sender=task.is_sender,
                    final_state=TransferState.CANCELLED.value,
                    timestamp=time.time(),
                )
                self._records.append(record)

    def get_resume_chunk(self, filename: str, file_size: int) -> int:
        """续传支持：根据已有的 .tmp 文件大小计算续传起始块索引

        Args:
            filename: 文件路径（不含 .tmp 后缀）
            file_size: 预期文件大小

        Returns:
            应从该块开始续传
        """
        import os

        tmp_path = filename + ".tmp"
        if os.path.exists(tmp_path):
            existing = os.path.getsize(tmp_path)
            return existing // CHUNK_SIZE
        return 0

    def get_records(self) -> list[TransferRecord]:
        """获取传输历史记录的副本"""
        with self._lock:
            return list(self._records)
