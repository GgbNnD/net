import threading
import numpy as np
from PyQt5.QtCore import QObject, pyqtSignal

class ServerSignals(QObject):
    log_msg = pyqtSignal(str)
    new_client = pyqtSignal(int, int, str, int)  # client_id, pid, thread_name, thread_id
    client_disconnected = pyqtSignal(int)
    update_image = pyqtSignal(int, np.ndarray)

_signals = None

def init_signals():
    global _signals
    _signals = ServerSignals()

def get_signals():
    return _signals

def log(msg):
    # Helper to include thread info and emit to GUI
    thread_name = threading.current_thread().name
    try:
        thread_id = threading.get_native_id()
    except AttributeError:
        thread_id = threading.current_thread().ident
    full_msg = f"[{thread_name}:{thread_id}] {msg}"
    print(full_msg)
    s = get_signals()
    if s is not None:
        s.log_msg.emit(full_msg)