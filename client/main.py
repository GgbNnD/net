import socket
import cv2
import struct
import json
import time
import numpy as np
import sys

def recvall(sock, n):
    """【网络通信】从 Socket 接收完整的 n 字节数据，解决半包/粘包"""
    data = bytearray()
    while len(data) < n:
        packet = sock.recv(n - len(data))
        if not packet:
            return None
        data.extend(packet)
    return data

def main():
    SERVER_IP = '127.0.0.1' # 服务器端 IP (单机跑可以写本机 127.0.0.1)
    PORT = 8888

    # 【网络编程】初始化 TCP 客户端
    client_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    print(f"尝试连接到服务器 {SERVER_IP}:{PORT} ...")
    try:
        client_socket.connect((SERVER_IP, PORT))
        print("✅ 成功连接服务器！")
    except Exception as e:
        print(f"❌ 连接失败: {e}")
        sys.exit(1)

    # 尝试打开摄像头 (0 通常是默认笔记本/外接摄像头)
    cap = cv2.VideoCapture(0)
    
    # 兼容性处理：如果没有摄像头，为了演示我们生成一个不断移动的图形作为视频流
    if not cap.isOpened():
        print("⚠️ 警告: 无法打开摄像头，将使用模拟生成的“移动方块”视频流来替代真实画面。")
        dummy_mode = True
        angle = 0.0
    else:
        dummy_mode = False

    print("=== 按下 'q' 键退出客户端 ===")

    try:
        while True:
            # 1. 抓取/生成帧
            if dummy_mode:
                # 绘制一个简单的模拟运动场景 (灰色背景 + 一个红色的方块)
                # 注：如果模拟帧没有类似人脸的纹理，服务器的人脸模型检测不出结果是正常的。
                frame = np.ones((480, 640, 3), dtype=np.uint8) * 100
                x = int(320 + 200 * np.cos(angle))
                y = int(240 + 150 * np.sin(angle))
                cv2.rectangle(frame, (x-50, y-50), (x+50, y+50), (0, 0, 255), -1)
                cv2.putText(frame, "Mock Video Stream (No Camera Detected)", (50, 50), cv2.FONT_HERSHEY_SIMPLEX, 0.8, (255, 255, 255), 2)
                angle += 0.1
                time.sleep(0.05) # 模拟 20 FPS 的生成速率
            else:
                ret, frame = cap.read()
                if not ret:
                    break

            # 2. 压缩编码图片。为了减少网络传输带宽，我们将原始图像编码为 JPEG 字节流
            encode_param = [int(cv2.IMWRITE_JPEG_QUALITY), 80]
            result, encimg = cv2.imencode('.jpg', frame, encode_param)
            if not result:
                continue
            
            img_bytes = encimg.tobytes()

            # 3. 【网络通信 - 发送】协议封装：先发送 4 字节(int)的长度，再发送真正的图片内容
            client_socket.sendall(struct.pack('>I', len(img_bytes)) + img_bytes)

            # 4. 【网络通信 - 接收】协议解析：先读取 4 字节获取返回结果的长度
            raw_msglen = recvall(client_socket, 4)
            if not raw_msglen:
                print("服务器中断连接。")
                break
            msglen = struct.unpack('>I', raw_msglen)[0]
            
            # 5. 读取完整的返回结果 (JSON字符串)
            result_bytes = recvall(client_socket, msglen)
            if not result_bytes:
                print("未接收到完整数据，服务器断开连接。")
                break
                
            # 解析 AI 进程传回来的检测结果
            detections = json.loads(result_bytes.decode('utf-8'))
            
            # 判断结果中是否包含报错信息
            if isinstance(detections, dict) and "error" in detections:
                print(f"服务器端 AI 模块报错: {detections['error']}")
                continue

            # 6. 在本地窗口渲染带检测框的画面
            for det in detections:
                if 'x' in det:
                    x, y, w, h = det['x'], det['y'], det['w'], det['h']
                    # 用红框圈出人脸
                    cv2.rectangle(frame, (x, y), (x+w, y+h), (0, 0, 255), 2)
                    cv2.putText(frame, 'Face Detected', (x, y-10), cv2.FONT_HERSHEY_SIMPLEX, 0.5, (0, 255, 0), 2)

            # 弹出窗口显示
            cv2.imshow("Client - Real-time AI Stream Detection", frame)
            
            # 检测是否按下 Q 键
            if cv2.waitKey(1) & 0xFF == ord('q'):
                break

    except ConnectionResetError:
        print("❌ 服务器强制关闭了连接。")
    except Exception as e:
        print(f"❌ 客户端发生错误: {e}")
    finally:
        if not dummy_mode:
            cap.release()
        cv2.destroyAllWindows()
        client_socket.close()
        print("客户端已退出。")

if __name__ == "__main__":
    main()
