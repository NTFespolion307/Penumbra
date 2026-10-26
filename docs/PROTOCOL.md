# Penumbra wire protocol

Protocol version **4** (`kProtocolVersion` in `core/version.h`). Version 2 added scene packages (`SceneHave`, `SceneChunk`, package fields in `SceneOffer`); version 3 added skipped devices to `WorkerReady` (M7); version 4 added the round trip, upload rate and scene cache use to `Stats` (M10). The schema is `net/schema/penumbra.fbs`; this
file explains it and must change in the same commit as the schema. Any change to a message bumps the protocol
version.

## Connection

- One TCP connection per worker host, opened by the worker to the controller (default port 7420). TCP keepalive
  on, Nagle off.
- **Encryption:** TLS 1.3 (mbedTLS) with an external pre-shared key and nothing else: no certificates, no older
  TLS versions. Key = HKDF-SHA256(join code, 16 canonical characters; no salt; info `penumbra-join-v1`), 32
  bytes; PSK identity `penumbra-join-v1`. Key exchange mode `psk_dhe_ke` only (ECDHE on top of the PSK, so a
  recorded session stays secret even if the code leaks later). Cipher suites: AES-256-GCM-SHA384,
  ChaCha20-Poly1305-SHA256, AES-128-GCM-SHA256. The code itself is never sent; a peer with another code fails the
  handshake (the PSK binder does not verify). A handshake must complete within 10 s.
- **Failed joins:** the controller counts failed handshakes per IP address; more than 5 within a minute block that
  address for 10 minutes (its connections are closed at once).
- Every message is one **frame**, inside the TLS stream. Integers are little-endian.

```
magic "PNBR" (4 bytes) | protocol version u16 | message type u16 | flags u16 | payload length u32 | payload
```

- The payload is one FlatBuffers table, named by the message type (table below). It is at most 16 MB.
- **Flags:** bit 0 is reserved for zstd compression of the whole payload (unused in version 1). Bit 1 marks a
  **part frame**.
- **Part frames:** a message bigger than 4 MB is sent as part frames of at most 4 MB. Each part's payload
  starts with `transfer id u32 | total length u32 | offset u32`, followed by the bytes. Parts of one message come
  in order. Only bulk messages (tile results, later scene data) are split. The sender keeps two queues and always
  sends a waiting control frame next, so a control message never waits behind more than one part. A reassembled
  message may be at most 512 MB.
- **Versions:** the controller answers a `Hello` whose frame version, app version or git hash differs from its own
  with `Reject` (`version_mismatch`) and closes the connection. Tiles from different builds may differ, so every
  machine must run the same commit.

## Messages

