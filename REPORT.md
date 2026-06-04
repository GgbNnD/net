# P2P 局域网文件传输工具 — 技术报告

## 项目概述

本项目是一个跨平台（Linux/Windows/macOS）的局域网 P2P 文件传输工具，采用 C++17 实现，前端为微信风格的聊天 Web UI。核心功能包括：UDP 组播自动发现、TCP 滑动窗口文件传输、MD5 完整性校验、文本聊天、设备持久化。

**技术栈**: C++17 + CMake + nlohmann/json + HTML5/CSS/JS（无框架）

---

## 一、系统架构

### 1.1 四层架构

```
┌──────────────────────────────────────────────────────────────┐
│                        Web UI (8891)                         │
│   HTTP Server + REST API + 微信风格 Chat UI                   │
├──────────────────────────────────────────────────────────────┤
│   Discovery (8888)  │  Signaling (8889)  │  Transfer (8890) │
│   UDP 组播自动发现   │  TCP 信令协商       │  TCP 滑动窗口     │
│   + TCP DEVICE_HELLO │  + 文本聊天         │  + MD5 校验      │
├──────────────────────────────────────────────────────────────┤
│                      Common Layer                            │
│   跨平台 Socket 封装 │ JSON 协议 │ UUID/MD5/Base64 工具      │
└──────────────────────────────────────────────────────────────┘
```

### 1.2 线程模型

每个服务层运行在独立线程中，通过回调函数解耦：

| 线程 | 职责 |
|------|------|
| `DeviceDiscovery::m_send_thread` | 每 3 秒组播 DEVICE_BROADCAST |
| `DeviceDiscovery::m_recv_thread` | 阻塞接收组播消息 |
| `SignalingServer::m_accept_thread` | 接受 TCP 连接（select 轮询） |
| `TransferReceiver::m_accept_thread` | 接受文件传输连接 |
| `HttpServer::m_accept_thread` | HTTP 请求处理 |
| `g_peer_probe_thread` | 每 5 秒 TCP 探活手动设备 |
| `DeviceManager::m_cleanup_thread` | 每 1 秒超时清理 |
| `g_transfer_threads` (动态) | 每个文件传输一个发送线程 |

### 1.3 协议设计

所有消息使用 JSON 格式。数据通道（8890）使用 1 字节类型标记区分 JSON 和二进制分片：

```cpp
if (marker == 'J') { /* JSON 消息: 文件头 / ACK */ }
if (marker == 'C') { /* 二进制分片: 16 字节头 + 变长数据 */ }
```

信令通道（8889）为纯 JSON 短连接，每个消息带 4 字节大端长度前缀：

```
[4-byte BE length] + [JSON string]
```

**分片头部设计（16 字节）**:

| 偏移 | 大小 | 字段 | 说明 |
|------|------|------|------|
| 0 | 4 | chunk_index | 分片编号（从 0 开始） |
| 4 | 4 | total_chunks | 总分片数 |
| 8 | 4 | data_size | 有效数据长度 |
| 12 | 4 | file_id_len | UUID 长度 |
| 16 | 变长 | file_id | UUID 字符串 |
| 16+N | 变长 | data | 分片数据（最多 64KB） |

---

## 二、核心算法与数据结构

### 2.1 滑动窗口协议

**参数**:
- 窗口大小: 16（最多 16 个未确认分片）
- 分片大小: 65536 字节（64KB）
- ACK 超时: 3000ms

**发送流程**:
```
1. 发送文件头（JSON）
2. for chunk_idx in 0..total_chunks:
     a. 等待窗口有空位（acked + window > chunk_idx）
     b. read_chunk(chunk_idx) → pread 读取磁盘
     c. send_chunk(sock, chunk) → TCP 发送
     d. try_recv_ack() → 非阻塞接收 ACK，更新已确认位置
     e. 如果 chunk_idx 已超时 → 重传未确认分片
3. 等待最终 ACK（最多 5 个超时周期 ≈ 10s）
4. 完成
```

**接收流程**:
```
1. recv_file_header() → 解析 FileMeta
2. open + ftruncate(.tmp 文件) → 预分配文件空间
3. while chunk_count < total_chunks:
     a. recv_chunk() → 接收分片（处理 'J'/'C' 标记）
     b. write_chunk() → pwrite 写入 .tmp 文件
     c. send_ack() → 发送当前最大连续分片号
4. commit_received_file() → rename(.tmp → 最终文件)
5. MD5 校验 → 成功/失败回调
```

### 2.2 FileChunkIO 分片 I/O

使用 POSIX `pread`/`pwrite` 实现随机位置的分片读写：

```cpp
// 发送方: 读取分片
actual_size = (chunk_idx == last) ? file_size - offset : chunk_size;
pread(m_fd, data, actual_size, offset);

// 接收方: 写入分片
pwrite(m_fd, data, data_size, offset);
```

**断点续传支持**:
- `m_received_bitmap` 位图记录已接收的分片
- 发送方通过 `FILE_REQUEST` 获取 `resume_from_chunk`
- 临时文件用于流式写入，完成后 rename 为正式文件

### 2.3 设备发现机制

