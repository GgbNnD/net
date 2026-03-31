import tkinter as tk

# Add the client directory to path if running directly to ensure imports work
import sys
import os
sys.path.append(os.path.dirname(os.path.abspath(__file__)))

from ui import ClientApp

def main():
    root = tk.Tk()
    app = ClientApp(root)
    root.mainloop()

if __name__ == "__main__":
    main()