| Type | Code | Direction | Contents and purpose |
| --- | --- | --- | --- |
| `Hello` | 1 | W → C | Protocol + app version, git hash, worker name, OS, CPU, cores, RAM, free cache disk, devices (name, backend, threads, memory, RT support). First message on a connection. |
| `Welcome` | 2 | C → W | Session id, worker id, controller name, heartbeat interval (ms). |
| `Reject` | 3 | C → W | Reason (`VersionMismatch`, `NotApproved`, `SessionFull`, `SessionClosed`, `Kicked`, `BadMessage`) and text; the controller then closes. With `--approve-workers` the controller asks its user before `Welcome`; no answer means `NotApproved`. |
| `SceneOffer` | 10 | C → W | Render id, full `RenderSettings` as JSON (paths as on the controller), pixel settings hash, scene hash, film size, package hash and manifest (below). |
| `SceneHave` | 11 | W → C | Render id and `Cached`, `Need` with the first missing chunk index (a resumed download), or `Rejected` with a reason (not enough disk or RAM, bigger than the cache). |
| `SceneChunk` | 12 | C → W | Package hash, chunk index, uncompressed size, zstd-compressed bytes. Bulk: sent in order from the first missing chunk. |
| `WorkerReady` | 13 | W → C | Render id; per device: backend, memory tier, build time, summary, or `skipped` with a reason (protocol 3: a device that cannot take the scene; the worker renders with the others). Or `scene_rejected` with a reason when no device can. A worker gets no lease before this, and none for a skipped device. |
| `RequestWork` | 20 | W → C | Device id and how many more items it can take. A request replaces the device's previous one (D-049). |
| `Lease` | 21 | C → W | Lease id, device id, item id (`tile << 32 \| chunk`), tile rectangle, sample range, settings hash. |
| `TileResult` | 22 | W → C | Lease id, device id, item id, rectangle, sample range, encoding, pixels, NaN count, render time, BLAKE3-64 checksum of the decoded floats. |
| `TileAck` | 23 | C → W | Lease id, `Accepted` / `Duplicate` / `Rejected` (+ reason). |
| `Release` | 24 | W → C | Lease ids handed back unfinished (device failed, worker leaving, lease it cannot take); requeued at once. |
| `DeviceFailed` | 25 | W → C | Device id and error: that device is removed, the worker continues with the others. Not in the guide's table; added so the controller can log and count it like a local device failure. |
| `Cancel` | 30 | C → W | `all`, or lease ids. Version 1 controllers only send `all` (stop, Ctrl+C, end of render). |
| `Pause` | 31 | C → W | `paused` true: stop requesting work; false: continue (the guide's `Pause` / `Resume`). |
| `RenderComplete` | 32 | C → W | The render is over. Workers without `--stay` say `Bye` and exit 0. |
| `Stats` | 40 | W → C | Every 2 s: CPU %, RAM used/total, upload-bound flag; per device busy %, VRAM, temperature (< -273 = unknown), tiles/s, samples/s. Protocol 4: the last heartbeat round trip (ms), upload bytes/s, and the scene cache's used bytes, limit and number of scenes (the UI's Hosts pages). |
| `Heartbeat` | 41 | both | The worker sends one every 5 s (its clock in µs); the controller answers at once with its own clock and the worker's value as `echo_us`. The echo gives the worker the round trip: above 50 ms each device keeps 3 items in flight instead of 2. The controller also sends its own heartbeat to every worker every 5 s (`echo_us` 0, ignored for the round trip): on a slow upload the worker's heartbeat waits behind tiles already in the network's buffers, and its echo alone could come back after 30 s (D-103). Either side drops the connection after 30 s without receiving anything, and the controller requeues that worker's leases. |
| `Log` | 42 | W → C | Level and text, shown in the controller's log. |
| `Bye` | 50 | both | Polite disconnect with a reason; the sender then closes its side. |
| `Kick` | 51 | C → W | Forced disconnect with a reason (worker exits with code 6). Sent after 3 rejected tile results. |

## Scene packages

The controller packages the scene before every render (also solo renders, where it is the check that every
file exists). The **manifest** (`core/package/package.h`) is JSON: format version, Penumbra version, the scene
file and HDRI as package-relative paths, and every file the scene reads (glTF buffers and images, OBJ material
libraries and `map_Kd` textures, the HDRI) with its size and BLAKE3-64 hash, relative to the deepest folder that
holds them all. The package is those files laid end to end, cut into 4 MB chunks; the manifest lists each
chunk's hash. The **package hash** is the BLAKE3-64 of the manifest. Render settings are not in the manifest,
so the same scene files are one package whatever the settings.

The worker answers a `SceneOffer` with `SceneHave`. If its cache holds the package, `Cached`. If not, it checks
the cache size limit, free disk (twice the package) and RAM, and answers `Rejected` with the reason or `Need`
with the first chunk it does not have yet. The controller then sends `SceneChunk`s in order: never more than 2
chunks queued per connection, several workers round-robin, all of them together under `--upload-limit` (bytes
on the wire per second). The worker checks each chunk against the manifest, writes it to
`<cache>/<hash>.partial`, and after the last one unpacks the files, checks every file hash, and moves them to
`<cache>/<hash>/`. A worker that loses the connection keeps the verified chunks and resumes there next time.
Manifest paths with `..`, drive letters or absolute paths are refused.

## Tile encoding

`TileEncoding.Float32Shuffled` (DECISIONS D-053): the tile's RGB means as float32, rearranged into 4 byte planes
(all first bytes, then all second bytes, ...), then zstd level 3. It is lossless, so a tile from any host merges
to the same bits as a local one. `Float16Shuffled` is reserved for a possible bandwidth mode (M9). The checksum
covers the decoded floats; a result that fails to decode, fails its checksum, or does not match its lease (item,
rectangle, sample range, pixel count) is answered with `Rejected`, and its item is requeued.

## A render, message by message

```
W+C: TLS 1.3 handshake (join-code key)
W: Hello                      C: Welcome
C: SceneOffer                 W: SceneHave(cached | need k | rejected)
C: SceneChunk k, k+1, ...     W: (unpacks, loads scene, builds BVH per device) WorkerReady
W: RequestWork(dev 0, 2)      C: Lease, Lease
W: TileResult                 C: TileAck(accepted)        ... repeated
W: Stats every 2 s
C: Cancel(all), RenderComplete
W: Bye, closes                C: closes
```

A worker that joins during a render gets the `SceneOffer` right after `Welcome` and joins the pool as soon as it
is ready. A worker whose connection drops has all its leases requeued.
