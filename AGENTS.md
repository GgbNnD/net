# AGENTS.md — P2P File Transfer Tool

## Build & Run

```bash
cmake -B build -S .          # re-run when adding .cpp (GLOB_RECURSE snapshots)
cmake --build build -j$(nproc)
./build/bin/P2PFileTransfer   # unit tests run silently, then services enter main loop
```

## Architecture

```
src/main.cpp           → Entry point, inline unit tests, REST API, probe thread
src/common/            → Shared types, platform socket wrapper, utils, JSON protocol
src/discovery/         → UDP multicast discovery (port 8888)
src/signaling/         → TCP signaling channel (port 8889, short-connection)
src/transfer/          → TCP file transfer (port 8890, sliding window, 64KB chunks)
src/web/               → HTTP server + REST API + static frontend (port 8891)
src/web/static/        → index.html, app.js, style.css (WeChat-style chat UI)
lib/nlohmann/json.hpp  → Vendored JSON library (v3.11.3)
```

Each layer runs in its own thread(s). All services register callbacks rather than direct coupling.

## Port Cleanup

**Must use SIGKILL (-9).** SIGTERM only sets `g_running = false` but `recv_loop()` blocks on `recvfrom()` and won't wake until the socket is closed. Without -9 the process hangs indefinitely.

```bash
kill -9 $(pgrep -f P2PFileTransfer)
sleep 1   # wait for sockets to close
```

## Defaults (src/common/types.h:90-99)

| Constant | Value | Purpose |
|----------|-------|---------|
| `DISCOVERY_PORT` | `8888` | UDP multicast |
| `SIGNALING_PORT` | `8889` | TCP control |
| `TRANSFER_PORT` | `8890` | TCP file transfer |
| `HTTP_PORT` | `8891` | Web UI |
| `DEVICE_TIMEOUT` | `10` | Device timeout (seconds) |
| `BROADCAST_INTERVAL` | `3` | UDP broadcast interval (seconds) |
| `CHUNK_SIZE` | `65536` | 64KB transfer chunks |
| `WINDOW_SIZE` | `16` | Sliding window |
| `MULTICAST_ADDR` | `239.255.255.250` | Multicast group |

## REST API Endpoints

| Method | Path | Key params | Purpose |
|--------|------|------------|---------|
| GET | `/api/devices` | — | Online device list (+ `online` bool, `manual` flag) |
| GET | `/api/transfers` | — | Transfer tasks (+ `target_ip`, `is_sender`, progress) |
| POST | `/api/message` | `target_ip`, `text` | Send text via signaling channel |
| POST | `/api/messages/poll` | `ip` | Fetch + clear received texts for given IP |
| POST | `/api/transfer` | `target_ip`, `filename`, `filedata`(base64) | Start file send |
| POST | `/api/peers/add` | `ip` | Manual add by IP (deduped, persists to `known_devices.json`) |
| POST | `/api/peers/remove` | `ip` | Remove + update `known_devices.json` |

Note: `/api/message` and `/api/transfer` use `target_ip`; `/api/messages/poll` and `/api/peers/add` use `ip`. `on_get()` has no query-param support — use POST for parameterized requests.

## Testing (inline in main.cpp)

1. **Phase 1** — UUID, MD5, formatting, protocol message construction
2. **Phase 2** — DeviceManager add/remove/timeout callbacks (no network)
3. **Phase 3** — SignalingServer + SignalingClient round-trip on localhost:**18889**
4. **Phase 4** — Chunk serde, FileChunkIO local I/O, e2e on localhost:**18890**

Test output is suppressed at startup (cout redirected to `/dev/null`, `main.cpp:1104`). Tests are `void` functions — failures print to cout but do not prevent service startup. Phase 4 e2e uses a 100KB file (2 chunks); does NOT test single-chunk edge cases.

## Key Implementation Details

