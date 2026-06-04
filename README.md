# P2P 局域网文件传输工具

> WeChat 风格聊天 UI | 组播自动发现 | 断点续传 | 跨平台 | 零依赖

## 功能特性

- **自动发现** — UDP 组播 + TCP 握手双向发现，启动即可看到局域网内其他设备
- **微信风格 UI** — 设备列表 + 聊天气泡 + 文件进度条，浏览器自动打开
- **文件传输** — TCP 滑动窗口协议（64KB 分片），支持断点续传和 MD5 完整性校验
- **文字聊天** — 基于信令通道的实时文本消息，收发双端同步
- **设备持久化** — 添加过的设备自动保存，重启后列表中可见（在线/离线状态灯）
- **跨平台** — C++17，支持 Linux / Windows / macOS

## 快速开始

### 编译

```bash
git clone <repo> && cd net
cmake -B build -S .
cmake --build build -j$(nproc)
```

> **注意**: 添加新的 `.cpp` 文件后需重新运行 `cmake -B build -S .`（CMakeLists.txt 使用 `GLOB_RECURSE`，在配置时快照源文件列表）。

### 运行

```bash
./build/bin/P2PFileTransfer
```

- 自动运行 4 阶段单元测试，通过后启动所有服务
- 浏览器自动打开 `http://localhost:8891`
- `Ctrl+C` 优雅退出

### 组播路由配置（重要）

UDP 组播发现需要系统有组播路由。如果没有，设备发现会静默失败：

```bash
# 查看当前组播路由
ip route show | grep 224

# 添加组播路由（替换 <interface> 为你的网卡名）
sudo ip route add 224.0.0.0/4 dev <interface>

# 获取当前默认接口名
ip route show default | awk '{print $5}'
```

## 系统架构

```
┌─────────────────────────────────────────────────────────────────┐
│                         P2PFileTransfer                         │
├──────────┬──────────┬──────────┬──────────┬────────────────────┤
│ Discovery│ Signaling│ Transfer │ Web UI   │ Common             │
│  UDP:8888│  TCP:8889│  TCP:8890│ HTTP:8891│                     │
├──────────┼──────────┼──────────┼──────────┼────────────────────┤
│ 组播发现 │ 信令协商 │ 滑动窗口 │ REST API │ 跨平台Socket封装    │
│ DEV_HELLO│ 文本聊天 │ 分片传输 │ 静态文件 │ JSON协议 (nlohmann) │
│ 探活线程 │ 控制消息 │ MD5校验  │ Chat UI  │ UUID/MD5/Base64     │
└──────────┴──────────┴──────────┴──────────┴────────────────────┘
```

### 目录结构

```
net/
├── src/
│   ├── main.cpp              # 入口、单元测试、服务编排、REST API、探活线程
│   ├── common/               # 公共层
│   │   ├── types.h/cpp       # DeviceInfo, FileMeta, TransferTask 等数据结构
│   │   ├── platform.h/cpp    # 跨平台 Socket 封装 (Linux/Windows/macOS)
│   │   ├── protocol.h/cpp    # JSON 协议构建/解析 + 线格式 (send/recv)
│   │   └── utils.h/cpp       # UUID, MD5, Base64, URL解码, 格式化工具
│   ├── discovery/            # 设备发现层
│   │   ├── device_discovery  # UDP 组播收发 (239.255.255.250:8888)
│   │   └── device_manager    # 设备列表管理 (超时清理、回调通知)
│   ├── signaling/            # 信令控制层
│   │   ├── signaling_server  # TCP 服务端 (8889), 处理 FILE_REQUEST/TEXT/HELLO
│   │   └── signaling_client  # TCP 客户端, 短连接请求-响应 + DEVICE_HELLO 探活
│   ├── transfer/             # 文件传输层
│   │   ├── transfer_sender   # 发送方: 连接→文件头→滑动窗口发送→ACK处理
│   │   ├── transfer_receiver # 接收方: 监听→接收→分片写入→MD5校验
│   │   ├── transfer_manager  # 任务管理器 (线程安全的 TransferTask 映射)
│   │   ├── file_io           # 分片 I/O (pread/pwrite, 临时文件, 断点续传)
│   │   └── chunk.h           # 分片数据结构 (序列化/反序列化)
│   └── web/                  # Web 层
│       ├── http_server.h/cpp # 最小 HTTP 服务器 (GET/POST 路由, CORS, 静态文件)
│       └── static/           # 前端文件
│           ├── index.html    # 微信风格聊天 UI
│           ├── app.js        # 设备列表/聊天/文件传输/消息轮询逻辑
│           └── style.css     # 深色主题样式
└── lib/
    └── nlohmann/json.hpp     # 第三方 JSON 库 (v3.11.3, header-only)
```

