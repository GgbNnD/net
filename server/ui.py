import os
import math
import multiprocessing as mp
from PyQt5.QtWidgets import (
    QMainWindow, QWidget, QVBoxLayout, QGridLayout, QTextEdit, QLabel, QSizePolicy
)
from PyQt5.QtCore import Qt
from PyQt5.QtGui import QImage, QPixmap

from common import get_signals
from network import ServerThread
from ai_worker import ai_worker_process

class MainWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("AI Server - Multi-Client Video Stream")
        self.resize(1024, 768)
        
        self.central_widget = QWidget()
        self.setCentralWidget(self.central_widget)
        
        self.main_layout = QVBoxLayout(self.central_widget)
        
        # Top status label for global Server & AI Process Info
        self.status_label = QLabel("Initializing server...")
        self.status_label.setAlignment(Qt.AlignCenter)
        self.status_label.setStyleSheet("font-size: 16px; font-weight: bold; background-color: #1e1e1e; color: #00ff00; padding: 10px; border-radius: 5px;")
        self.main_layout.addWidget(self.status_label, stretch=0)
        
        # Grid layout for video streams
        self.video_widget = QWidget()
        self.video_layout = QGridLayout(self.video_widget)
        self.main_layout.addWidget(self.video_widget, stretch=3)
        
        # Text area for logs
        self.log_area = QTextEdit()
        self.log_area.setReadOnly(True)
        self.main_layout.addWidget(self.log_area, stretch=1)
        
        # State
        self.client_labels = {}  # client_id -> dict {'container': QWidget, 'video': QLabel}
        
        # Connect signals
        s = get_signals()
        if s:
            s.log_msg.connect(self.append_log)
            s.new_client.connect(self.add_client_stream)
            s.client_disconnected.connect(self.remove_client_stream)
            s.update_image.connect(self.update_client_image)
        
        # Start server components
        self.start_server()

    def append_log(self, msg):
        self.log_area.append(msg)
        # Auto-scroll
        scrollbar = self.log_area.verticalScrollBar()
        scrollbar.setValue(scrollbar.maximum())

    def reorganize_grid(self):
        # Remove all widgets from layout
        for i in reversed(range(self.video_layout.count())): 
            widget = self.video_layout.itemAt(i).widget()
            if widget is not None:
                self.video_layout.removeWidget(widget)
        
        # Add back according to count to make it adaptive
        clients = list(self.client_labels.values())
        n = len(clients)
        if n == 0:
            return
            
        cols = math.ceil(math.sqrt(n))
        rows = math.ceil(n / cols)
        
        for idx, client_data in enumerate(clients):
            r = idx // cols
            c = idx % cols
            self.video_layout.addWidget(client_data['container'], r, c)

    def add_client_stream(self, client_id, pid, thread_name, thread_id):
        container = QWidget()
        layout = QVBoxLayout(container)
        layout.setContentsMargins(0, 0, 0, 0)
        
        info_text = f"Client {client_id} | PID: {pid} | Thread: {thread_name} ({thread_id})"
        info_label = QLabel(info_text)
        info_label.setAlignment(Qt.AlignCenter)
        info_label.setStyleSheet("background-color: #333; color: #0f0; font-weight: bold; padding: 5px;")
        info_label.setSizePolicy(QSizePolicy.Preferred, QSizePolicy.Maximum)
        
        video_label = QLabel(f"Waiting for stream... (Client {client_id})")
        video_label.setAlignment(Qt.AlignCenter)
        video_label.setStyleSheet("background-color: black; color: white; border: 1px solid gray;")
        video_label.setSizePolicy(QSizePolicy.Ignored, QSizePolicy.Ignored)
        video_label.setScaledContents(False)  # We will scale the pixmap manually to keep aspect ratio
        
        layout.addWidget(info_label, stretch=0)
        layout.addWidget(video_label, stretch=1)
        
        self.client_labels[client_id] = {'container': container, 'video': video_label}
        self.reorganize_grid()

    def remove_client_stream(self, client_id):
        if client_id in self.client_labels:
            client_data = self.client_labels.pop(client_id)
            client_data['container'].deleteLater()
            self.reorganize_grid()

    def update_client_image(self, client_id, img_rgb):
        if client_id in self.client_labels:
            label = self.client_labels[client_id]['video']
            
            h, w, ch = img_rgb.shape
            bytes_per_line = ch * w
            qimg = QImage(img_rgb.data, w, h, bytes_per_line, QImage.Format_RGB888)
            
            # Make a copy of the image so data is not garbage collected
            qimg_copy = qimg.copy()
            
            # Scale pixmap to fit the label size while keeping aspect ratio
            pixmap = QPixmap.fromImage(qimg_copy)
            scaled_pixmap = pixmap.scaled(
                label.size(), 
                Qt.KeepAspectRatio, 
                Qt.SmoothTransformation
            )
            label.setPixmap(scaled_pixmap)

    def start_server(self):
        self.task_queue = mp.Queue(maxsize=100)
        
        self.ai_process = mp.Process(target=ai_worker_process, args=(self.task_queue,), daemon=True)
        self.ai_process.start()
        
        self.server_thread = ServerThread('0.0.0.0', 8888, self.task_queue)
        self.server_thread.start()
        
        # Update status label with PIDs
        main_pid = os.getpid()
        ai_pid = self.ai_process.pid
        self.status_label.setText(f"🖥️ Server Master PID: {main_pid}  |  🧠 AI Worker Process PID: {ai_pid}  |  🔌 Port: 8888")

    def closeEvent(self, event):
        self.append_log("正在关闭服务器...")
        self.server_thread.stop()
        self.task_queue.put(None)
        self.ai_process.join(timeout=1.0)
        if self.ai_process.is_alive():
            self.ai_process.terminate()
        event.accept()