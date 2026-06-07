# P2P 局域网文件传输工具 — 技术报告

## 项目概述

本项目是一个跨平台（Linux/Windows/macOS）的局域网 P2P 文件传输工具，采用 C++17 实现，前端为微信风格的聊天 Web UI。核心功能包括：UDP 组播自动发现、ECDH 密钥交换、MAC 报文防伪造、TCP 滑动窗口文件传输（zlib 压缩）、MD5 完整性校验、文本聊天、设备持久化。

**技术栈**: C++17 + CMake + nlohmann/json + OpenSSL + zlib + HTML5/CSS/JS（无框架）

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
│                      │                     │  + zlib 压缩     │
├──────────────────────────────────────────────────────────────┤
│                    Security Layer                             │
│   MAC 报文鉴别码    │  ECDH 密钥交换 (secp256r1)              │
│   MD5 + 共享密钥    │  PeerKey 对端密钥管理                   │
├──────────────────────────────────────────────────────────────┤
│                      Common Layer                            │
│   跨平台 Socket 封装 │ JSON 协议 │ UUID/MD5/Base64 工具      │
│   MAC 签名/校验      │ 消息构建函数                            │
└──────────────────────────────────────────────────────────────┘
```

### 1.2 线程模型

每个服务层运行在独立线程中，通过回调函数解耦：

| 线程 | 所属模块 | 职责 | 间隔/触发 |
|------|---------|------|-----------|
| `m_send_thread` | DeviceDiscovery | UDP 组播 DEVICE_BROADCAST | 每 3 秒 |
| `m_recv_thread` | DeviceDiscovery | 阻塞接收 UDP 组播消息 | `recvfrom()` 阻塞 |
| `m_accept_thread` | SignalingServer | TCP 连接接受（select 轮询） | 200ms 超时 |
| `m_accept_thread` | TransferReceiver | 文件传输连接接受 | 200ms 超时 |
| `m_accept_thread` | HttpServer | HTTP 请求处理 | 200ms 超时 |
| `m_handler_threads` (动态) | SignalingServer/TransferReceiver/HttpServer | 每个客户端连接一个处理线程 | 连接时创建 |
| `g_peer_probe_thread` | main.cpp | TCP DEVICE_HELLO 探活所有设备 | 每 5 秒 |
| `m_cleanup_thread` | DeviceManager | 超时设备清理 | 每 1 秒 |
| `g_transfer_threads` (动态) | main.cpp | 每个文件传输一个发送线程 | 传输时创建 |

### 1.3 跨平台 Socket 封装

`src/common/platform.h/cpp` 提供平台无关的 Socket API：

| 平台 | `SOCKET_FD` | `INVALID_SOCKET_FD` | `CLOSE_SOCKET` | `SOCK_SEND` | `SOCK_RECV` |
|------|-------------|---------------------|----------------|-------------|-------------|
| Linux | `int` | `-1` | `close()` | `send()` | `recv()` |
| Windows | `SOCKET` (uint64_t) | `INVALID_SOCKET` | `closesocket()` | `send()` | `recv()` |

`NetworkUtils` 命名空间还提供 `set_nonblocking()`（`fcntl(O_NONBLOCK)` / `ioctlsocket(FIONBIO)`）、`set_reuse_addr()`、`get_local_ips()`、`is_would_block()`。

### 1.4 协议设计

所有控制消息使用 JSON 格式。数据通道（8890）使用 1 字节类型标记区分 JSON 和二进制分片：

```
if (marker == 'J') { /* JSON 消息: 文件头 / ACK */ }
if (marker == 'C') { /* 二进制分片: 16 字节头 + 变长数据 */ }
```

信令通道（8889）和 UDP 通道（8888）为纯 JSON。TCP 消息带 4 字节大端长度前缀：

```
[4-byte BE length] + [JSON string]
```

**分片头部设计（16 字节 + 变长）**:

| 偏移 | 大小 | 字段 | 说明 |
|------|------|------|------|
| 0 | 4 | chunk_index | 分片编号（从 0 开始） |
| 4 | 4 | total_chunks | 总分片数 |
| 8 | 4 | data_size | 有效数据长度 |
| 12 | 4 | file_id_len | UUID 字符串长度 |
| 16 | file_id_len | file_id | UUID 字符串 |
| 16+file_id_len | data_size | data | 分片数据（最多 64KB） |

**消息类型**（`src/common/protocol.h:MsgType`）:

| 阶段 | 消息类型 | 传输方式 | 说明 |
|------|---------|---------|------|
| 发现 | `DEVICE_BROADCAST` | UDP 组播 | 设备上线广播 |
| 发现 | `DEVICE_OFFLINE` | UDP 组播 | 设备离线通知 |
| 发现 | `DEVICE_HELLO` | TCP 8889 | 探活握手，互相发现 |
| 通信 | `TEXT_MESSAGE` | TCP 8889 | 聊天文本 |
| 协商 | `FILE_REQUEST` | TCP 8889 | 文件传输请求 |
| 协商 | `FILE_RESPONSE` | TCP 8889 | 接受/拒绝响应 |
| 控制 | `TRANSFER_CANCEL/PAUSE/RESUME/DONE/ERROR` | TCP 8889 | 传输控制 |
| 传输 | `CHUNK_ACK` | TCP 8890 | 分片确认 |
| 系统 | `TEXT_ACK` | TCP 8889 | 文本消息确认 |
| 系统 | `CONTROL_ACK` | TCP 8889 | 控制消息确认 |

---

## 二、MAC 报文鉴别码（防伪造）

### 2.1 设计

`src/common/protocol.cpp` 实现基于 MD5 + 共享密钥的轻量级 MAC（Message Authentication Code）：

```
MAC = MD5(json_payload_string + shared_secret)
```

- 共享密钥：`p2p-transfer-secret-2024`（编译期常量）
- 发送方：序列化消息 → 计算 MAC → 添加 `"mac"` 字段 → 发送
- 接收方：提取 `"mac"` 字段 → 移除后重新序列化 → 计算期望 MAC → 比对
- 兼容性：无 `mac` 字段的消息仍接受（向后兼容旧版本）

### 2.2 集成覆盖

| 协议层 | 签名入口 | 校验入口 |
|--------|---------|---------|
| TCP 信令 | `send_json_message()` 内部调用 `sign_message()` | `recv_json_message()` 内部调用 `verify_message()` |
| TCP 传输控制 | 同上（JSON 消息走 `send/recv_json_message`） | 同上 |
| UDP 组播 | `send_loop()` 显式调用 `sign_message()` | `recv_loop()` 显式调用 `verify_message()` |
| UDP 离线通知 | `stop()` 显式调用 `sign_message()` | 同上 |

### 2.3 安全特性

- **防伪造**：攻击者不知道共享密钥，无法伪造有效 MAC
- **防篡改**：修改消息内容会导致 MAC 不匹配，消息被丢弃
- **防重放**：可结合 `timestamp` 字段（`DEVICE_BROADCAST` 已包含时间戳）
- **签名覆盖**：`sign_message()` 先移除已有 `mac` 字段再计算，避免递归/篡改攻击

### 2.4 ECDH 密钥交换

为了解决硬编码共享密钥的问题，引入基于 OpenSSL 的 ECDH 密钥协商：

**算法**：`secp256r1` (NIST P-256) 椭圆曲线 Diffie-Hellman

**密钥生成** (`Utils::generate_ecdh_keypair`):
```
EVP_PKEY_keygen(EVP_PKEY_EC, NID_X9_62_prime256v1)
→ DER 编码 → Base64 编码 → 公钥/私钥字符串
```

**共享密钥计算** (`Utils::compute_ecdh_shared`):
```
1. Base64 解码本地私钥和远端公钥 → DER
2. d2i_AutoPrivateKey / d2i_PUBKEY 加载密钥
3. EVP_PKEY_derive() 计算共享密钥
4. MD5(shared_raw) → 64 字符 hex 子密钥
```

**密钥交换流程**:
```
A                                          B
  │ 生成 ECDH 密钥对                         │ 生成 ECDH 密钥对
  │                                         │
  │ DEVICE_HELLO (含 public_key_A)          │
  │ ────────────────────────────────────→   │
  │                                         │ compute_ecdh_shared(priv_B, pub_A)
  │                                         │ PeerKey::store(A.ip, shared_B)
  │                                         │
  │              DEVICE_HELLO (含 public_key_B)
  │   ←───────────────────────────────────  │
  │ compute_ecdh_shared(priv_A, pub_B)       │
  │ PeerKey::store(B.ip, shared_A)           │
  │                                         │
  │          后续消息 MAC = MD5(payload +     │
  │          DEFAULT_SECRET + shared_AB)     │