采用 UDP 组播 + TCP 握手双层发现：

**第一层: UDP 组播**
- 组播地址: `239.255.255.250:8888`
- 每 3 秒广播 `DEVICE_BROADCAST`
- 加入所有非回环接口的组播组
- TTL=4（跨 /24 子网）

**第二层: TCP DEVICE_HELLO 握手**
- 手动添加的设备每 5 秒进行 TCP 探活
- 探活时发送 `DEVICE_HELLO` JSON 消息（含本机 device_id, name, ip, port）
- 接收方自动将发送方加入设备列表（IP 去重）
- 实现双向发现：A 添加 B 后，B 的探活将 A 反向加入 B 的列表

### 2.4 设备持久化

设备列表持久化到 `known_devices.json`：

```json
[
  {"id": "uuid", "name": "设备名", "ip": "10.162.181.2", "port": 8889}
]
```

**关键逻辑**:
- 加载时跳过本机 IP、按 IP 去重
- 保存时同样去重，清理历史重复条目
- 手动添加、DEVICE_HELLO 触发时自动保存

---

## 三、项目迭代过程

### 第一阶段：基础设施搭建（Commit 1-5）

- 初始化 CMake 项目、目录结构
- 引入 nlohmann/json header-only 库
- 实现跨平台 Socket 封装（`platform.h/cpp`）
- 定义核心数据类型（`DeviceInfo`, `FileMeta`, `TransferTask`）

### 第二阶段：核心模块开发（Commit 6-9）

**设备发现模块** (`discovery/`):
- `DeviceDiscovery`: UDP 组播收发，广播间隔 3 秒
- `DeviceManager`: 线程安全设备映射，10 秒超时清理，上线/离线回调

**信令控制模块** (`signaling/`):
- `SignalingServer`: TCP 监听 8889，处理 `FILE_REQUEST`/`FILE_RESPONSE`
- `SignalingClient`: 短连接请求-响应，非阻塞 connect + select 超时

**文件传输模块** (`transfer/`):
- `TransferSender`: 滑动窗口发送，64KB 分片，ACK 重传
- `TransferReceiver`: 分片接收，`pwrite` 写入，MD5 校验
- `FileChunkIO`: 随机位置分片读写，位图断点续传
- `Chunk`: 二进制序列化/反序列化（16 字节头 + 变长体）

**Web 模块** (`web/`):
- `HttpServer`: 最小 HTTP 服务器，GET/POST 路由，CORS，静态文件服务
- REST API: `/api/devices`, `/api/transfers`, `/api/transfer`
- 前端: 基础设备列表 + 文件发送 UI

### 第三阶段：发现机制修复与增强（Commit 10-16）

**关键 Bug 修复**:

1. **组播 IP 选择错误** (`7d8dfea`):
   - 原 `select_local_ip()` 取 `getifaddrs()` 第一个 IP，Docker/VPN 虚拟接口 IP 会误选
   - 改用 UDP connect trick：`connect(1.1.1.1:53)` + `getsockname()` 获取主网卡 IP
   - 在所有非回环接口上调用 `IP_ADD_MEMBERSHIP` 加入组播组

2. **文件传输从未启动** (`a8f6ba0`):
   - `POST /api/transfer` 只创建 TransferTask 记录，从未调用 `TransferSender::send_file()`
   - 新增 base64 文件上传 + 后台线程启动实际传输

3. **手动设备自动超时离线** (`a8d8ea3`):
   - `DeviceInfo` 新增 `manual` 标志，手动设备跳过 `cleanup_loop()` 超时清理
   - 新增 TCP 探活线程：每 5 秒 `test_connect()` 检查手动设备可达性

4. **TCP 探活日志噪音** (`e367b39`):
   - 探活连接不发数据即关闭，信令服务器打印"接收消息失败"
   - 改为静默关闭空连接

### 第四阶段：扫描方案尝试与放弃（Commit 17-19）

尝试了 TCP 子网扫描替代组播发现：
- 多线程并行扫描（8 线程 × 32 并发 × 200ms 超时）
- 稀疏扫描策略（每 /24 只探 .1 和 .2）

**投入生产后的问题**:
- /16 子网扫描耗时长（单线程 ~50 秒）
- IP 字节序转换错误（`ntohl`/`htonl` 使用位置不当）
- 最终发现组播失败的根本原因是**缺少组播路由**（`ip route add 224.0.0.0/4 dev <iface>`）

**结论**: 移除 TCP 扫描代码，回归纯组播发现。TTL 从 1 提高到 4，解决跨 /24 子网路由问题。

### 第五阶段：功能完善（Commit 20-28）

**互相发现机制** (`fef5f91`):
- 新增 `DEVICE_HELLO` 协议消息
- `test_connect()` 探活时发送本机信息
- 信令服务器新增 `m_device_hello_cb` 回调
- 优雅关闭确保消息送达：`shutdown(SHUT_WR)` + 排空接收缓冲区

**设备持久化**:
- `known_devices.json` 加载/保存
- IP 去重 + 跳过本机 IP
- 保存时自动清理历史重复条目

