import tkinter as tk
from PIL import Image, ImageTk
from worker import ClientWorker

class ClientApp:
    def __init__(self, root):
        self.root = root
        self.root.title("Client - Real-time AI Stream Detection")
        self.root.protocol("WM_DELETE_WINDOW", self.on_closing)
        
        self.video_label = tk.Label(root)
        self.video_label.pack()
        
        self.status_label = tk.Label(root, text="正在连接...")
        self.status_label.pack()

        # We must keep a reference to current photo image to prevent garbage collection
        self._current_imgtk = None
        
        # Start network and processing thread
        self.worker = ClientWorker(self.update_status, self.update_image)
        self.worker.start()

    def update_status(self, text):
        self.root.after(0, lambda: self.status_label.config(text=text))

    def update_image(self, rgb_frame):
        # We must schedule UI updates in the main Tkinter thread
        self.root.after(0, self._render_image, rgb_frame)
        
    def _render_image(self, rgb_frame):
        img = Image.fromarray(rgb_frame)
        imgtk = ImageTk.PhotoImage(image=img)
        self._current_imgtk = imgtk
        self.video_label.configure(image=imgtk)

    def on_closing(self):
        self.worker.stop()
        self.root.destroy()