```

**存储** (`src/common/peer_key.h/cpp`):
- `PeerKey::store(ip, shared_secret)` — 按 IP 存储对端共享密钥
- `PeerKey::get(ip)` — 查询指定 IP 的密钥
- 线程安全：内部使用 `std::mutex` 保护

**MAC 签名增强**:
- `sign_message(msg, peer_ip)` 调用 `mac_secret_for(ip)` 获取 `DEFAULT_SECRET + peer_key`
- UDP 广播使用空 IP，仅用默认密钥
- 向后兼容：无 ECDH 密钥时回退到纯 DEFAULT_SECRET

---

## 三、核心算法与数据结构

### 3.1 滑动窗口协议

**参数**:
- 窗口大小: 16（最多 16 个未确认分片）
- 分片大小: 65536 字节（64KB）
- ACK 超时: 3000ms
- 发送缓冲区: 256KB (`SO_SNDBUF`)
- 接收超时: 500ms (`SO_RCVTIMEO`，在 `connect()` 之前设置)

**发送流程** (`TransferSender::send_chunks()`):
```
1. 计算总窗口大小: min(window_size, total_chunks)
2. for chunk_idx in 0..total_chunks-1:
     a. 填充窗口: while (sent_idx < total_chunks && sent_idx - base < window_size)
        - pread() 读取分片数据
        - send_chunk(sock, chunk) 发送 'C' 标记 + 二进制分片
        - sent_idx++
     b. 暂停 50ms（让接收方处理和回复 ACK）
     c. try_recv_ack(sock, ack) 非阻塞接收 ACK（SO_RCVTIMEO=500ms）
        - 如果收到且 max_contiguous >= base: base = max_contiguous + 1
     d. 检查取消标志 m_cancelled（原子变量）
