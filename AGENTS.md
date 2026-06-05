# AGENTS.md — P2P Bluetooth File Transfer Tool

## Build & Run

```bash
# Prerequisites (one-time)
sudo apt install -y libbluetooth-dev   # provides <bluetooth/bluetooth.h> and AF_BLUETOOTH

# Build
cmake -B build -S .                    # re-run when adding .cpp (GLOB_RECURSE snapshots)
cmake --build build -j$(nproc)

# Run
./build/bin/P2PFileTransfer            # unit tests run silently, then services enter main loop
```

## Architecture

```
src/main.cpp           → Entry point, inline unit tests, REST API, probe thread
src/common/            → Shared types, platform socket wrapper, utils, JSON protocol
src/bluetooth/         → Bluetooth I/O layer
  bt_utils             → D-Bus connection, BDADDR conversion, RFCOMM socket helpers, auto-pair agent
  bt_discovery         → BlueZ D-Bus device scanning (replaces old UDP multicast)
  bt_signaling_server  → RFCOMM channel 1 listener (replaces old TCP port 8889)
  bt_signaling_client  → RFCOMM channel 1 client (non-blocking connect + DEVICE_HELLO)
  bt_transfer_sender   → RFCOMM channel 2 outbound (sliding window, 64KB chunks)
  bt_transfer_receiver → RFCOMM channel 2 listener (writes to disk, sends ACKs)
src/discovery/         → DeviceManager only (in-memory registry, timeout cleanup)
src/transfer/          → TransferManager, FileChunkIO, Chunk serialization (transport-agnostic)
src/web/               → HTTP server + REST API + static frontend (port 8891)
src/web/static/        → index.html, app.js, style.css (WeChat-style chat UI)
lib/nlohmann/json.hpp  → Vendored JSON library (v3.11.3)
```

Each layer runs in its own thread(s). All services register callbacks rather than direct coupling.

## Device Addressing

All addresses are **BDADDR** strings ("AA:BB:CC:DD:EE:FF"), not IPv4.

- `DeviceInfo.addr` — Bluetooth address (was `.ip` before migration)
- `DeviceInfo.port` — RFCOMM channel number (1 = signaling, 2 = transfer)
- `DeviceInfo.connected` — true after successful RFCOMM DEVICE_HELLO handshake (green dot in UI)
- `DeviceInfo.last_probed` — timestamp of last RFCOMM probe attempt (default epoch = trigger immediate first probe)
- `DeviceManager::find_device_id_by_addr(addr)` — deduplication by BDADDR

## Channel Numbers

| Channel | Purpose | Defaults constant |
|---------|---------|-------------------|
| 1       | Signaling (file negotiation, chat, DEVICE_HELLO) | `SIGNALING_CHANNEL` |
| 2       | File transfer (binary chunks, sliding window) | `TRANSFER_CHANNEL` |
| 8891    | HTTP Web UI (localhost) | `HTTP_PORT` |

## D-Bus & BlueZ Integration

The discovery layer talks to BlueZ over the **D-Bus system bus** (`dbus_bus_get_private`).

- **Adapter path**: found via `ObjectManager.GetManagedObjects` → first object with `org.bluez.Adapter1`
- **StartDiscovery / StopDiscovery**: called on the adapter object
- **Device signals**: `InterfacesAdded` → new device, `PropertiesChanged` → RSSI updates
- **Auto-pair agent**: registers on `/org/bluez` root path (NOT on adapter path), capability `NoInputNoOutput`
- **Dispatch thread**: calls `dbus_connection_read_write_dispatch(conn, 100ms)` in a loop, checks `m_running` each iteration — no need for `kill -9`, SIGTERM works

## RFCOMM Sockets

RFCOMM sockets (`AF_BLUETOOTH`, `SOCK_STREAM`, `BTPROTO_RFCOMM`) are standard UNIX file descriptors. All existing `Protocol::send_json_message()`, `recv_json_message()`, `SOCK_SEND`, `SOCK_RECV`, `select()`, `fcntl(O_NONBLOCK)`, `setsockopt(SO_RCVTIMEO)` work unchanged.

Server bind: `BDADDR_ANY` (zeroed `bdaddr_t`), not a string.
```cpp
bdaddr_t bdaddr_any = {0};
bacpy(&addr.rc_bdaddr, &bdaddr_any);   // NOT &BDADDR_ANY — that's a compound literal rvalue
```

## Auto-Connection & Green/Gray Dot

The **peer probe thread** (3s cycle, max 3 probes/cycle, 1s timeout) probes ALL devices — not just manual ones:
- Connected devices: re-probed every 15s (light touch, keeps alive)
- Unconnected devices: re-probed every 60s (avoid flooding)
- On success → sets `connected = true`, `last_seen = now` (green dot)
- On failure → sets `connected = false` (gray dot, not yet paired)

`DEVICE_HELLO` callback also sets `connected = true` — the receiving side marks the sender as connected. This gives **bidirectional** green dots as soon as either side probes first.