**微信风格 UI** (`22225d3`):
- 左侧设备列表（头像 + 在线/离线状态灯）
- 右侧聊天区域（发送蓝色气泡、接收灰色气泡）
- 底部输入栏（文字 + 文件按钮）
- 新增 `TEXT_MESSAGE` 协议支持文字聊天
- 自动打开浏览器

**传输体验优化**:
- 接收侧 `set_on_receive_start` 回调创建 TransferTask
- 前端进度条 + 百分比显示
- 文件完成状态指示（绿色 ✓ / 红色 ✗）
- 接收的消息每 2 秒轮询拉取

**Bug 修复**:
- 同 IP 多个名称 → IP 去重
- 绿灯不变灰 → `online` 字段根据 `last_seen` 计算（15 秒阈值）
- 离线不应移除 → 仅变灰，永久保留
- HELLO 消息丢失 → 优雅关闭
- 离线日志 → 探活线程对比在线/离线状态
- 单分片文件 MD5 校验失败 → 循环条件 `chunk_count < total_chunks`

---

## 四、技术难点与解决方案

### 4.1 网络接口选择

**问题**: 多网卡机器（Docker、VPN）上 `getifaddrs()` 返回的第一个非回环 IP 可能是虚拟接口 IP。

**解决**: 使用 UDP connect trick — 创建临时 socket，`connect()` 到 `1.1.1.1:53` 触发内核路由表查询，`getsockname()` 获取实际对外通信的接口 IP。此方法不产生网络流量，稳定可靠。

### 4.2 组播跨子网失效

**问题**: 两台设备在不同 /24 子网时，组播包被路由器丢弃。

**分析**: 两个子网（10.162.44.x 和 10.162.181.x）在同一 /16 但不同 /24，组播 TTL=1 经过路由器后减为 0。

**解决**: TTL 提高到 4；同时需要 `sudo ip route add 224.0.0.0/4 dev <iface>` 添加组播路由。

### 4.3 单分片文件空提交

**问题**: 接收循环 `get_max_contiguous_chunk() < total_chunks - 1` 对于 1 分片文件计算为 `0 < 0 = false`，循环不执行，空文件被当完整文件提交，MD5 不匹配。

**解决**: 改用 `chunk_count < meta.total_chunks` 直接控制循环。

### 4.4 IP 去重

**问题**: DEVICE_HELLO 每次握手创建新的 DeviceInfo（不同 UUID），手动添加也重复创建。

**解决**: `DeviceManager::find_device_id_by_ip(ip)` 按 IP 查找已有设备，存在则更新而非新增。

### 4.5 离线状态同步

**问题**: 服务器离线检测仅用于终端日志，前端需要独立的在线状态。

**解决**: 
- 探活线程成功时更新 `last_seen`
- `/api/devices` 计算 `(now - last_seen) < 15s` 返回 `online` 布尔值
- 前端根据 `online` 切换绿色/灰色状态灯
- 离线的设备不删除，仅标记为离线

---

## 五、数据流完整链路

```
用户操作                Web UI               Server              Remote Device
  │                      │                     │                      │
  │  选择文件            │                     │                      │
  │ ─────────────────→  │ FileReader          │                      │
  │                     │ readAsDataURL()     │                      │
  │                     │ base64 编码          │                      │
  │                     │                     │                      │
  │                     │ POST /api/transfer  │                      │
  │                     │ ─────────────────→  │ url_decode           │
  │                     │                     │ base64_decode         │
  │                     │                     │ write /tmp/p2p_send/  │
  │                     │                     │                      │
  │                     │                     │ SignalingClient      │
  │                     │                     │ FILE_REQUEST ───────→│
  │                     │                     │ ←── FILE_RESPONSE ── │ (ACCEPT)
  │                     │                     │                      │
  │                     │                     │ TransferSender       │
  │                     │                     │ connect 8890 ───────→│
  │                     │                     │ FILE_HEADER + chunks │
  │                     │                     │ ←── ACKs ─────────── │
  │                     │                     │                      │
  │                     │                     │ mark_complete()      │
  │                     │ ← GET /api/transfers│                      │
  │                     │ 进度: 100% ✓        │                      │
  │                     │                     │                      │
  │                     │                     │         [接收侧]     │
  │                     │                     │ ← FILE_REQUEST ───── │
  │                     │                     │ FILE_RESPONSE ──────→│
  │                     │                     │ ← connect 8890 ───── │
  │                     │                     │ recv chunks → .tmp   │
  │                     │                     │ commit → MD5 check   │
  │                     │                     │ set_on_receive_start │
  │                     │                     │ → add_task(接收)     │
```

---

## 六、已知限制与改进方向

1. **组播依赖路由配置**: 需要手动 `ip route add`，可考虑自动检测并警告
2. **大文件 base64 上传**: 浏览器侧 base64 编码增加 33% 体积，上传大文件 OOM 风险
3. **无用户认证**: 无密码或密钥验证，信任局域网内所有设备
4. **HTTP 明文传输**: Web 界面无 HTTPS，适合局域网使用
5. **单方向文件浏览**: 无法浏览远端设备的文件列表，只能推文件
6. **Web 界面无滚动加载**: 长对话历史全部渲染在 DOM 中