3. 等待最终 ACK（最多 5 个超时周期 ≈ 2.5s）
4. 回调通知完成
```

**接收流程** (`TransferReceiver::handle_receive()`):
```
1. recv_file_header() → 读取 'J' 标记 → 解析 FILE_HEADER JSON → 获得 FileMeta
2. 创建 .tmp 文件 → open() + ftruncate() 预分配文件空间
3. 构造 FileChunkIO（读取已有文件大小用于断点续传）
4. while chunk_count < total_chunks:
     a. recv_chunk() → 读取 1 字节标记
        - 'C': 解析 16 字节头 + file_id + 分片数据
        - 'J': 解析 JSON 控制消息（TRANSFER_DONE/ERROR）
     b. pwrite() 写入 .tmp 文件
     c. 更新接收位图 (m_received_bitmap)
     d. send_ack(sock, file_id, get_max_contiguous_chunk())
5. 发送最终 ACK
6. commit_received_file() → rename(.tmp → 正式文件) → MD5 校验
```

### 3.2 FileChunkIO 分片 I/O

使用 POSIX `pread`/`pwrite` 实现随机位置的分片读写：

```cpp
// 发送方: 读取分片
offset = chunk_index * m_chunk_size;
actual_size = (chunk_index == last_chunk) ? m_file_size - offset : m_chunk_size;
pread(m_fd, data, actual_size, offset);