## 网络协议

### 端口分配

| 端口 | 协议 | 用途 |
|------|------|------|
| 8888 | UDP 组播 | 设备发现 (DEVICE_BROADCAST / DEVICE_OFFLINE) |
| 8889 | TCP | 信令控制 (FILE_REQUEST / FILE_RESPONSE / TEXT_MESSAGE / DEVICE_HELLO) |
| 8890 | TCP | 文件传输 (文件头 + 分片数据 + ACK) |
| 8891 | HTTP | Web 界面 + REST API |

### 消息类型

**发现阶段（UDP 组播）**
- `DEVICE_BROADCAST` — 周期性设备上线广播（3 秒间隔）
- `DEVICE_OFFLINE` — 设备离线通知

**信令阶段（TCP 短连接）**
- `DEVICE_HELLO` — TCP 探活握手，携带本机设备信息，实现互相发现
- `FILE_REQUEST` — 文件传输请求（文件名、大小、分片数）
- `FILE_RESPONSE` — 请求响应（ACCEPT / REJECT）
- `TEXT_MESSAGE` — 聊天文本消息
- `TRANSFER_CANCEL / PAUSE / RESUME` — 传输控制

**传输阶段（TCP 长连接，带 1 字节类型标记）**
- `'J'` + JSON — 文件头（FileMeta）或 ACK 确认
- `'C'` + Binary — 文件分片数据（16 字节头 + 变长数据体）

### 分片协议

```
分片头部 (16 字节, 大端序):
┌────────────────┬────────────────┬────────────────┬────────────────┐
│ chunk_index    │ total_chunks   │ data_size      │ file_id_len    │
│ (u32, 4 字节)  │ (u32, 4 字节)  │ (u32, 4 字节)  │ (u32, 4 字节)  │
├────────────────┴────────────────┴────────────────┴────────────────┤
│ file_id (变长, UUID 格式, 通常 36 字节)                           │
├──────────────────────────────────────────────────────────────────┤
│ data (变长, 最多 64KB)                                            │
└──────────────────────────────────────────────────────────────────┘
```

## 数据流

### 设备发现流程

```
设备 A                          设备 B
  │                               │
  ├─ UDP 组播 DEVICE_BROADCAST ──→│   (每 3 秒)
  │                               │ B 的 recv_loop 收到
  │                               │ → DeviceManager.update_device(A)
  │                               │
  ├─ TCP 探活 + DEVICE_HELLO ────→│   (每 5 秒, 手动设备)
  │                               │ B 的信令服务器收到
  │                               │ → m_device_hello_cb → DeviceManager.update_device(A)
  │                               │
  │←─── TCP 探活 + DEVICE_HELLO ──┤    (B 也探活 A, 双向)
  │ A 的信令服务器收到              │
  │ → update_device(B)              │
```

### 文件传输流程

