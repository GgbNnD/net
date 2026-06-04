# AGENTS.md — P2P File Transfer Tool

## Build & Run

```bash
cmake -B build -S .          # re-run when adding .cpp (GLOB_RECURSE snapshots)
cmake --build build -j$(nproc)
./build/bin/P2PFileTransfer   # tests run at startup, then services enter main loop
```

## Architecture

```
src/main.cpp           → Entry point, inline unit tests, REST API, probe thread
src/common/            → Shared types, platform socket wrapper, utils, JSON protocol
src/discovery/         → UDP multicast device discovery (port 8888)
src/signaling/         → TCP signaling channel (port 8889, short-connection)
src/transfer/          → TCP file transfer (port 8890, sliding window, 64KB chunks)
src/web/               → HTTP server + REST API + static frontend (port 8891)
src/web/static/        → index.html, app.js, style.css (WeChat-style chat UI)
lib/nlohmann/json.hpp  → Vendored JSON library (v3.11.3)
```

Each layer runs in its own thread(s). All services register callbacks rather than direct coupling.

## Testing (inline in main.cpp)

1. **Phase 1** — UUID, MD5, formatting, protocol message construction
2. **Phase 2** — DeviceManager add/remove/timeout callbacks (no network)
3. **Phase 3** — SignalingServer + SignalingClient round-trip on localhost:**18889**
4. **Phase 4** — Chunk serde, FileChunkIO local I/O, e2e on localhost:**18890**

If a test fails (e.g., port in use), the program still starts live services. Phase 4 e2e uses a 100KB file (2 chunks); it does NOT test single-chunk edge cases.

## Port Cleanup

```bash
sudo fuser -k 18889/tcp 18890/tcp 8888/tcp 8889/tcp 8890/tcp
# or:
kill -9 $(pgrep -f P2PFileTransfer)
sleep 2   # wait for TIME_WAIT
```

## REST API Endpoints

| Method | Path | Purpose |
|--------|------|---------|
| GET | `/api/devices` | Online device list (+ `online` bool, `manual` flag) |
| GET | `/api/transfers` | Transfer tasks (+ `target_ip`, `is_sender`, progress) |
| POST | `/api/transfer` | Start file send (body: url-encoded, `filedata` = base64) |
| POST | `/api/message` | Send text via signaling channel |
| POST | `/api/messages/poll` | Fetch + clear received texts for given IP |
| POST | `/api/peers/add` | Manual add by IP (deduped, persists to `known_devices.json`) |
| POST | `/api/peers/remove` | Remove + update `known_devices.json` |

`on_get()` has no query-param support — use POST for parameterized requests.

## Key Implementation Details

### Multicast route (required)
UDP multicast to `239.255.255.250:8888` needs a route. Without it, discovery silently fails.
```bash
sudo ip route add 224.0.0.0/4 dev <interface>
```
Verify with: `ip route show | grep 224`

### select_local_ip() uses UDP connect trick
Connects a temp socket to `1.1.1.1:53` to trigger kernel routing, then `getsockname()` to get the primary interface IP. Avoids picking docker/virtual IPs. Falls back to first `get_local_ips()` result.

### Multicast join on ALL interfaces
`create_socket()` calls `IP_ADD_MEMBERSHIP` for every non-loopback IP, not just the selected one. This ensures multicast packets arrive regardless of which interface they come in on.

### Mutual discovery via DEVICE_HELLO
`SignalingClient::test_connect()` sends a `DEVICE_HELLO` JSON message before closing. The receiving `SignalingServer` calls `m_device_hello_cb` which auto-adds the sender via `DeviceManager`. Both sides see each other after a single manual add.

### Device deduplication by IP
`DeviceManager::find_device_id_by_ip(ip)` finds existing devices by IP. Both the `DEVICE_HELLO` callback and `/api/peers/add` check this before creating new entries. Also skip self-IP (local addresses are filtered out).

### Device persistence (known_devices.json)
Manual/auto-discovered devices saved to `known_devices.json`. On startup, `load_known_devices()` loads them, skipping self-IPs and deduplicating by IP. `save_known_devices()` also deduplicates before writing. Both functions must be forward-declared before `init_discovery_service()`.