// 接收方: 写入分片
offset = chunk.chunk_index * m_chunk_size;
pwrite(m_fd, chunk.data.data(), chunk.data_size, offset);
m_received_bitmap[chunk.chunk_index] = true;
```

**断点续传支持** (`FileChunkIO::init()`):
- 构造函数读取 `.tmp` 文件大小 → 计算已完成分片数 → 初始化位图
- 已存在的完整分片在位图中标记为已接收
- `get_max_contiguous_chunk()` 扫描位图找最长连续前缀 → 发送方据此 `resume_from_chunk`

### 3.3 zlib 文件压缩传输

基于 zlib 的透明文件压缩，减少传输数据量：

**发送方** (`TransferSender::send_file`):
```
1. 读取原始文件到内存
2. Utils::compress_data(raw) → 调用 zlib compress()
3. 若压缩后体积 < 原始体积:
   - 写入临时文件 /tmp/p2p_send_compressed_{file_id}
   - 使用压缩后的 file_size / total_chunks
   - FILE_HEADER 设置 compression = "zlib"
4. 否则按原始文件传输
```

**接收方** (`TransferReceiver::handle_receive`):
```
1. 接收所有分片 → commit_received_file()
2. 若 meta.compression == "zlib":
   - 读取已提交文件到内存
   - Utils::decompress_data(compressed) → 调用 zlib uncompress()
   - 覆盖写回解压后的原始数据
3. MD5 校验（基于解压后的数据）
```

**压缩函数实现** (`Utils::compress_data` / `decompress_data`):
- `compress() / uncompress()` 单次调用接口
- `decompress_data` 使用倍增缓冲策略（初始 4x，翻倍至成功）处理未知解压大小
- 失败返回空字符串，调用方回退处理

**临时文件生命周期**:
```
接收开始 → 创建 {filename}.tmp → pwrite 写入各分片 → 全部接收完成
→ commit (rename .tmp → filename) → 解压缩覆盖 → MD5 校验
→ 校验通过 → 完成
→ 校验失败 → 保留文件 (供检查)
→ 取消 → unlink(.tmp)

**临时文件生命周期**:
```
接收开始 → 创建 {filename}.tmp → pwrite 写入各分片 → 全部接收完成
→ MD5 校验通过 → rename(.tmp → filename) → 完成
→ MD5 校验失败 → 保留 .tmp (供下次断点续传)
→ 取消 → unlink(.tmp)
```

### 3.4 设备发现机制

采用双层发现：UDP 组播 + TCP DEVICE_HELLO 握手。

**第一层: UDP 组播** (`DeviceDiscovery`)

| 参数 | 值 |
|------|-----|
| 组播地址 | `239.255.255.250` |
| 端口 | `8888` |
| 广播间隔 | 3 秒 |
| TTL | 64 |
| 广播内容 | `DEVICE_BROADCAST` (device_id, name, ip, **port=8889**) |
| 接收缓冲 | 2048 字节 |

**关键**: 广播消息中的 `port` 字段发送的是 **信令端口 8889**，而非发现端口 8888。探活线程使用此端口进行 TCP DEVICE_HELLO 连接。

**第二层: TCP DEVICE_HELLO 握手** (探活线程)

```
A 的探活线程 (每 5 秒)                     B 的信令服务器 (端口 8889)
   │                                           │
   │ test_connect(B.ip, B.port=8889)            │
   │ ──── TCP connect ──────────────────────→   │
   │ ──── DEVICE_HELLO JSON ────────────────→   │
   │                                           │ handle_client():
   │                                           │   m_device_hello_cb(msg, sender_ip)
   │                                           │   → DeviceManager::update_device(A)
   │                                           │   → save_known_devices()
   │                                           │
   │ shutdown(SHUT_WR)                          │
   │ drain recv buffer                          │ CLOSE_SOCKET
   │ CLOSE_SOCKET                               │
```

**互相发现原理**:
1. A 通过 UDP 自动发现 B → B 进入 A 的 DeviceManager（`manual=false`）
2. A 的探活线程探测 B（探活所有设备，不限于 manual）→ 发送 DEVICE_HELLO
3. B 收到 DEVICE_HELLO → B 的 DeviceManager 添加/更新 A（`manual=true`，持久化）
4. B 的探活线程也会探测 A → 双向建立

### 3.5 设备持久化