**Important**: `DeviceManager::update_device()` only overwrites `connected`/`last_probed` when the incoming device's `last_probed` is non-epoch. This prevents Bluetooth scan results (`connected=false` default) from overwriting probe results.

## REST API Endpoints

| Method | Path | Key fields (body / response) |
|--------|------|------------------------------|
| GET | `/api/devices` | `addr`, `connected`, `online`, `manual` |
| GET | `/api/transfers` | `target_addr`, `is_sender`, `progress_chunk`, `speed` |
| POST | `/api/transfer` | body: `target_addr`, `filename`, `filedata` (base64) |
| POST | `/api/message` | body: `target_addr`, `target_channel`, `text` |
| POST | `/api/messages/poll` | body: `addr` → returns `{messages: [{text, time}]}` |
| POST | `/api/peers/add` | body: `addr`, `name` → deduped by `find_device_id_by_addr()` |
| POST | `/api/peers/remove` | body: `addr` |

`on_get()` has no query-param support — use POST for parameterized requests.

## Testing (inline in main.cpp)

1. **Phase 1** — UUID, MD5, formatting, BDADDR conversion, protocol message construction
2. **Phase 2** — DeviceManager add/remove/timeout callbacks (uses fake BDADDRs like "AA:BB:CC:DD:EE:01")
3. **Phase 3** — `BtSignalingServer` + `BtSignalingClient` self-connect on local RFCOMM channel 10; falls back gracefully if adapter unpaired
4. **Phase 4** — Chunk serde, FileChunkIO local I/O; RFCOMM e2e test skipped (needs real BT connection)

Test output is suppressed at startup (cout redirected to `/dev/null`, `main.cpp:~1020`). Tests are `void` functions — failures print to cout but do not prevent service startup.

## Key Implementation Details

### Protocol wire format (unchanged from LAN version)
| Marker | Description | Payload |
|--------|-------------|---------|
| `'J'` | JSON message | 4-byte BE length + JSON |
| `'C'` | Binary chunk | 16-byte header + variable body |

Signaling channel (RFCOMM 1) uses JSON only (no marker, short-connection). Data channel (RFCOMM 2) uses the 1-byte marker prefix.

### Non-blocking connect with timeout
`BtUtils::rfcomm_connect()` uses the same pattern: `fcntl(O_NONBLOCK)` → `connect()` (expect `EINPROGRESS`) → `select()` with timeout → `getsockopt(SO_ERROR)` → `fcntl(reset flags)`. Works identically on `AF_BLUETOOTH` sockets.

### `is_would_block()` must include `EINPROGRESS`
`platform.cpp:is_would_block()` checks `EINPROGRESS` (Linux errno 115) alongside `EWOULDBLOCK`/`EAGAIN`.

### Handler threads: no `detach()`
Both `BtSignalingServer` and `BtTransferReceiver` join handler threads in `stop()`. Never `detach()` — causes fd leaks.

### SO_RCVTIMEO before `connect()`
`BtTransferSender::send_file()` sets `SO_RCVTIMEO` (500ms) *before* `connect()`. Setting it after has no effect on some Linux kernels. Same for RFCOMM.

### FileChunkIO receiver init order
Must `open()+ftruncate()` the `.tmp` file BEFORE constructing `FileChunkIO` (reads file size from disk). The transfer receiver handles this in `handle_receive()`.

### Bluetooth scan interval
BtDiscovery internal timeout is 15s (matching `Defaults::DEVICE_TIMEOUT`). Non-manual devices that haven't been seen for 15s are removed from the in-memory cache.

### Device persistence (`known_devices.json`)
Manual devices saved to `known_devices.json`. Field keys: `"id"`, `"name"`, `"addr"`, `"port"`. On startup, `load_known_devices()` skips self-BDADDR (`g_local_addr`) and deduplicates by addr. `save_known_devices()` also deduplicates before writing.

### `json` vs `nlohmann::json`
`protocol.h` defines `using json = nlohmann::json`. New headers should use fully qualified `nlohmann::json` to avoid include-order dependency.

### HTTP server body reading
`handle_client()` reads until `\r\n\r\n`, then extracts `Content-Length` and continues reading until full body received. 10s `SO_RCVTIMEO`. Maximum 65536-byte header block.

### Base64 file upload
Frontend uses `FileReader.readAsDataURL()`, strips the `data:...;base64,` prefix, `encodeURIComponent()`s the base64, and sends as URL-encoded form field `filedata`. Server `url_decode()`s and `base64_decode()`s, writes to `/tmp/p2p_send/<filename>`, starts `BtTransferSender`.

### Web UI is WeChat-style chat
Left sidebar: device list (avatars, green=connected / gray=not-paired status dots). Right: message bubbles (blue=sent, gray=received). File bubbles show progress bars + done status. Text polling every 2s via `POST /api/messages/poll`. Transfer polling every 1s via `GET /api/transfers`.

### Auto-open browser
`system("xdg-open http://localhost:8891 2>/dev/null &")` after HTTP server starts.

## Git Conventions
- Branch: `BT`
- Commit prefix: `feat:`, `fix:`, `docs:`, `init:` (Chinese descriptions)
- Each module phase gets its own commit
