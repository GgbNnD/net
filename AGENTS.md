# AGENTS.md — P2P File Transfer Tool

## Build & Run

```bash
cmake -B build -S .                      # re-run when adding .cpp (GLOB_RECURSE snapshots)
cmake --build build -j$(nproc)
./build/bin/P2PFileTransfer              # main server: unit tests → services → main loop
./build/bin/P2PBench [size_mb] [threads] # localhost benchmark
./build/bin/P2PBench --recv <port>       # receiver-only mode (for cross-machine testing)
./build/bin/P2PBench --send <ip> <size> <threads> <port>  # sender-only mode
./build/bin/P2PBench --gen <size_mb>     # generate test file only
```

## Port Cleanup

**Must use SIGKILL (-9).** SIGTERM only sets `g_running = false` but `recv_loop()` blocks on `recvfrom()` and won't wake until the socket is closed.

```bash
kill -9 $(pgrep -f P2PFileTransfer)
kill -9 $(pgrep -f P2PBench)
sleep 1
```

## Architecture

```
src/main.cpp           → Entry point, inline unit tests, REST API, probe thread
src/common/            → Shared types, platform socket wrapper, utils (crypto), JSON protocol
src/discovery/         → UDP multicast discovery (port 8888)
src/signaling/         → TCP signaling channel (port 8889, short-connection, ECDH handshake)
src/transfer/          → TCP file transfer (port 8890, multi-thread, BBR, AES-256-GCM)
src/web/               → HTTP server + REST API + static frontend (port 8891)
src/web/static/        → index.html, app.js, style.css (WeChat-style chat UI)
bench_main.cpp         → Standalone benchmark binary (P2PBench)
lib/nlohmann/json.hpp  → Vendored JSON library (v3.11.3)
```

## Defaults (src/common/types.h)

| Constant | Value | Purpose |
|----------|-------|---------|
| `DISCOVERY_PORT` | `8888` | UDP multicast |
| `SIGNALING_PORT` | `8889` | TCP control |
| `TRANSFER_PORT` | `8890` | TCP data (multi-connection) |
| `HTTP_PORT` | `8891` | Web UI |
| `DEVICE_TIMEOUT` | `10` | Device timeout (seconds) |
| `BROADCAST_INTERVAL` | `3` | UDP broadcast interval (seconds) |
| `CHUNK_SIZE` | `65536` | 64KB transfer chunks |
| `NUM_THREADS` | `4` | Default parallel transfer threads |
| `MULTICAST_ADDR` | `239.255.255.250` | Multicast group |

## REST API Endpoints

| Method | Path | Key params | Purpose |
|--------|------|------------|---------|
| GET | `/api/devices` | — | Online device list (+ `online` bool, `manual` flag) |
| GET | `/api/transfers` | — | Transfer tasks (+ `target_ip`, `is_sender`, progress) |
| POST | `/api/message` | `target_ip`, `text` | Send text (AES-256-GCM encrypted if ECDH key exists) |
| POST | `/api/messages/poll` | `ip` | Fetch + clear received texts for given IP |
| POST | `/api/transfer` | `target_ip`, `filename`, `filedata`(base64) | Start file send (4 threads default) |
| POST | `/api/peers/add` | `ip` | Manual add by IP (deduped, persists to `known_devices.json`) |
| POST | `/api/peers/remove` | `ip` | Remove + update `known_devices.json` |

Note: `/api/message` and `/api/transfer` use `target_ip`; `/api/messages/poll` and `/api/peers/add` use `ip`. `on_get()` has no query-param support — use POST for parameterized requests.

## Unit Tests (inline in main.cpp)

1. **Phase 1** — UUID, MD5 data, formatting, protocol message construction
2. **Phase 2** — DeviceManager add/remove/timeout callbacks (no network)
3. **Phase 3** — SignalingServer + SignalingClient round-trip on localhost:**18889**
4. **Phase 4** — Chunk serde, FileChunkIO local I/O, e2e on localhost:**18890**

Test output is suppressed at startup (cout redirected to `/dev/null`). Tests are `void` functions — failures print to cout but do not prevent service startup. Phase 4 uses a 100KB file (2 chunks); does NOT test single-chunk edge cases.

## Encryption (AES-256-GCM + ECDH)

### Key Exchange

- secp256r1 (NIST P-256) ECDH via OpenSSL `EVP_PKEY_derive`
- Probe thread sends `DEVICE_HELLO` with public key → server replies `DEVICE_HELLO_ACK` with its public key
- Both sides compute shared secret: `local_private × remote_public = shared` (32 raw bytes → AES-256 key)
- Stored in `PeerKey::s_secrets[ip]` (global `std::map`, mutex-protected)
- **No more MD5 hashing** of ECDH output — raw 32 bytes used directly

### AES-256-GCM

- **Every JSON message and file chunk** is encrypted when `PeerKey::get(peer_ip)` is non-empty
- Per-message random 12-byte IV (`RAND_bytes`), 16-byte GCM Tag
- OpenSSL `EVP_aes_256_gcm()` API in `utils.cpp:aes_gcm_encrypt/aes_gcm_decrypt`
- GCM Tag verified on every receive — `EVP_DecryptFinal_ex` returns 0 on mismatch
- **No MD5-MAC**: `sign_message()`, `verify_message()`, `mac_secret_for()` are all removed
- **No file-level MD5 checksum**: removed from `FileMeta`, `send_file_header`, `send_range_header`, and receiver verification — GCM Tag per-chunk provides full integrity