`known_devices.json` 存储手动添加和通过 DEVICE_HELLO 互相发现的设备：

```json
[
  {"id": "uuid", "name": "设备名", "ip": "10.150.65.192", "port": 8889}
]
```

**保存时机**: `/api/peers/add`、`/api/peers/remove`、DEVICE_HELLO 回调
**加载时机**: `init_discovery_service()` 启动时（在 DeviceDiscovery 启动之前）
**去重策略**: 加载和保存时都按 IP 去重，跳过本机 IP
**手动标志**: 仅 `manual=true` 的设备持久化；`manual=false` 的自动发现设备不持久化

---

## 四、MD5 算法实现

`src/common/utils.cpp` 包含完整的 RFC 1321 MD5 实现：

### 4.1 核心结构

```cpp
struct MD5Context {
    uint32_t state[4];      // A, B, C, D (初始化为标准魔数)
    uint64_t total_bytes;   // 已处理字节总数
    uint8_t  buffer[64];    // 当前未处理的输入块
    size_t   buffer_len;    // buffer 中有效字节数
};
```

### 4.2 函数接口

| 函数 | 用途 |
|------|------|
| `md5_init(ctx)` | 初始化 A=0x67452301, B=0xEFCDAB89, C=0x98BADCFE, D=0x10325476 |
| `md5_update(ctx, data, len)` | 增量输入数据，满 64 字节触发 `md5_transform()` |
| `md5_final(ctx, digest[16])` | 填充 0x80 + 零字节 + 64 位长度，最后 transform，输出 16 字节 |
| `md5_transform(state, block)` | 64 步 MD5 核心变换（4 轮 × 16 步） |
| `Utils::md5_data(data, size)` | 内存数据 MD5，返回 32 字符 hex |
| `Utils::md5_file(path)` | 文件 MD5，8KB 分块流式读取，返回 32 字符 hex |

### 4.3 在项目中的应用

| 场景 | 函数 | 位置 |
|------|------|------|
| 文件传输校验 | `md5_file` | `transfer_sender.cpp:63`, `transfer_receiver.cpp:264` |
| MAC 报文鉴别码 | `md5_data` | `protocol.cpp:24,33` |
| 单元测试验证 | `md5_data` | `main.cpp:102,369-374` |

---

## 五、REST API 完整说明

| 方法 | 路径 | 参数 (URL-encoded) | 用途 |
|------|------|-------------------|------|
| GET | `/api/devices` | — | 设备列表（含 `online`, `manual`, `name`, `ip`, `port`） |
| GET | `/api/transfers` | — | 传输任务（含 `state`, `progress`, `speed`） |
| POST | `/api/messages/poll` | `ip` | 拉取并清空指定 IP 发来的文本消息 |
| POST | `/api/message` | `target_ip`, `text` | 发送文本消息 |
| POST | `/api/peers/add` | `ip`, `name`(可选) | 手动添加设备，持久化 |
| POST | `/api/peers/remove` | `ip` | 移除手动设备，更新持久化 |
| POST | `/api/transfer` | `target_ip`, `filename`, `filedata`(base64) | 发起文件传输 |

**注意**: `/api/message` 和 `/api/transfer` 使用 `target_ip` 参数；`/api/messages/poll` 和 `/api/peers/add` 使用 `ip` 参数。`on_get()` 不支持 query 参数，需用 POST 传参。

**在线判定**: `/api/devices` 返回的 `online` 字段 = `(now - last_seen) < Defaults::DEVICE_TIMEOUT(10s)`

---

## 六、数据流完整链路

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
  │                     │                     │ send_request() →     │
  │                     │                     │   sign_message()     │
  │                     │                     │   FILE_REQUEST ────→│
  │                     │                     │   ← FILE_RESPONSE ── │
  │                     │                     │   verify_message() ← │
  │                     │                     │                      │
  │                     │                     │ TransferSender       │
  │                     │                     │ connect 8890 ───────→│
  │                     │                     │ FILE_HEADER + chunks │
  │                     │                     │ ←── CHUNK_ACKs ──── │
  │                     │                     │                      │
  │                     │                     │ mark_complete()      │
  │                     │ ← GET /api/transfers│                      │
  │                     │ 进度: 100% ✓        │                      │
