"""
设备管理模块

维护在线设备列表，提供线程安全的 CRUD 操作和自动超时清理。
与 C++ 版本的 DeviceManager 功能一致。

关键特性：
- 使用 threading.RLock() 可重入锁，支持在持有锁时调用其他需要加锁的方法
- 手动添加的设备（manual=True）永不过期超时
- known_devices.json 持久化仅保存 manual 设备，去重按 IP
- 设备上下线回调在锁外执行，避免死锁
"""

import threading
import time
from p2p_transfer.common.types import DeviceInfo, DEVICE_TIMEOUT, SIGNALING_PORT


class DeviceManager:
    """线程安全的设备列表管理器

    包含自动超时清理后台线程（每 1 秒检查一次）。
    支持设备列表的持久化到 known_devices.json。
    """

    def __init__(self):
        # 使用可重入锁 RLock，允许 load_known_devices() 在持有锁时调用
        # find_device_id_by_ip() 等也需要加锁的方法
        self._lock = threading.RLock()
        # device_id → DeviceInfo 映射
        self._devices: dict[str, DeviceInfo] = {}
        self._running = False
        # 后台清理线程
        self._thread: threading.Thread | None = None
        # 设备上下线回调函数
        self._on_online = None
        self._on_offline = None

    def set_on_device_online(self, callback):
        """设置设备上线回调 callback(DeviceInfo)"""
        self._on_online = callback

    def set_on_device_offline(self, callback):
        """设置设备离线回调 callback(DeviceInfo)"""
        self._on_offline = callback

    def start(self):
        """启动设备管理器及其后台清理线程"""
        self._running = True
        self._thread = threading.Thread(target=self._cleanup_loop, daemon=True)
        self._thread.start()

    def stop(self):
        """停止后台清理线程"""
        self._running = False
        if self._thread:
            self._thread.join(timeout=2)

    def add_device(self, info: DeviceInfo):
        """添加或更新设备信息

        如果是新设备（id 不在列表中），触发 on_online 回调。
        如果是已有设备，更新 ip/port/name/last_seen。
        回调在锁外执行以避免死锁。
        """
        with self._lock:
            existing = self._devices.get(info.id)
            if existing:
                # 更新已有设备信息
                existing.ip = info.ip
                existing.port = info.port
                existing.name = info.name
                existing.last_seen = time.time()
                existing.first_seen = min(existing.first_seen, existing.last_seen)
            else:
                # 新设备：设置时间戳并触发上线回调
                info.first_seen = time.time()
                info.last_seen = info.first_seen
                self._devices[info.id] = info
                cb = self._on_online
                if cb:
                    cb(info)

    def update_device(self, info: DeviceInfo):
        """更新设备信息的别名方法（与 add_device 等效）"""
        self.add_device(info)

    def remove_device(self, device_id: str) -> bool:
        """根据 device_id 移除设备并触发离线回调"""
        with self._lock:
            if device_id in self._devices:
                dev = self._devices.pop(device_id)
                cb = self._on_offline
                if cb:
                    cb(dev)
                return True
        return False

    def remove_by_ip(self, ip: str) -> bool:
        """根据 IP 地址移除设备（不触发回调，用于手动管理）"""
        with self._lock:
            for did, dev in list(self._devices.items()):
                if dev.ip == ip:
                    self._devices.pop(did)
                    return True
        return False

    def get_device(self, device_id: str) -> DeviceInfo | None:
        """根据 device_id 获取设备信息"""
        with self._lock:
            return self._devices.get(device_id)

    def get_all_devices(self) -> list[DeviceInfo]:
        """获取所有设备列表的副本"""
        with self._lock:
            return list(self._devices.values())

    def find_device_id_by_ip(self, ip: str) -> str:
        """根据 IP 查找 device_id（线性扫描 O(n)）"""
        with self._lock:
            for did, dev in self._devices.items():
                if dev.ip == ip:
                    return did
        return ""

    def find_device_by_ip(self, ip: str) -> DeviceInfo | None:
        """根据 IP 查找设备信息"""
        with self._lock:
            for dev in self._devices.values():
                if dev.ip == ip:
                    return dev
        return None

    def _cleanup_loop(self):
        """后台清理线程：每 1 秒检查并移除超时设备

        超时判定：last_seen < now - DEVICE_TIMEOUT (10 秒)
        manual 设备（manual=True）不会被超时清理。
        清理时触发离线回调（在锁外执行）。
        """
        while self._running:
            time.sleep(1)
            offline_devices = []
            cutoff = time.time() - DEVICE_TIMEOUT
            with self._lock:
                for did, dev in list(self._devices.items()):
                    # manual 设备永不过期
                    if not dev.manual and dev.last_seen < cutoff:
                        offline_devices.append(did)

            # 在锁外执行移除和回调，避免死锁
            for did in offline_devices:
                with self._lock:
                    dev = self._devices.pop(did, None)
                if dev and self._on_offline:
                    self._on_offline(dev)

    def save_known_devices(self, path: str, own_ips: list[str]):
        """将手动添加的设备列表保存到 known_devices.json

        保存规则：
        - 仅保存 manual=True 的设备
        - 按 IP 去重
        - 排除本机 IP
        - 输出格式为 JSON 数组
        """
        with self._lock:
            saved = []
            seen_ips = set()
            for dev in self._devices.values():
                if dev.manual and dev.ip not in own_ips and dev.ip not in seen_ips:
                    seen_ips.add(dev.ip)
                    saved.append(
                        {
                            "id": dev.id,
                            "name": dev.name,
                            "ip": dev.ip,
                            "port": dev.port,
                            "manual": True,
                        }
                    )
            import json

            with open(path, "w") as f:
                json.dump(saved, f, indent=2)

    def load_known_devices(self, path: str, own_ips: list[str]):
        """从 known_devices.json 加载手动添加的设备

        加载规则：
        - 跳过本机 IP
        - 按 id 和 IP 双重去重
        - 所有加载的设备标记为 manual=True
        - 文件不存在或 JSON 格式错误时静默跳过
        """
        import os
        import json

        if not os.path.exists(path):
            return
        try:
            with open(path, "r") as f:
                data = json.load(f)
            with self._lock:
                for entry in data:
                    ip = entry.get("ip", "")
                    if ip in own_ips:
                        continue
                    did = entry.get("id", "")
                    # 按 id 去重
                    if did in self._devices:
                        continue
                    # 按 IP 去重（使用内部方法，RLock 允许重入）
                    existing = self.find_device_id_by_ip(ip)
                    if existing:
                        continue
                    dev = DeviceInfo(
                        id=did,
                        name=entry.get("name", ""),
                        ip=ip,
                        port=entry.get("port", 0),
                        first_seen=time.time(),
                        last_seen=time.time(),
                        manual=True,                       # 加载的设备标记为手动
                    )
                    self._devices[did] = dev
        except (json.JSONDecodeError, IOError):
            pass
