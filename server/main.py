import socket
import threading
import multiprocessing as mp
import cv2
import numpy as np
import struct
import json

def ai_worker_process(task_queue):
    """
    【进程间通信 (IPC) - 消费者】
    这是独立的 AI Worker 进程。由于 AI 计算是 CPU 密集型任务，我们将其放在独立进程中，
    避免 GIL 锁和计算阻塞网络 I/O。
    """
    print("[AI Worker] 进程启动，正在加载模型...")
    # 为了避免依赖沉重的深度学习库，这里使用 OpenCV 自带的 Haar 级联分类器做人脸检测演示。
    # 在真实项目中，这里会加载 PyTorch/TensorFlow 的 YOLO/ResNet 模型。
    face_cascade = cv2.CascadeClassifier(cv2.data.haarcascades + 'haarcascade_frontalface_default.xml')
    print("[AI Worker] 模型加载完成，等待任务...")

    while True:
        # IPC: 从多进程队列中获取任务，阻塞等待
        task = task_queue.get()
        if task is None:  # 毒药药丸(Poison Pill)，用于安全退出进程
            break
        
        client_id, img_bytes, pipe_tx = task
        
        try:
            # 解析客户端传来的图片字节流
            nparr = np.frombuffer(img_bytes, np.uint8)
            img = cv2.imdecode(nparr, cv2.IMREAD_COLOR)
            
            # 执行 AI 推理计算 (人脸检测)
            gray = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY)
            faces = face_cascade.detectMultiScale(gray, scaleFactor=1.1, minNeighbors=4)
            
            # 格式化检测结果 (边界框的坐标和大小)
            results = [{"x": int(x), "y": int(y), "w": int(w), "h": int(h)} for (x, y, w, h) in faces]
            
            # IPC: 使用单向管道(Pipe)将结果传回给专门负责该客户端的网络线程
            pipe_tx.send(json.dumps(results))
        except Exception as e:
            print(f"[AI Worker] 处理异常: {e}")
            pipe_tx.send(json.dumps({"error": str(e)}))

def recvall(sock, n):
    """【网络编程】辅助函数：确保从 socket 读取精确的 n 个字节，解决 TCP 粘包/半包问题"""
    data = bytearray()
    while len(data) < n:
        packet = sock.recv(n - len(data))
        if not packet:
            return None
        data.extend(packet)
    return data

def handle_client_thread(conn, addr, task_queue, client_id):
    """
    【多线程编程】每个客户端连接由一个独立的线程处理。
    负责网络 I/O，并将任务投递给 AI 进程。
    """
    print(f"[Network Thread] 线程启动，处理客户端 {addr} (ID: {client_id})")
    
    # IPC: 创建管道 (Pipe)。网络线程持有 pipe_rx 接收结果，AI 进程持有 pipe_tx 发送结果。
    # 这样可以精准地将 AI 处理完的图片结果返回给对应的客户端线程。
    pipe_rx, pipe_tx = mp.Pipe(duplex=False)
    
    try:
        while True:
            # 网络通信协议 Step 1: 读取 4 字节的包头，它代表接下来图片数据的长度
            raw_msglen = recvall(conn, 4)
            if not raw_msglen:
                break
            msglen = struct.unpack('>I', raw_msglen)[0]
            
            # 网络通信协议 Step 2: 根据包头长度，读取完整的 JPEG 图片数据
            img_data = recvall(conn, msglen)
            if not img_data:
                break
            
            # IPC: 将 (客户端ID, 图片数据, 专属管道发送端) 放入多进程队列，交由 AI 进程处理
            task_queue.put((client_id, img_data, pipe_tx))
            
            # IPC: 线程阻塞在这里，等待 AI 进程通过管道把结果传回来
            result_json = pipe_rx.recv()
            
            # 网络编程: 收到 AI 结果后，按照相同协议（4字节长度头 + 载荷数据）发送回客户端
            res_bytes = result_json.encode('utf-8')
            conn.sendall(struct.pack('>I', len(res_bytes)) + res_bytes)
            
    except ConnectionResetError:
        pass
    except Exception as e:
        print(f"[Network Thread] 客户端 {client_id} 发生错误: {e}")
    finally:
        print(f"[Network Thread] 客户端 {addr} (ID: {client_id}) 连接已断开")
        conn.close()

def main():
    HOST = '0.0.0.0'
    PORT = 8888

    # IPC: 创建用于主进程（网络）和子进程（AI计算）之间通信的消息队列 (Message Queue)
    task_queue = mp.Queue(maxsize=100)
    
    # 【多进程编程】启动重负载的 AI 工作进程
    ai_process = mp.Process(target=ai_worker_process, args=(task_queue,), daemon=True)
    ai_process.start()

    # 【网络编程】初始化 TCP 服务器
    server_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1) # 允许端口复用
    server_socket.bind((HOST, PORT))
    server_socket.listen(5)
    
    print(f"[Server Master] 服务端已启动，正在监听 {HOST}:{PORT}")
    client_id_counter = 0

    try:
        while True:
            # 主线程负责 accept 阻塞等待新连接
            conn, addr = server_socket.accept()
            client_id_counter += 1
            
            # 每到来一个新连接，分配一个独立的线程来处理该 Socket 连接的网络收发
            client_thread = threading.Thread(
                target=handle_client_thread, 
                args=(conn, addr, task_queue, client_id_counter),
                daemon=True
            )
            client_thread.start()
    except KeyboardInterrupt:
        print("\n[Server Master] 收到关闭信号，正在清理资源...")
    finally:
        server_socket.close()
        # 发送毒药药丸让 AI 进程安全退出
        task_queue.put(None) 
        ai_process.join()

if __name__ == "__main__":
    main()
