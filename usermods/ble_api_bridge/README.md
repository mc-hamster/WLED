# BLE API Bridge — maintained fork

This usermod exposes WLED's JSON control API over authenticated Bluetooth LE. Use it with the `ble` branch of [mc-hamster/WLED-iOS](https://github.com/mc-hamster/WLED-iOS/tree/ble). Wi-Fi remains available. The firmware supports one connected Bluetooth client at a time.

The implementation targets Arduino-ESP32 3.3.12 / ESP-IDF 5.5.5 and NimBLE-Arduino 2.5.1. ESP8266, ESP32-S2, and ESP32-P4 do not have the required integrated BLE radio. The supported BLE families are classic ESP32 and ESP32-S3. ESP32-C3 is excluded from this fork's BLE support; other radio-capable chips need their own validation. Do not combine this bridge with another usermod that owns the NimBLE device/server.

## Pair an iPhone

1. Install a BLE-enabled build for the board's exact chip, flash size, and PSRAM arrangement.
2. Open WLED over Wi-Fi. In **Settings → Usermods → BleApiBridge**, leave `enabled` on and note `pairing-code`.
3. In the forked iOS app, add a device, choose **Bluetooth**, and select the nearby WLED device. A blank firmware `device-name` becomes `WLED-` followed by a unique MAC suffix.
4. Tap **Add**, accept the iOS system pairing prompt, and enter the device's six-digit code. The app reads a protected characteristic before starting its command timeout, allowing time to enter the code.
5. Use the native power, brightness, and main-segment color controls. The app subscribes to state updates and reconnects while active. A brief background transition is tolerated; longer background periods disconnect to release the device.

Pairing and bond storage belong to iOS. No code is entered into or saved by the app. Initial provisioning currently requires Wi-Fi access to read the code; the firmware has no physical display. This is not a Wi-Fi provisioning service.

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

A configured WLED settings PIN is independently checked on every `POST /json/cfg`; another HTTP client's global unlock does not authorize a BLE request. Include `"pin":"1234"` in that request's JSON when required. Protected configuration reads return 401 and should use WLED's PIN-protected Wi-Fi settings page. State control requires the BLE bond but does not require the settings PIN.

## GATT contract (protocol 1)

| Role | UUID | Properties |
| --- | --- | --- |
| Service | `7c2e0001-5d2b-4fd0-b1c2-0cc8f5470101` | Advertised service |
| RX | `7c2e0002-5d2b-4fd0-b1c2-0cc8f5470101` | Authenticated, encrypted write with response |
| TX | `7c2e0003-5d2b-4fd0-b1c2-0cc8f5470101` | Protected read (`ready`), indicate |
| LIVE | `7c2e0004-5d2b-4fd0-b1c2-0cc8f5470101` | Protected read (`live`), indicate |

Client sequence: connect → discover service/characteristics → read TX to complete pairing → subscribe to TX and optionally LIVE → send one request at a time. A service UUID alone is not sufficient validation. The iOS app validates properties, the protected probe, and WLED's JSON identity before saving a device.

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

Supported reads: `/json`, `/json/si`, `/json/state`, `/json/info`, `/json/effects`, `/json/fxdata`, `/json/pins`, `/json/cfg`. Supported writes: `/json`, `/json/state`, `/json/cfg`. These call WLED's existing serializers/deserializers; the bridge does not serve HTML, presets files, arbitrary HTTP endpoints, or firmware uploads.

## Reference client and regression tests

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

For reviewed changes, dependency exceptions, build results, and the physical-device acceptance matrix, see [Bluetooth review](../../docs/BLUETOOTH_REVIEW.md). Compile and simulated transport tests do not certify radio behavior or the system pairing dialog on real hardware.
