import os
import socket
import threading
import multiprocessing as mp
import struct
import json
import cv2
import numpy as np

from PyQt5.QtCore import QThread
from common import log, get_signals

def recvall(sock, n):
    data = bytearray()
    while len(data) < n:
        packet = sock.recv(n - len(data))
        if not packet:
            return None
        data.extend(packet)
    return data

def handle_client_thread(conn, addr, task_queue, client_id):
    pid = os.getpid()
    thread_name = threading.current_thread().name
    try:
        thread_id = threading.get_native_id()
    except AttributeError:
        thread_id = threading.current_thread().ident

    log(f"处理客户端 {addr} (ID: {client_id})")
    s = get_signals()
    if s: s.new_client.emit(client_id, pid, thread_name, thread_id)
    
    pipe_rx, pipe_tx = mp.Pipe(duplex=False)
    
    try:
        while True:
            raw_msglen = recvall(conn, 4)
            if not raw_msglen:
                break
            msglen = struct.unpack('>I', raw_msglen)[0]
            
            img_data = recvall(conn, msglen)
            if not img_data:
                break
            
            task_queue.put((client_id, img_data, pipe_tx))
            
            result_json = pipe_rx.recv()
            
            # --- Prepare image for UI ---
            nparr = np.frombuffer(img_data, np.uint8)
            img = cv2.imdecode(nparr, cv2.IMREAD_COLOR)
            if img is not None:
                # Draw bounding boxes
                results = json.loads(result_json)
                if isinstance(results, list):
                    for box in results:
                        x, y, w, h = box.get("x", 0), box.get("y", 0), box.get("w", 0), box.get("h", 0)
                        cv2.rectangle(img, (x, y), (x+w, y+h), (0, 255, 0), 2)
                
                # Convert BGR to RGB for PyQt
                img_rgb = cv2.cvtColor(img, cv2.COLOR_BGR2RGB)
                if s: s.update_image.emit(client_id, img_rgb)
            
            # --- Send result back to client ---
            res_bytes = result_json.encode('utf-8')
            conn.sendall(struct.pack('>I', len(res_bytes)) + res_bytes)
            
    except ConnectionResetError:
        pass
    except Exception as e:
        log(f"客户端 {client_id} 发生错误: {e}")
    finally:
        log(f"客户端 {addr} (ID: {client_id}) 连接已断开")
        if s: s.client_disconnected.emit(client_id)
        conn.close()

class ServerThread(QThread):
    def __init__(self, host, port, task_queue):
        super().__init__()
        self.host = host
        self.port = port
        self.task_queue = task_queue
        self.server_socket = None
        self.running = True

    def run(self):
        self.server_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.server_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.server_socket.bind((self.host, self.port))
        self.server_socket.listen(5)
        
        log(f"服务端已启动，正在监听 {self.host}:{self.port}")
        client_id_counter = 0

        self.server_socket.settimeout(1.0)

        while self.running:
            try:
                conn, addr = self.server_socket.accept()
                conn.settimeout(None)
                client_id_counter += 1
                
                client_thread = threading.Thread(
                    target=handle_client_thread, 
                    args=(conn, addr, self.task_queue, client_id_counter),
                    name=f"ClientThread-{client_id_counter}",
                    daemon=True
                )
                client_thread.start()
            except socket.timeout:
                continue
            except Exception as e:
                if self.running:
                    log(f"Server accept error: {e}")
                break
                
        self.server_socket.close()
        log("服务端已停止监听")

    def stop(self):
        self.running = False
        self.wait()