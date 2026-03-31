# AI Server-Client System - Agentic Coding Guidelines

This document provides context, commands, and style guidelines for AI coding agents operating in this repository. 

## 1. Project Context & Architecture

This repository contains a real-time AI video streaming and detection system separated into a server and a client.

- **Server (`server/`)**: Uses **PyQt5** for the GUI, **multiprocessing** for CPU-intensive AI tasks (OpenCV face detection), and **threading** for concurrent TCP network I/O.
- **Client (`client/`)**: Uses **Tkinter** for the GUI and a background thread for webcam capture (OpenCV), image encoding, and network transmission.
- **Environment**: The project relies on a Conda environment named `ai`. Key dependencies include `PyQt5`, `opencv-python-headless` (to avoid Qt plugin conflicts), `numpy`, and `Pillow`.

---

## 2. Build, Run, and Lint Commands

Agents should execute commands within the designated Conda environment.

### Environment Activation
Before running any scripts, ensure the environment is activated:
```bash
source /home/cells/miniconda3/etc/profile.d/conda.sh && conda activate ai
```

### Execution Commands
- **Run Server**: `python server/main.py`
- **Run Client**: `python client/main.py`

### Linting & Formatting (Recommended)
We prefer `ruff` for fast linting and formatting. If not installed, use standard tools like `flake8` or `black`.
- **Check Linting**: `ruff check .`
- **Auto-format Code**: `ruff format .`

### Testing Commands
While a formal test suite might not be fully initialized, standard `pytest` is the designated framework.
- **Run all tests**: `pytest`
- **Run a specific test file**: `pytest tests/test_network.py`
- **Run a single test function**: `pytest tests/test_network.py::test_recvall`
- *(Note: Ensure sockets/ports are properly mocked or released during network testing to prevent address-in-use errors).*

---

## 3. Architecture & Threading Rules

When modifying the codebase, agents MUST adhere to these strict architectural constraints:

### Server Threading/Process Rules
1. **Never block the Main GUI Thread**: All network I/O must happen in background threads (e.g., `ServerThread`, `handle_client_thread`).
2. **Never run CPU-bound AI in a Thread**: Due to the Python GIL, OpenCV/AI inference must occur in an isolated `multiprocessing.Process` (`ai_worker.py`).
3. **Cross-Thread GUI Updates**: Background threads/processes MUST NEVER update PyQt5 widgets directly. Use `pyqtSignal` emitted via `ServerSignals` to pass data to the main thread.
4. **IPC**: Use `multiprocessing.Queue` or `multiprocessing.Pipe` for exchanging images and results between network threads and the AI worker process.

### Client Threading Rules
1. **Tkinter Safety**: Tkinter is strictly single-threaded. The `ClientWorker` thread must NEVER directly configure Tkinter widgets.
2. **Callbacks**: Use `self.root.after(0, callback_function, args)` to schedule UI updates safely from the worker thread.

### Network Protocol
1. **TCP Streaming**: Because TCP is a stream, prevent half-packets/sticky packets by using the length-prefix framing protocol.
2. **Packet Format**: `[4-byte big-endian Unsigned Int (Length)] + [Payload (JPEG bytes / JSON bytes)]`.
3. **Helper Function**: Always use the `recvall(sock, n)` utility to guarantee exactly `n` bytes are read.

---

## 4. Code Style Guidelines

### Python Formatting & PEP8
- **Indentation**: Use 4 spaces per indentation level.
- **Line Length**: Aim for a maximum line length of 100 characters.
- **Imports**: Group imports properly:
  1. Standard library imports (e.g., `os`, `sys`, `socket`, `threading`, `json`)
  2. Third-party imports (e.g., `cv2`, `numpy`, `PyQt5`, `PIL`)
  3. Local application imports (e.g., `from common import ...`)
- **Strings**: Use f-strings for string interpolation. 

### Naming Conventions
- **Variables & Functions**: Use `snake_case` (e.g., `update_image`, `client_socket`).
- **Classes**: Use `PascalCase` (e.g., `ServerThread`, `MainWindow`).
- **Constants**: Use `UPPER_SNAKE_CASE` (e.g., `SERVER_IP`, `PORT`).
- **Private Members**: Prefix internal variables or methods with an underscore (e.g., `_current_imgtk`).

### Typing
- **Type Hints**: Gradually introduce standard Python type hints (`-> None`, `: int`, `: np.ndarray`) to improve code readability and facilitate static analysis. 
  *Example*: `def recvall(sock: socket.socket, n: int) -> bytearray | None:`

### Error Handling
- **Specific Exceptions**: Catch specific exceptions (e.g., `ConnectionResetError`, `socket.timeout`) instead of a broad `except Exception:` whenever possible.
- **Logging**: Do not silently pass errors (unless intentionally ignoring `ConnectionResetError` on disconnect). Always log exceptions using the provided `log()` utility or print statements.
- **Resource Cleanup**: Always use `try...finally` blocks or context managers (`with`) to ensure sockets are closed, file descriptors are freed, and hardware devices (like `cv2.VideoCapture`) are released properly, even during a crash.
- **Graceful Degradation**: If hardware is missing (e.g., no webcam found), provide a fallback mode (like the existing `dummy_mode` drawing moving blocks) instead of crashing abruptly.

### UI / UX Aesthetics
- **Layouts**: Use layout managers (`QGridLayout`, `QVBoxLayout`, `pack(fill=BOTH)`) rather than absolute positioning.
- **Adaptability**: Ensure widgets gracefully scale when windows are resized (e.g., setting `QSizePolicy` correctly in PyQt).
- **Feedback**: Provide immediate visual feedback for network states (e.g., "Connecting...", "Success", "Disconnected").