```
发送方 (Web Browser → Local Server → TransferSender)    接收方 (TransferReceiver)
  │                                                        │
  │ 1. 用户选文件, base64编码上传                            │
  │ → 服务器解码保存到 /tmp/p2p_send/                       │
  │                                                        │
  │ 2. SignalingClient 发送 FILE_REQUEST ──────────────→   │
  │                                                        │ → 自动 ACCEPT
  │ ←─────────────── FILE_RESPONSE (ACCEPT) ────────────   │
  │                                                        │
  │ 3. TransferSender 连接 8890 ─────────────────────────→  │
  │ → 发送文件头 (FileMeta JSON)                             │
  │                                    │← set_on_receive_start 回调
  │                                    │  → 创建 TransferTask (is_sender=false)
  │                                                        │
  │ 4. 滑动窗口发送分片 'C' + Chunk ────────────────────→   │
  │ ←───────────── 'J' + ACK ────────────────────────────   │
  │ (窗口大小=16, ACK 超时=3s 重传)                           │
  │                                                        │
  │ 5. 全部发送完成                                          │
  │                                    │ → commit_received_file (rename .tmp)
  │                                    │ → MD5 校验
  │                                    │ → mark_complete → 前端显示"已完成"
```

## REST API

| 方法 | 路径 | 请求体 | 说明 |
|------|------|--------|------|
| GET | `/api/devices` | — | 设备列表（含 `online` 状态, `manual` 标签） |
| GET | `/api/transfers` | — | 传输任务列表（含 `target_ip`, `is_sender`, 进度） |
| POST | `/api/transfer` | `target_ip=&target_port=&filename=&filedata=<base64>` | 上传并发送文件 |
| POST | `/api/message` | `target_ip=&target_port=&text=...` | 发送聊天文本 |
| POST | `/api/messages/poll` | `ip=...` | 拉取并清除该 IP 发来的新消息 |
| POST | `/api/peers/add` | `ip=&name=` | 手动添加设备（去重, 持久化） |
| POST | `/api/peers/remove` | `ip=` | 移除设备 |

## 关键技术实现

- **IP 选择**: 使用 UDP connect trick（连接 `1.1.1.1:53` 触发内核路由表查询，`getsockname()` 获取主网卡 IP），避免被 Docker/VPN 虚拟接口干扰
- **组播加入**: 在所有非回环接口上调用 `IP_ADD_MEMBERSHIP`，确保多网卡环境下组播包正常接收
- **IP 去重**: `DeviceManager::find_device_id_by_ip()` 防止同一 IP 重复出现
- **设备持久化**: `known_devices.json` 加载/保存，跳过本机 IP，保存时自动去重
- **滑动窗口**: 窗口大小 16，ACK 超时 3 秒重传，支持断点续传（通过 `FileChunkIO` 位图）
- **非阻塞 connect**: `SignalingClient` 使用 `set_nonblocking()` → `select()` → `getsockopt(SO_ERROR)` 实现可超时的 TCP 连接
- **信号安全**: 所有服务线程通过 `select()` 200ms 超时轮询 `m_running` 退出，避免 `close()` + `accept()` 的平台兼容问题
- **线程管理**: handler 线程使用 `join()` 回收，禁止 `detach()`

## 环境要求

- **编译器**: GCC 8+ / Clang 10+ / MSVC 2019+
- **CMake**: 3.14+
- **C++ 标准**: C++17
- **依赖**: 仅 pthread（Linux），无其他外部库
- **权限**: 添加组播路由需要 `sudo`

## 开发

### 测试

所有测试内联在 `src/main.cpp`，共 4 阶段：

```bash
./build/bin/P2PFileTransfer
# 查看输出中的 [OK] 和 === 验证完成 === 标记
```

### 端口清理

测试失败或进程残留时清理端口：

```bash
sudo fuser -k 18889/tcp 18890/tcp 8888/tcp 8889/tcp 8890/tcp
# 或直接杀进程
kill -9 $(pgrep -f P2PFileTransfer)
sleep 2  # 等待 TIME_WAIT
```

## 许可证

MIT License
