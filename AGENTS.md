# AGENTS.md — P2P File Transfer Tool

## Build & Run

```bash
# Configure (required after adding new .cpp files — GLOB_RECURSE needs re-run)
cmake -B build -S .
cmake --build build -j$(nproc)

# Run (terminates on Ctrl+C; tests run at startup, then services enter main loop)
./build/bin/P2PFileTransfer
```

**When adding new `.cpp` files:** Must re-run `cmake -B build -S .` because CMakeLists.txt uses `file(GLOB_RECURSE)`, which snapshots at configure time.

## Architecture

```
src/main.cpp           → Entry point, inline unit tests, service orchestration
src/common/            → Shared types, platform socket wrapper, utils, JSON protocol
src/discovery/         → UDP multicast device discovery (port 8888)
src/signaling/         → TCP signaling channel (port 8889, short-connection)
src/transfer/          → TCP file transfer (port 8890, long-connection, sliding window)
src/web/               → Minimal HTTP server + REST API + static frontend (port 8891)
lib/nlohmann/json.hpp  → Vendored JSON library (v3.11.3)
```

Each layer runs in its own thread(s). All services register callbacks rather than direct coupling.

## Testing

No external test framework. Tests are inline in `main.cpp` — `run_phase1_tests()` through `run_phase4_tests()`:

1. **Phase 1** — UUID, MD5, formatting, protocol message construction
2. **Phase 2** — DeviceManager add/remove/timeout callbacks (no network)
3. **Phase 3** — SignalingServer + SignalingClient round-trip on localhost:18889
4. **Phase 4** — Chunk serde, FileChunkIO local I/O, e2e TransferSender→TransferReceiver on localhost:18890

Tests run sequentially at startup. If any test fails (e.g., port in use), the program still starts live services and enters the main loop.

## Port Cleanup After Failed Runs

Test ports can stay in LISTEN from zombie processes. Kill them:

```bash
kill -9 $(ps aux | grep P2PFileTransfer | grep -v grep | awk '{print $2}')
# Or targeted:
fuser -k 18889/tcp 18890/tcp 8888/tcp 8889/tcp 8890/tcp
```

## Common Compilation Pitfalls

- **`#include <fstream>`** — Often missing in main.cpp test code that creates test files
- **`#include <fcntl.h>`, `<unistd.h>`, `<sys/stat.h>`** — Needed for POSIX file operations (`open`, `pread`, `pwrite`, `ftruncate`)
- **`#include <thread>`, `<chrono>`** — Must be explicit; `std::this_thread::sleep_for` needs them
- **`json` vs `nlohmann::json`** — `protocol.h` defines `using json = nlohmann::json`. New headers should use `nlohmann::json` (fully qualified) to avoid dependency on `protocol.h` inclusion order.

## Protocol Wire Format

All TCP messages on the data channel (port 8890) use a **1-byte type marker** prefix:

| Marker | Description   | Payload Format                           |
|--------|---------------|------------------------------------------|
| `'J'`  | JSON message  | `Protocol::send_json_message()` (4-byte big-endian length + JSON) |
| `'C'`  | Binary chunk  | `Chunk::serialize()` (16-byte header + variable body) |

The signaling channel (port 8889) uses only JSON messages (no marker; short-connection request-response).

## Key Implementation Details

### Non-blocking connect with timeout
`SignalingClient::connect_to()` uses: `set_nonblocking()` → `connect()` (expect `EINPROGRESS`) → `select()` with timeout → `getsockopt(SO_ERROR)` → `set_blocking()`.

### Missing `EINPROGRESS` in `is_would_block()`
`platform.cpp:is_would_block()` must include `EINPROGRESS` (Linux errno 115) alongside `EWOULDBLOCK`/`EAGAIN`. Non-blocking `connect()` returns `EINPROGRESS`, not `EWOULDBLOCK`.

### Server accept loops use `select()`, not blocking `accept()`
Both `SignalingServer` and `TransferReceiver` use `FD_SET` + `select()` with 200ms timeout so the loop can check `m_running` and exit on `stop()`. Blocking `accept()` cannot be reliably interrupted by `close()` on all platforms.

### Handler thread management (no `detach()`)
Both `SignalingServer` and `TransferReceiver` keep handler threads in `std::vector<std::thread>` and join them in `stop()`. Never use `detach()` — it creates zombie processes and port leaks.

### Socket receive timeouts
`SignalingServer::handle_client()` sets `SO_RCVTIMEO` (5s) on each accepted client socket, preventing stuck handler threads if the client disconnects mid-protocol.

### FileChunkIO receiver init
Must create+ftruncate the `.tmp` file BEFORE constructing `FileChunkIO` (which reads file size from disk to compute `total_chunks`). The class does not support re-init after construction.

### Self-exclusion in discovery
Both `DeviceDiscovery::recv_loop()` and `DeviceManager::update_device()` check `device.id == m_device_id` and skip. This double guard ensures the local device never appears in the online list even through edge cases.

### Callback-safe unlocking
`DeviceManager::update_device()` and `remove_device()` use `std::unique_lock` so they can `lock.unlock()` before invoking callbacks, preventing deadlocks from re-entrant access.

### SO_RCVTIMEO must be set BEFORE `connect()`
`TransferSender::send_file()` sets `SO_RCVTIMEO` (500ms) on the socket *before* `connect()`. Setting it after the connection is established has no effect on some Linux kernels. The sender uses this for blocking `recv()` on ACKs instead of `select()` + non-blocking socket, which avoids select-to-recv race conditions on localhost.

### Sender ACK timeout for small files
The sender polls for ACK after sending all chunks. For files with few chunks (≤8), the receiver may not send intermediate ACKs, so the sender waits up to `no_ack_cycles > 5` iterations × ~2s (SO_RCVTIMEO + 50ms sleep) ≈ 10s before force-completing. A 50ms `sleep_for` is inserted between chunk sends and ACK reads to give the receiver time to process.

### Web frontend static files
The HTTP server serves files from `src/web/static/` (relative to working directory). The `index.html` expects `style.css` and `app.js` in the same directory. CORS headers are set to `*` for browser-based API access. The server is minimal — no HTTPS, no compression, no HTTP/2.

### TransferManager bridges services
`TransferManager` owns the task map (thread-safe). `TransferReceiver` calls `mark_complete()` on finish. The REST API reads tasks from `TransferManager` for the web UI. New transfer tasks should be created via `TransferManager::add_task()` so they appear in the web transfer list.

## Git Conventions
- Branch: `p2p`
- Commit prefix: `feat:`, `fix:`, `init:` (Chinese descriptions)
- Each completed module phase gets its own commit after unit verification passes