```

---

## 七、关键技术难点与解决方案

### 7.1 网络接口选择

**问题**: 多网卡机器上 `getifaddrs()` 返回的首个 IP 可能为 Docker/VPN 虚拟接口。

**解决**: UDP connect trick — `connect(1.1.1.1:53)` 触发内核路由查询 → `getsockname()` 获取实际对外通信 IP。不产生网络流量。

### 7.2 组播跨子网失效

**问题**: 不同 /24 子网时组播包被路由器丢弃。

**解决**: TTL 提高到 64；需 `sudo ip route add 224.0.0.0/4 dev <iface>` 添加组播路由。

### 7.3 WiFi 客户端隔离

**问题**: 企业/校园 WiFi AP 阻止无线客户端间组播通信。

**解决**: `known_devices.json` + TCP DEVICE_HELLO 单播探活仍可穿越 AP 隔离。

### 7.4 UDP 广播端口错误

**问题**: 广播消息发送发现端口 8888，探活线程用此端口连接 TCP 失败（TCP 监听在 8889）。

**解决**: 广播中的 `port` 改为 `Defaults::SIGNALING_PORT`(8889)。

### 7.5 探活线程跳过自动发现设备

**问题**: 原探活线程 `if (!d.manual) continue;` 只探测手动添加设备，UDP 自动发现设备永远不被探活。

**解决**: 移除 manual 过滤，探测所有设备。

### 7.6 信令服务端无响应导致客户端超时

**问题**: `TEXT_MESSAGE` 和 `TRANSFER_*` 消息处理完成后直接关闭连接，客户端 `send_request()` 等待响应失败。

**解决**: 发送 `TEXT_ACK` / `CONTROL_ACK` 后再关闭连接。

### 7.7 单分片文件空提交

**问题**: 接收循环条件 `get_max_contiguous_chunk() < total_chunks - 1` 对 1 分片文件计算为 `0 < 0 = false`，空文件被提交。

**解决**: 改用 `chunk_count < meta.total_chunks`。

### 7.8 非阻塞连接错误判定

**问题**: `connect()` 非阻塞模式下返回 `EINPROGRESS` 被误判为真实错误。

**解决**: `is_would_block()` 同时检查 `EWOULDBLOCK`、`EAGAIN`、`EINPROGRESS`。

### 7.9 FileChunkIO 初始化顺序

**问题**: FileChunkIO 构造函数读取文件大小，若临时文件未创建则 total_chunks=0。

**解决**: 接收方先 `open()+ftruncate()` 创建 .tmp 文件，再构造 FileChunkIO。

### 7.10 `SO_RCVTIMEO` 设置时机

**问题**: 部分 Linux 内核在 `connect()` 之后设置 `SO_RCVTIMEO` 无效。

**解决**: `TransferSender::send_file()` 在 `connect()` 之前设置。

---

## 八、已知限制与改进方向

1. **组播依赖路由配置**: 需手动 `ip route add`，可考虑自动检测并提示
2. **大文件 base64 上传**: base64 编码增加 33% 体积，大文件有 OOM 风险
3. **压缩需全量加载内存**: 发送方和接收方压缩/解压时需将整个文件读入内存，超大文件（>1GB）可能 OOM
4. **ECDH 依赖 OpenSSL**: 增加编译依赖，Windows 平台需额外安装
5. **无密钥交换持久化**: ECDH 共享密钥不持久化，重启后需重新协商（通过 DEVICE_HELLO 自动完成）
6. **HTTP 明文传输**: Web 界面无 HTTPS，局域网可接受
7. **无用户认证**: 无密码或密钥验证，MAC 仅防伪造不防中间人
8. **短连接模型**: 信令通道每次消息新建 TCP 连接，高频消息效率低
9. **单方向文件浏览**: 只能推送文件，无法浏览远端文件列表
10. **Web 界面无滚动加载**: 长对话历史全部渲染在 DOM 中
