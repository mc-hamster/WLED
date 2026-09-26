# BLE API Bridge — maintained fork

This usermod exposes WLED's JSON control API over authenticated Bluetooth LE. Use it with the `ble` branch of [mc-hamster/WLED-iOS](https://github.com/mc-hamster/WLED-iOS/tree/ble). Wi-Fi remains available. The firmware supports one connected Bluetooth client at a time.

The implementation targets Arduino-ESP32 3.3.12 / ESP-IDF 5.5.5 and NimBLE-Arduino 2.5.1. ESP8266, ESP32-S2, and ESP32-P4 do not have the required integrated BLE radio. The supported BLE families are classic ESP32 and ESP32-S3. ESP32-C3 is excluded from this fork's BLE support; other radio-capable chips need their own validation. Do not combine this bridge with another usermod that owns the NimBLE device/server.

## Pair an iPhone

1. Install a BLE-enabled build for the board's exact chip, flash size, and PSRAM arrangement.
2. Obtain the device-specific pairing code. Over USB/UART, send the single ASCII character `B` at the configured serial baud rate (normally 115200); the response is `{"ble":{"pairingCode":"NNNNNN"}}`. Alternatively, open **Settings → Usermods → BleApiBridge** over Wi-Fi and note `pairing-code`. Keep Bluetooth enabled.
3. In the forked iOS app, add a device, choose **Bluetooth**, and select the nearby WLED device. A blank firmware `device-name` becomes `WLED-` followed by a unique MAC suffix.
4. Tap **Add**, accept the iOS system pairing prompt, and enter the device's six-digit code. The app reads a protected characteristic before starting its command timeout, allowing time to enter the code.
5. Use the native lighting, effect, palette, scene and playlist controls, or open the complete offline device workspace for settings and tools. The app subscribes to state updates and reconnects while active. A brief background transition is tolerated; longer background periods disconnect to release the device.

Pairing and bond storage belong to iOS. No code is entered into or saved by the app. Initial provisioning can use the physical USB/UART pairing-code query without joining Wi-Fi. Keep the code with the device or print a device label. The query requires the normal WLED serial RX/TX pins to be available; no code is emitted automatically or broadcast over Bluetooth. Wi-Fi credentials can subsequently be configured from the offline Bluetooth settings interface.

The code is generated once per installation and persisted in NVS. It is **no longer universally `123456`**. Changing it to another number from 100000–999999 forgets firmware-side bonds and disconnects connected clients. If iOS reports stale pairing information, forget this WLED device in **Settings → Bluetooth**, then pair again. A failed or cancelled pairing attempt requires an explicit retry; the app avoids repeatedly opening the system prompt.

Only one client can connect. Disconnect reference tools or another phone before adding the device. iOS cannot silently remove a system bond on the user's behalf.

## Build profiles and installation

From the WLED repository root:

```sh
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
npm ci
npm run build
cp usermods/ble_api_bridge/platformio_override.ini.sample platformio_override.ini
.venv/bin/pio run -e esp32dev_ble_api_bridge
```

If a local override already exists, merge the sample into it instead of overwriting it. Ordinary upstream targets do not include this usermod.

The BLE profiles retain their base target's WLED features, libraries, and usermods (including AudioReactive). **OTA is intentionally disabled; firmware updates require USB.** No other WLED features are removed: 2D/matrix effects, GIF, DMX input, IR, Alexa, Hue sync, ESP-NOW, and the remaining base integrations stay enabled. ESP32-C3 is excluded from support.

| Environment | Base hardware | USB-only partition layout |
| --- | --- | --- |
| `esp32dev_ble_api_bridge` | Classic ESP32, 4 MB flash | One 3 MiB app; 960 KiB filesystem |
| `esp32dev_8MB_ble_api_bridge` | Classic ESP32, 8 MB flash | One 4 MiB app; 3,968 KiB filesystem; 64 KiB coredump |
| `esp32s3dev_ble_api_bridge` | ESP32-S3, 8 MB flash, octal PSRAM (`qio_opi`) | One 4 MiB app; 3,968 KiB filesystem; 64 KiB coredump |