### Wire format markers on data channel (port 8890)

| Marker | Meaning | Payload |
|--------|---------|---------|
| `'J'` | Plain JSON | 4-byte BE len + JSON |
| `'E'` | Encrypted JSON | 4-byte BE (plain_len+28) + 12B IV + ciphertext + 16B Tag |
| `'C'` | Plain chunk | 16-byte header + file_id + data |
| `'D'` | Encrypted chunk | 4-byte BE len + 12B IV + encrypted(serialized_chunk) + 16B Tag |

Signaling channel (port 8889) uses the same JSON wire format but without the 1-byte marker prefix. `recv_json_message` with peer_ip auto-detects encryption: try AES-GCM decrypt first, fall back to plain JSON parse.

## File Transfer (multi-thread + BBR)

### Architecture change from old sliding window

- **Removed**: Sliding window (`send_chunks` with `base`/`next_chunk`/`no_ack_cycles`), `try_recv_ack()` for ACK-based sliding, `m_paused`/`m_cancelled`, `WINDOW_SIZE`
- **Replaced with**: N threads (default 4, `Defaults::NUM_THREADS`), each opens independent TCP connection, BBR congestion control, sends its chunk range

### Transfer flow

1. **Sender** divides `total_chunks` into N ranges, spawns N threads
2. Each thread: `connect()` → `enable_bbr()` → send `FILE_RANGE` JSON header (with `start_chunk`/`end_chunk`/`conn_index`/`total_connections`) → send chunks sequentially → send `RANGE_DONE` → wait for `RANGE_ACK` → close
3. **Receiver**: first connection creates `InboundTransfer` state (file_id → bitmap + `.tmp` file), subsequent connections join the same state. Each handler thread writes chunks via `pwrite()` (thread-safe). When `finished_connections == total_connections` and all chunks received → `rename(.tmp → final)` → `zlib` decompress → fire completion callback
4. **InboundTransfer** uses static `std::map<file_id, InboundTransfer>` with `std::mutex` for thread-safe multi-connection tracking

### BBR congestion control

```cpp
// In send_chunk_range, per-connection:
setsockopt(sock, IPPROTO_TCP, TCP_CONGESTION, "bbr", 3);
```
Requires Linux kernel 4.9+. Falls back silently to default (cubic) if not available.

### Backward compatibility

Receiver handles both `FILE_HEADER` (single-connection, old protocol) and `FILE_RANGE` (multi-connection, new protocol) via message type dispatch in `handle_receive()`.

## Key Implementation Details

### UDP broadcast port (CRITICAL)
`DeviceDiscovery::send_loop()` sends `Defaults::SIGNALING_PORT` (8889) in the broadcast message, NOT `m_port` (8888). Broadcasting 8888 would cause all DEVICE_HELLO probes to fail.

### Probe thread probes ALL devices
`main.cpp:1153-1191` — the peer probe thread runs every 5s and calls `test_connect()` for **all** devices. The probe sends DEVICE_HELLO with public key and waits for DEVICE_HELLO_ACK for bidirectional ECDH.

### Multicast route (required)
```bash
sudo ip route add 224.0.0.0/4 dev <interface>
```

### select_local_ip() uses UDP connect trick
Connects a temp socket to `1.1.1.1:53` to trigger kernel routing, then `getsockname()` to get the primary interface IP.

### Multicast join on ALL interfaces
`create_socket()` calls `IP_ADD_MEMBERSHIP` for every non-loopback IP, plus a fallback join on `m_local_ip`.

### Signaling server ACK responses
`TEXT_MESSAGE`, `TRANSFER_*`, and `DEVICE_HELLO` now all send ACK responses before closing. `DEVICE_HELLO` replies with `DEVICE_HELLO_ACK` containing the server's ECDH public key.

### Non-blocking connect with timeout
`SignalingClient::connect_to()` uses: `set_nonblocking()` → `connect()` → `select()` with timeout → `getsockopt(SO_ERROR)` → `set_blocking()`.

### `is_would_block()` must include `EINPROGRESS`
`platform.cpp:is_would_block()` checks `EINPROGRESS` (Linux errno 115) alongside `EWOULDBLOCK`/`EAGAIN`.

### Handler threads: no `detach()`
Both `SignalingServer` and `TransferReceiver` join handler threads in `stop()`. Never `detach()` — causes port leaks.

### FileChunkIO receiver init order
Must `open()+ftruncate()` the `.tmp` file BEFORE constructing `FileChunkIO` (reads file size from disk).

### Device persistence (known_devices.json)
Only devices with `manual=true` are saved. `load_known_devices()` + `save_known_devices()` must be forward-declared before `init_discovery_service()`.

### Single-chunk receiver bug (FIXED)
Now uses `while (chunk_count < meta.total_chunks)` instead of `get_max_contiguous_chunk() < total_chunks - 1`.

### `json` vs `nlohmann::json`
`protocol.h` defines `using json = nlohmann::json`. New headers should use fully qualified `nlohmann::json` to avoid include-order dependency.

## Git Conventions
- Branch: `p2p`
- Commit prefix: `feat:`, `fix:`, `init:` (Chinese descriptions)
