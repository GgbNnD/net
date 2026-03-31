import cv2
import numpy as np
import json

def ai_worker_process(task_queue):
    """
    【进程间通信 (IPC) - 消费者】
    这是独立的 AI Worker 进程。由于 AI 计算是 CPU 密集型任务，我们将其放在独立进程中，
    避免 GIL 锁和计算阻塞网络 I/O。
    """
    print("[AI Worker] 进程启动，正在加载模型...")
    face_cascade = cv2.CascadeClassifier(cv2.data.haarcascades + 'haarcascade_frontalface_default.xml')
    print("[AI Worker] 模型加载完成，等待任务...")

    while True:
        task = task_queue.get()
        if task is None:
            break
        
        client_id, img_bytes, pipe_tx = task
        
        try:
            nparr = np.frombuffer(img_bytes, np.uint8)
            img = cv2.imdecode(nparr, cv2.IMREAD_COLOR)
            
            gray = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY)
            faces = face_cascade.detectMultiScale(gray, scaleFactor=1.1, minNeighbors=4)
            
            results = [{"x": int(x), "y": int(y), "w": int(w), "h": int(h)} for (x, y, w, h) in faces]
            pipe_tx.send(json.dumps(results))
        except Exception as e:
            print(f"[AI Worker] 处理异常: {e}")
            pipe_tx.send(json.dumps({"error": str(e)}))