See the [review's build results](../../docs/BLUETOOTH_REVIEW.md#verification-performed) for the validation status of each target. Earlier compact builds that removed features or reduced storage have been retired. Do not distribute those images. An S3 board with a different PSRAM type needs a matching base environment.

The two OTA slots are replaced by one factory application partition. NVS and filesystem offsets and capacities stay the same as upstream; 8 MB boards also keep the coredump partition. Wi-Fi configuration, backup/restore, filesystem access, and the full WLED web interface remain available. Neither Wi-Fi OTA nor BLE OTA is available. The firmware reports this through `info.opt` bit 0; the web UI hides its update section and the app shows USB update guidance. Stock release updates remain blocked for recognized BLE firmware.

Before changing partition layouts, back up settings and presets through Wi-Fi. Building and then running `.venv/bin/pio run -e YOUR_ENVIRONMENT -t upload` with the board connected uses the matching bootloader, partition table, and application over USB. If distributing a binary instead, use the factory image for the initial layout transition. Install the matching **factory image over USB** to update the bootloader, partition table, and application together; an application-only image cannot change the partition table. Factory images contain padding across NVS and can erase stored Wi-Fi credentials, the pairing code, and bonds even though the filesystem offset is preserved. Restore configuration/presets if necessary and re-pair after an NVS reset. Keep passwords separately: WLED's configuration backup does not include them.

Subsequent updates with the **same** layout can use the application binary at `0x10000` over USB, preserving data partitions when no full-chip erase is requested. The generated `firmware.factory.bin` is flashed at `0x0`; `firmware.bin` is the application only. Release filenames contain `_BLE_NOOTA`. Do not flash an 8 MB image onto a 4 MB board. The fork's ordinary `esp32dev` regression target also uses the 3 MiB USB-only layout, without the BLE usermod.

Classic ESP32 profiles rebuild the pinned Arduino 3.3.12 / IDF 5.5.5 SDK with a BLE-only controller and without C++ exceptions/RTTI, which WLED does not use. This leaves instruction RAM for all the WLED integrations. The first build takes longer because it compiles the SDK; later builds use its cache. Keep `pio-scripts/ble_sdk.py` with the sample: it makes SDK cache selection account for board settings and resolves the release-name override before SDK compilation. Do not build different SDK configurations concurrently against the same PlatformIO package directory.

## Settings and security

| Setting | Meaning |
| --- | --- |
| `enabled` | Starts/stops advertising and disconnects when disabled; can be re-enabled without rebooting |
| `device-name` | Optional advertised name, bounded to 29 UTF-8 bytes; blank uses a unique default |
| `max-request-bytes` | 256–4096; default 4096 |
| `pairing-code` | Persistent six-digit code; changing it revokes firmware bonds |

The bridge requires encryption, authenticated pairing, bonding, and LE Secure Connections. `MYNEWT_VAL_BLE_SM_SC_ONLY=1` is mandatory in the supplied profiles; the build fails without it. Numeric comparison is never silently accepted. Pairing remains discoverable while enabled, but a new client must know the device code. WLED's existing local-network trust model still applies to its Wi-Fi settings page.

A configured WLED settings PIN is independent of Bluetooth pairing. Authenticate using `POST /ble/auth` with `{"pin":"1234"}` before accessing configuration, settings forms, or privileged files. Authorization belongs only to the current encrypted connection, expires on disconnect or PIN change, and never borrows another HTTP client's global unlock. `{"lock":true}` revokes it. Incorrect attempts are throttled across reconnects. The older request-local `pin` field remains supported for `POST /json/cfg`. Lighting state and preset controls require the Bluetooth bond but do not require the settings PIN.

## GATT contract (protocol 1)

| Role | UUID | Properties |
| --- | --- | --- |
| Service | `7c2e0001-5d2b-4fd0-b1c2-0cc8f5470101` | Advertised service |
| RX | `7c2e0002-5d2b-4fd0-b1c2-0cc8f5470101` | Authenticated, encrypted write with response |
| TX | `7c2e0003-5d2b-4fd0-b1c2-0cc8f5470101` | Protected read (`ready`), indicate |
| LIVE | `7c2e0004-5d2b-4fd0-b1c2-0cc8f5470101` | Protected read (`live`), indicate |

Client sequence: connect → discover service/characteristics → read TX to complete pairing → install TX and LIVE receivers/subscriptions → send an initial `GET /json/info` or `GET /json` → send one request at a time. A service UUID alone is not sufficient validation. The iOS app validates properties, the protected probe, and WLED's JSON identity before saving a device.

Bonded platforms can restore notification subscriptions before application callbacks are ready. Firmware therefore holds all LIVE indications until it dispatches this connection's first complete authenticated request; its TX response finishes before LIVE begins. Subscription alone does not request an initial snapshot. Install the LIVE receiver before the first request if live updates may be used on this connection. Keep assembling complete LIVE frames even while the application is not consuming them, so enabling delivery later cannot begin halfway through a frame. A client that never uses LIVE may leave that channel unused.

Every request and response frame begins with a **two-byte little-endian byte count**, excluding those two bytes. The first ATT packet contains both length bytes; subsequent packets contain payload bytes only. Limit every write/indication to `min(negotiated ATT MTU - 3, 244)` bytes. For Core Bluetooth, cap using both maximum write lengths: the with-response value alone can advertise long writes larger than one ATT packet.

Request body (UTF-8):

```text
POST /json/state

{"on":true,"bri":128}
```

TX response body:

```text
200 application/json

{"success":true}
```

LIVE frames contain raw `{"state":...,"info":...}` JSON, without a status header. TX and LIVE need separate assemblers. They share one firmware indication scheduler, which waits for confirmation of **every** packet, including the final one. The next command can assemble while a live update finishes.

Requests are bounded by the configured maximum (at most 4096 bytes); response/live frames by the 65535-byte length field and available JSON/heap capacity. Status 400 means malformed input; 401 settings PIN required; 404 unsupported path; 405 unsupported method; 503 unavailable JSON buffer or oversized serialization. Invalid framing, overlapping ready requests, queue overflow, indication failure, or a 10-second partial-request/indication stall disconnects the link. Never retry on that same stream: protocol 1 has no request identifiers.

An unauthenticated connection is closed after 120 seconds so an abandoned pairing does not hold the device indefinitely. The iOS connection/pairing deadline is 90 seconds; command idle deadline is 30 seconds, renewed by write acknowledgements and response chunks. Live updates are coalesced at 150 ms and commands take priority. `/json/info` exposes `ble.protocol`, `ble.maxRequest`, and `ble.security` for client capability detection.

API version 2 adds complete local-control adapters while retaining protocol-1 frames and UUIDs. Reads include `/json`, `/json/si`, `/json/state`, `/json/info`, `/json/effects`, `/json/fxdata`, `/json/palettes` (or `/json/pal`), `/json/palx?page=N`, `/json/pins`, `/json/cfg`, `/json/net`, `/json/nodes`, and `/json/live`. `/json` includes effect and palette catalogs; `/json/si` and LIVE indications contain state/info only. Effect metadata matches HTTP's strings after `@`.

State/config writes accept `/json`, `/json/si`, `/json/state`, and `/json/cfg`. Settings scripts are available at `/settings/s.js?p=N`; URL-encoded POST forms use `/settings/{wifi,leds,ui,sync,time,sec,dmx,um,2D}`. Both transports call the same settings mutation routine, including ordered duplicate usermod fields. Unsupported compiled-out pages return an error. `/reset` reboots after confirming its response. Legacy `/win&...` commands are accepted. The iOS app supplies bundled HTML/CSS/JavaScript offline and routes requests through these adapters; firmware does not need to transmit its entire web bundle.

### API version 2 transfer contract

`GET /ble/capabilities` returns `version:2`, `protocol:1`, `maxFrameSize`, `maxChunk`, `maxPathBytes`, `maxRequestSize`, `maxFileSize`, `features`, `authorized`, and `pinRequired`. Obey these negotiated limits, including when the user has lowered the request limit below 4096. `GET /ble/auth` returns the authorization state. No secrets belong in URLs.

All following operations are JSON POST requests. Transfer IDs are opaque eight-character hexadecimal strings, scoped to the current connection. Only one upload or staged request is active at a time; serialize each complete begin/write/commit sequence. Reads can interleave between files.

| Endpoint/operation | Request fields | Response |
| --- | --- | --- |
| `/ble/fs`, `list` | `cursor` (default 0), `limit` (1–16) | `files:[{name,size}]`, `next` cursor or null |
| `/ble/fs`, `read` | `path`, `offset`, `length` up to `maxChunk`, `revision` for nonzero offsets | `path,size,offset,next,eof,revision,data` (base64) |
| `/ble/fs`, `begin` | `path,size,sha256` | `id,maxChunk,next:0` |
| `/ble/request`, `begin` | `method:"POST",path,contentType,size,sha256` | `id,maxChunk,next:0` |
| Either endpoint, `write` | `id,offset,data` (canonical base64) | `id,next` |
| Either endpoint, `commit` | `id` | File: `success,saved,size,sha256,reboot`; request: actual route's response |
| Either endpoint, `abort` | `id` | `success:true` |
| `/ble/fs`, `delete` | `path` | `success:true` |

`sha256` and read `revision` are lowercase SHA-256 hex strings. Writes must have sequential offsets and exact total length; malformed base64, bad checksums, and invalid paths fail before commit. A temporary file is atomically renamed only after verification. Disconnect, timeout (60 seconds without transfer activity), and reboot remove incomplete uploads. A file replacement requires room for both the old and new files; low-space failure preserves the original.

Read clients **must hash the complete reconstructed file and compare it with the returned revision**. Firmware hashes ordinary files at the start and end; changes produce 409 when detected. The aggregate client hash additionally catches interleaved edits. Restart changed reads from offset zero. Configuration backup reads serialize the current public configuration (passwords remain excluded) and reject a changed revision between chunks. Secret files (`wsec` in the name), noncanonical paths, and bridge staging files are excluded. Preset/palette reads have the same public visibility as their HTTP equivalents; other file operations honor the settings PIN.

Staged requests accept only the supported JSON state/config and settings-form POST routes, up to 32768 bytes. JSON still respects WLED's configured JSON-memory capacity. Reserved `cfg.json` restores require a configuration object with revision, identity and LED hardware fields; preset files require an object root. All JSON uploads must parse. Custom palettes reload after commit/delete, and preset changes invalidate the UI cache.

`POST /ble/presets` with `{"op":"rename","id":1,"name":"Evening"}` edits the stored preset/playlist name without applying it. Preset IDs are 1–250 and names are at most 32 UTF-8 bytes. Successful mutations return a persistence receipt. Settings, configuration, and preset-save replies wait for core persistence and return `saved:true` only after a checked write. `GET /ble/status` exposes `config` and `presets` objects (`pending,generation,success`) plus `rebootPending`. Generations distinguish a completed write from an older successful operation. Reboot and Bluetooth reconfiguration wait until the last response indication is confirmed; failed saves suppress a requested reboot. A receipt includes `reboot`, `reconnect`, and `bluetoothEnabled` when a configuration change affects the link; finish the UI operation using that receipt before trying another command. A disconnected pending save still checks its result, but never delivers its old receipt to a replacement connection.

`POST /ble/ddp` with `{"data":"<base64>"}` accepts the same 0x02-prefixed RGB DDP binary packet as WLED's WebSocket endpoint, up to 1428 decoded bytes and the negotiated request-frame limit. Send sequentially with back-pressure; Bluetooth pixel throughput is lower than Wi-Fi. `/json/live` samples at most 256 pixels as six-character RGB hex strings with `n` sampling step and optional matrix `w,h`. Poll only while the preview is visible. Real-time network integrations continue to need their underlying network even though their configuration interface works over Bluetooth.

## Reference client and regression tests

See [Phase 1 hardware tests](README_PHASE1.md) for unattended regression, soak,
configuration, and reboot suites with independent state verification and reports.

For the API 2 parity smoke test with Wi-Fi off, run `parity_hil.py --address DEVICE` after pairing. Add `--ask-pin` when a settings PIN is configured. This checks catalogs, live pixels, settings scripts, a large staged state request, and a uniquely named temporary file's chunked write/read/checksum/abort/delete paths. It also verifies that rejected replacements preserve the original bytes. Reports contain outcomes, never PINs or configuration contents. Disconnect the iPhone app first: the reference tool and phone cannot share the radio connection. Run the iPhone hardware suite separately. The smoke test does not replace settings/reboot/power-loss and low-heap testing on real hardware.

For reversible persistence checks on an exclusively controlled fixture, use `parity_persistence_hil.py --address DEVICE --expected-mac USB_VERIFIED_MAC --output NEW_PRIVATE_DIRECTORY`. It backs up configuration, runtime state and the original preset bytes before mutation; tests scene save/rename/delete and playlist file storage without starting playback; saves a temporary description through the complete UI form; and checks disconnect cleanup. Add `--allow-reboot` for durable settings/restoration checks with measured uptime-reset evidence and `--exercise-pin` to test a temporary settings PIN only when the initial PIN is blank. A known existing PIN can be supplied through the environment variable named by `--settings-pin-env`; an unknown PIN stops the run without replacing it. Recovery credentials are journaled privately before mutation, cleanup is separately bounded and shielded from cancellation, and failed cases stay failed even after successful restoration. Final restoration checks include the original runtime UDP send/receive groups. Keep the backup directory until final restoration is verified. The tool uses only BLE and does not replace the independent USB backup or radio/power-loss acceptance tests.

```sh
.venv/bin/pip install -r usermods/ble_api_bridge/requirements-client.txt
.venv/bin/python usermods/ble_api_bridge/client_example.py --scan
.venv/bin/python usermods/ble_api_bridge/client_example.py --address DEVICE get /json/info
.venv/bin/python usermods/ble_api_bridge/client_example.py --address DEVICE post /json/state '{"on":true,"bri":128}'
.venv/bin/python -m unittest discover -s usermods/ble_api_bridge/tests -p 'test_*.py' -v
clang++ -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined usermods/ble_api_bridge/tests/framing_test.cpp -o /tmp/wled-ble-framing-test
/tmp/wled-ble-framing-test
```

On macOS, `DEVICE` is the peripheral UUID printed by scanning; pairing is automatic on the protected read. Linux/Windows may use `--pair`. The client uses Bleak 3.0.2 and serialized writes with response.

**Initial macOS commissioning is a manual setup step.** Stock Bleak 3.0.2 times out a protected characteristic read after 20 seconds; increasing the overall connection timeout does not extend that inner limit. If entering the system pairing code needs more time, run:

```sh
.venv/bin/python usermods/ble_api_bridge/client_example.py --commission --address DEVICE get /json/info
```

This explicit commissioning mode selects an instance-local CoreBluetooth backend adapter through Bleak's `backend` argument. It extends only the protected TX probe to at most 90 seconds, within a 110-second total connection deadline. The firmware independently closes unauthenticated links after 120 seconds. Complete the macOS prompt using the device's code; the adapter does not enter codes, suppress prompts, or weaken authentication. It uses one private delegate method and refuses to run with a Bleak version other than the pinned 3.0.2 until that integration is revalidated. Normal API and unattended runs retain the stock backend after commissioning. A timeout disconnects and requires an explicit retry.

The reusable API supports `subscribe_live()`, `next_live(timeout=...)`, `unsubscribe_live()`, `get_json()`, `post_json()`, and `refresh_capabilities()`. `connect_timeout` is an upper bound for connection/discovery/probe/subscription, not a promise that platform operations wait that long. Use `BleApiBridgeClient(..., commission=True, connect_timeout=110)` only for initial macOS setup; unattended suites should fail and report pairing loss rather than wait for human input.

The reference client always installs the physical LIVE receiver during connection. `connect(live=True)` and `subscribe_live()` perform a read-only capabilities request after subscriptions are ready, which also starts the firmware's live stream. `unsubscribe_live()` stops application delivery and clears queued updates while the receiver continues assembling and discarding complete frames. It does not change the bonded characteristic subscription or interrupt an in-flight frame.

Re-enabling delivery on an already-started stream waits for a future LIVE update; it does not synthesize a notification or force a fresh initial snapshot. Call `get_json("/json")` when an immediate authoritative snapshot is needed. Waiters from an unsubscribed delivery session fail even if another caller immediately subscribes again.

For reviewed changes, dependency exceptions, build results, and the physical-device acceptance matrix, see [Bluetooth review](../../docs/BLUETOOTH_REVIEW.md). Compile and simulated transport tests do not certify radio behavior or the system pairing dialog on real hardware.
