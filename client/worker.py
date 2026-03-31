import socket
import cv2
import struct
import json
import time
import numpy as np
import threading
from utils import recvall

class ClientWorker(threading.Thread):
    def __init__(self, update_status_cb, update_image_cb):
        super().__init__(daemon=True)
        self.update_status_cb = update_status_cb
        self.update_image_cb = update_image_cb
        self.running = True
        self.client_socket = None
        self.cap = None
        self.dummy_mode = False

    def run(self):
        SERVER_IP = '127.0.0.1'
        PORT = 8888

        self.cap = cv2.VideoCapture(0)
        
        if not self.cap.isOpened():
            self.update_status_cb("⚠️ 使用模拟视频流")
            self.dummy_mode = True
        else:
            self.update_status_cb("✅ 成功开启摄像头！")
            self.dummy_mode = False

        angle = 0.0

        while self.running:
            self.client_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self.client_socket.settimeout(3.0)  # 超时时间设为3秒
            try:
                self.update_status_cb("正在连接服务器...")
                self.client_socket.connect((SERVER_IP, PORT))
                self.client_socket.settimeout(None)  # 连接成功后取消超时限制
                self.update_status_cb("✅ 成功连接服务器！")
            except Exception as e:
                self.update_status_cb(f"❌ 连接失败，3秒后重连...")
                if self.running: time.sleep(3)
                continue

            try:
                while self.running:
                    if self.dummy_mode:
                        frame = np.ones((480, 640, 3), dtype=np.uint8) * 100
                        x = int(320 + 200 * np.cos(angle))
                        y = int(240 + 150 * np.sin(angle))
                        cv2.rectangle(frame, (x-50, y-50), (x+50, y+50), (0, 0, 255), -1)
                        cv2.putText(frame, "Mock Video Stream", (50, 50), cv2.FONT_HERSHEY_SIMPLEX, 0.8, (255, 255, 255), 2)
                        angle += 0.1
                        time.sleep(0.05)
                    else:
                        ret, frame = self.cap.read()
                        if not ret:
                            time.sleep(0.1)
                            continue

                    encode_param = [int(cv2.IMWRITE_JPEG_QUALITY), 80]
                    result, encimg = cv2.imencode('.jpg', frame, encode_param)
                    if not result:
                        continue
                    
                    img_bytes = encimg.tobytes()

                    # Send length and data
                    self.client_socket.sendall(struct.pack('>I', len(img_bytes)) + img_bytes)

                    raw_msglen = recvall(self.client_socket, 4)
                    if not raw_msglen:
                        self.update_status_cb("❌ 服务器中断连接。")
                        break
                    msglen = struct.unpack('>I', raw_msglen)[0]
                    
                    result_bytes = recvall(self.client_socket, msglen)
                    if not result_bytes:
                        self.update_status_cb("❌ 未接收到完整数据。")
                        break
                        
                    detections = json.loads(result_bytes.decode('utf-8'))
                    
                    if isinstance(detections, dict) and "error" in detections:
                        print(f"服务器端 AI 模块报错: {detections['error']}")
                        continue

                    for det in detections:
                        if 'x' in det:
                            x, y, w, h = det['x'], det['y'], det['w'], det['h']
                            cv2.rectangle(frame, (x, y), (x+w, y+h), (0, 0, 255), 2)
                            cv2.putText(frame, 'Face Detected', (x, y-10), cv2.FONT_HERSHEY_SIMPLEX, 0.5, (0, 255, 0), 2)

                    # Return RGB frame for UI
                    rgb_frame = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)
                    self.update_image_cb(rgb_frame)

            except Exception as e:
                if self.running:
                    self.update_status_cb(f"❌ 客户端异常: {e}")
            finally:
                if self.client_socket:
                    try:
                        self.client_socket.close()
                    except:
                        pass
                if self.running:
                    self.update_status_cb("⚠️ 连接已断开，准备重连...")
                    time.sleep(2)

        # 循环结束，清理资源
        if self.cap and not self.dummy_mode:
            self.cap.release()
            
    def stop(self):
        self.running = False
        if self.client_socket:
            try:
                self.client_socket.close()
            except:
                pass