### UDP broadcast port (CRITICAL)
`DeviceDiscovery::send_loop()` sends `Defaults::SIGNALING_PORT` (8889) in the broadcast message, NOT `m_port` (8888). This is because the probe thread connects to `device.port` for TCP DEVICE_HELLO, and the TCP server listens on port 8889. Broadcasting 8888 would cause all DEVICE_HELLO probes to fail.

### Probe thread probes ALL devices
`main.cpp:1131-1168` — the peer probe thread runs every 5s and calls `test_connect()` for **all** devices (not just `d.manual` ones). Previously it skipped auto-discovered devices (`if (!d.manual) continue;`), preventing mutual discovery.

### Signaling server ACK responses
`signaling_server.cpp:264-283` — `TEXT_MESSAGE` and `TRANSFER_*` control messages must send an ACK response before closing the socket. `SignalingClient::send_request()` waits for `recv_json_message()`; without a response, `recv_json_message` fails on connection close and returns false even though the message was delivered. DEVICE_HELLO does NOT need an ACK (consumed by `test_connect()` which uses `shutdown(SHUT_WR)` + drain).

### Multicast route (required)
UDP multicast to `239.255.255.250:8888` needs a route. Without it, multicast packets are silently dropped.
```bash
sudo ip route add 224.0.0.0/4 dev <interface>
```
Verify: `ip route show | grep 224`

### WiFi client isolation
Enterprise/campus WiFi APs often block multicast between wireless clients. Fallback: `known_devices.json` + TCP `DEVICE_HELLO` probe (unicast on port 8889) still works.

### select_local_ip() uses UDP connect trick
Connects a temp socket to `1.1.1.1:53` to trigger kernel routing, then `getsockname()` to get the primary interface IP. Avoids docker/virtual IPs. Falls back to first `get_local_ips()` result.

### Multicast join on ALL interfaces
`create_socket()` calls `IP_ADD_MEMBERSHIP` for every non-loopback IP, plus a fallback join on `m_local_ip`.

### Mutual discovery via DEVICE_HELLO
`SignalingClient::test_connect()` sends a `DEVICE_HELLO` JSON message before closing. The receiving `SignalingServer` calls `m_device_hello_cb` which adds/updates the sender via `DeviceManager` with `manual=true`. The sender's side stays at its original `manual` state.

### Device deduplication by IP
`DeviceManager::find_device_id_by_ip(ip)` finds existing devices by IP. Both the `DEVICE_HELLO` callback and `/api/peers/add` check this before creating new entries. Self-IPs are filtered out via `get_local_ips()`.

### Device persistence (known_devices.json)
Only devices with `manual=true` are saved to `known_devices.json`. On startup, `load_known_devices()` loads them (skipping self-IPs, deduplicating). `save_known_devices()` also deduplicates before writing. Load happens before discovery service starts. Both functions must be forward-declared before `init_discovery_service()`.

### Auto-discovered devices (manual=false)
- Probed by the peer probe thread every 5s ✓
- Timed out after `DEVICE_TIMEOUT` (10s) by `DeviceManager::cleanup_loop()` ✓
- NOT persisted to `known_devices.json` (only manual devices are)
- Displayed in Web UI with `manual: false`

### Offline = last_seen > 10s ago
`Defaults::DEVICE_TIMEOUT = 10` seconds. Manual devices (`manual=true`) are exempt from timeout cleanup.

### Single-chunk receiver bug (FIXED)
Original loop: `get_max_contiguous_chunk() < total_chunks - 1` → `0 < 0` for single-chunk → never executes, empty file committed as "complete" → MD5 mismatch. Now uses `while (chunk_count < meta.total_chunks)`.

### FileChunkIO receiver init order
Must `open()+ftruncate()` the `.tmp` file BEFORE constructing `FileChunkIO` (reads file size from disk). The transfer receiver handles this in `handle_receive()`.

### TransferTask creation on receive
`TransferReceiver::set_on_receive_start()` fires when the file header arrives. The callback creates a `TransferTask` with `is_sender=false` and `target.ip=sender_ip`.

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
