import sys
import threading
import multiprocessing as mp

# Add the server directory to path if running directly to ensure imports work
import os
sys.path.append(os.path.dirname(os.path.abspath(__file__)))

from PyQt5.QtWidgets import QApplication

from common import init_signals
from ui import MainWindow

def main():
    # Set thread name for main thread
    threading.current_thread().name = "MainGUIThread"
    
    app = QApplication(sys.argv)
    
    # Initialize signals after QApplication
    init_signals()
    
    window = MainWindow()
    window.show()
    sys.exit(app.exec_())

if __name__ == "__main__":
    mp.set_start_method('spawn', force=True) # Ensure safe multiprocess start on some platforms
    main()