### Peer probe thread
Runs every 5 seconds, calls `test_connect()` with local device info. On success, updates `last_seen` (keeping device "online" for 15s). On failure, does NOT remove the device — `last_seen` ages out, web UI shows gray dot. Offline transitions logged as `[事件] 设备离线`.

### Offline = last_seen > 15s ago
Both terminal main loop and `/api/devices` use `(now - last_seen) > 15s` as the online/offline threshold. Offline devices stay in the list permanently; only manual removal deletes them.

### Single-chunk receiver bug (FIXED)
The original loop condition `get_max_contiguous_chunk() < total_chunks - 1` evaluates to `0 < 0` for single-chunk files, so the receive loop never executes, yet the empty file is committed as "complete" → MD5 mismatch. Now uses `while (chunk_count < meta.total_chunks)`.

### FileChunkIO receiver init order
Must `open()+ftruncate()` the `.tmp` file BEFORE constructing `FileChunkIO` (reads file size from disk). The transfer receiver handles this in `handle_receive()`.

### TransferTask creation on receive
`TransferReceiver::set_on_receive_start()` fires when the file header arrives. The callback creates a `TransferTask` with `is_sender=false` and `target.ip=sender_ip`, so the web UI shows received files with progress bars.

### Non-blocking connect with timeout
`SignalingClient::connect_to()` uses: `set_nonblocking()` → `connect()` (expect `EINPROGRESS`) → `select()` with timeout → `getsockopt(SO_ERROR)` → `set_blocking()`.

### `is_would_block()` must include `EINPROGRESS`
`platform.cpp:is_would_block()` checks `EINPROGRESS` (Linux errno 115) alongside `EWOULDBLOCK`/`EAGAIN`.

### Handler threads: no `detach()`
Both `SignalingServer` and `TransferReceiver` join handler threads in `stop()`. Never `detach()` — causes port leaks.

### SO_RCVTIMEO before `connect()`
`TransferSender::send_file()` sets `SO_RCVTIMEO` (500ms) *before* `connect()`. Setting it after has no effect on some Linux kernels.

### Protocol wire format
| Marker | Description | Payload |
|--------|-------------|---------|
| `'J'` | JSON message | 4-byte BE length + JSON |
| `'C'` | Binary chunk | 16-byte header + variable body |

Signaling channel (port 8889) uses JSON only (no marker, short-connection). Data channel (port 8890) uses the 1-byte marker prefix.

### Message types
`DEVICE_BROADCAST`, `DEVICE_OFFLINE` (UDP multicast), `DEVICE_HELLO` (TCP mutual discovery), `FILE_REQUEST`, `FILE_RESPONSE`, `TEXT_MESSAGE` (chat), plus transfer control/ack types. Defined in `protocol.h:MsgType`.

### HTTP server body reading
`handle_client()` reads until `\r\n\r\n`, then extracts `Content-Length` and continues reading until full body received. 10s `SO_RCVTIMEO`. Maximum 65536-byte header block.

### Base64 file upload
Frontend uses `FileReader.readAsDataURL()`, strips the `data:...;base64,` prefix, `encodeURIComponent()`s the base64, and sends as URL-encoded form field `filedata`. Server `url_decode()`s and `base64_decode()`s, writes to `/tmp/p2p_send/<filename>`, starts `TransferSender`.

### Web UI is WeChat-style chat
Left sidebar: device list (avatars, green/gray status dots). Right: message bubbles (blue=sent, gray=received). File bubbles show progress bars + done status. Text polling every 2s via `POST /api/messages/poll`. Transfer polling every 1s via `GET /api/transfers`.

### Auto-open browser
`system("xdg-open http://localhost:8891 2>/dev/null &")` after HTTP server starts.

### `json` vs `nlohmann::json`
`protocol.h` defines `using json = nlohmann::json`. New headers should use fully qualified `nlohmann::json` to avoid include-order dependency.

## Git Conventions
- Branch: `p2p`
- Commit prefix: `feat:`, `fix:`, `init:` (Chinese descriptions)
- Each module phase gets its own commit after unit tests pass
