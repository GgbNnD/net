# AI-Powered Real-Time Video Stream Detection Gateway
(智能网络流媒体 AI 实时检测网关)

## 项目简介 (Project Overview)
本项目是一个基于 C/S (Client-Server) 架构的硬核系统编程级项目。主要用于展示在 Python 环境下，如何优雅地将 **网络编程 (Network I/O)**、**多线程 (Multi-threading)** 与 **进程间通信 (IPC / Multi-processing)** 结合，以解决 AI 推理（CPU/GPU密集型）阻塞高并发网络收发（I/O密集型）的经典痛点。

当前内置的 AI 模型为基于 OpenCV 的 Haar 级联人脸检测，但系统架构已完全解耦，可随时替换为更复杂的深度学习模型（如 YOLO, ResNet 等）。

## 核心架构设计 (Architecture)

系统采用了 **Master-Worker (主从多进程 + 多线程)** 架构：

1. **网络层 (Master Process & Threads)**
   - **主线程 (Main Thread)**: 仅负责绑定端口并 `accept` 新的 TCP 连接。
   - **客户端工作线程 (Client Threads)**: 每当有新客户端接入，分配一个独立线程负责该 Socket 的数据流读写，实现并发隔离。
2. **计算层 (AI Worker Process)**
   - 考虑到 Python 的 GIL (全局解释器锁) 以及 AI 计算的 CPU 密集特性，启动一个完全独立的进程来专门执行 AI 推理。
3. **进程间通信 (IPC)**
   - **Task Queue (消息队列)**: 网络线程收到完整的图像帧后，将 `(客户端ID, 图像数据, 专属接收管道)` 放入 `multiprocessing.Queue` 中。
   - **Pipe (匿名管道)**: AI 进程处理完毕后，通过网络线程传递过来的单向管道 `Pipe(tx)` 将检测结果精准回传给对应的网络线程 `Pipe(rx)`，随后网络线程将其发回客户端。

## 目录结构 (Directory Structure)
```text
.
├── client/
│   └── main.py       # 客户端：负责调用摄像头抓帧、JPEG压缩、应用层协议封装发送、结果渲染
├── server/
│   └── main.py       # 服务端：负责高并发 Socket 监听、粘包处理、IPC 调度与 AI 推理
└── README.md         # 项目文档
```

## 网络通信协议 (Custom TCP Protocol)
为了解决 TCP 流式传输中的“粘包”与“半包”问题，系统实现了一个简单的二进制应用层协议（TLV 变体）：
- **Header**: 4 字节的无符号整数（Big-Endian，`>I`），表示后续 Payload 的确切字节数。
- **Payload**: 
  - 客户端 -> 服务端：压缩后的 JPEG 图像字节流。
  - 服务端 -> 客户端：包含检测结果（如边界框坐标 `[{x, y, w, h}]` 或错误信息）的 JSON 字符串字节流。

## 环境与依赖 (Environment & Dependencies)
项目依赖于名为 `ai` 的 Conda 环境。
- **Python**: 3.10+
- **第三方库**: `opencv-python`, `numpy`

**环境配置指令:**
```bash
conda create -n ai python=3.10 -y
conda activate ai
pip install opencv-python numpy
```

## 运行指南 (How to Run)

1. **启动服务端** (在一个终端窗口中):
   ```bash
   conda activate ai
   cd server
   python main.py
   ```
2. **启动客户端** (在另一个终端窗口中，可启动多个以测试并发):
   ```bash
   conda activate ai
   cd client
   python main.py
   ```
   *注意: 如果运行环境没有摄像头，客户端会自动降级为生成模拟的运动色块视频流。*

---

## 🤖 给后续开发 Agent 的建议 (Instructions for Future Agents)

如果其他 AI Agent 需要在此项目基础上继续开发，请参考以下演进方向：

1. **AI 模型的平滑替换 (Upgrade AI Model)**
   - 目标: 将 `server/main.py` 中的 `cv2.CascadeClassifier` 替换为现代深度学习模型。
   - 建议: 引入 `torch` 和 `ultralytics` (YOLOv8)。由于架构已解耦，只需修改 `ai_worker_process` 函数内部的模型加载和推理逻辑，将 YOLO 返回的 bounding box 转换为统一定义的 JSON 格式即可，网络层代码**完全无需修改**。
   - 注意事项: 如果引入 GPU (CUDA)，请确保在 AI 子进程启动**之后**再初始化 CUDA context，避免多进程 CUDA context 冲突。

2. **性能优化 (Performance Optimization)**
   - **网络层**: 当前为每个连接创建一个 Thread (Thread-per-connection)。如果需要支持成千上万的并发连接，请重构 `server/main.py` 的网络层，使用基于 `selectors` 或 `asyncio` 的 Reactor 模式（多路复用）。
   - **IPC 层**: 对于极高分辨率的视频流，通过 `Queue` 传递大量 bytes 依然有拷贝开销。可以引入 `multiprocessing.shared_memory` (Python 3.8+)，将图像数据写入共享内存，Queue 中只传递内存句柄(Name)和尺寸，实现真正的零拷贝 (Zero-Copy) IPC。

3. **客户端优化 (Client Enhancements)**
   - 增加断线重连机制。
   - 实现异步发送/接收逻辑，避免等待服务器响应时造成本地摄像头画面的卡顿。
