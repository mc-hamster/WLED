# BLE API Bridge

`ble_api_bridge` is an ESP32-only WLED usermod that exposes a small BLE GATT transport for existing WLED JSON APIs.

The usermod stays isolated inside `usermods/ble_api_bridge`:

- no new WLED control API is invented
- BLE requests are translated into existing `/json` handlers
- when the usermod is enabled, BLE is always available
- Wi-Fi remains standard WLED Wi-Fi behavior

## What It Implements

- ESP-NimBLE transport using the ESP32 core's built-in NimBLE host
- encrypted BLE characteristics
- fixed BLE security policy:
  - bonding required
  - MITM required
  - LE Secure Connections required
  - fixed passkey `123456`
  - pairing always open
- `GET /json`
- `GET /json/state`
- `GET /json/info`
- `GET /json/si`
- `GET /json/effects`
- `GET /json/fxdata`
- `GET /json/pins`
- `GET /json/cfg`
- `POST /json`
- `POST /json/state`
- `POST /json/cfg`

## Behavior

If the usermod is enabled:

- BLE starts during setup
- BLE remains available while the usermod stays enabled
- Wi-Fi is not suppressed or reconfigured by this usermod

If the usermod is disabled:

- the BLE bridge is not started

There is no BLE-only or dual-mode runtime toggle.

## Build

Enable the usermod the usual WLED way.

Example `platformio_override.ini`:

```ini
[env:esp32dev_ble_api_bridge]
extends = env:esp32dev
custom_usermods = ble_api_bridge
```

This usermod is intended for `ESP32` targets only.

## GATT Layout

Service UUID:

- `7c2e0001-5d2b-4fd0-b1c2-0cc8f5470101`

Characteristics:

- RX write characteristic: `7c2e0002-5d2b-4fd0-b1c2-0cc8f5470101`
- TX indicate characteristic: `7c2e0003-5d2b-4fd0-b1c2-0cc8f5470101`

Expected client behavior:

1. Pair and bond with passkey `123456`.
2. Connect.
3. Enable indications on the TX characteristic.
4. Write the request to the RX characteristic in chunks.
5. Reassemble the response from TX indications.

## Wire Format

Request payload format:

```text
METHOD PATH

BODY
```

Examples:

```text
GET /json/state
```

```text
POST /json/state

{"on":true,"bri":128}
```

Chunking rules:

- the first RX write starts with a 2-byte little-endian total request length
- the remaining bytes in that first write are request bytes
- subsequent writes contain raw request bytes only

Response payload format:

```text
STATUS CONTENT-TYPE

BODY
```

Example:

```text
200 application/json

{"success":true}
```

Response chunking rules:

- the first TX indication starts with a 2-byte little-endian total response length
- the remaining bytes in that first indication are response bytes
- subsequent indications contain raw response bytes only

## Security Model

This usermod does not expose BLE security settings.

It always uses:

- encrypted link
- bonded peers
- MITM protection
- LE Secure Connections
- static passkey `123456`
- always-open pairing

## How Requests Map Into WLED

This usermod does not reimplement WLED state logic.

It dispatches into existing WLED internals:

- JSON reads use the same serializers as HTTP
- JSON writes use `deserializeState()` and `deserializeConfig()`

That keeps the BLE surface aligned with WLED's existing control behavior.

## Client Usage

Typical flow:

1. Connect to the device over BLE.
2. Subscribe to TX indications.
3. Send `GET /json/info` to inspect the device.
4. Send `POST /json/state` to control LEDs.
5. Use the normal WLED JSON API over BLE.

Examples:

Get current state:

```text
GET /json/state
```

Turn on and set brightness:

```text
POST /json/state

{"on":true,"bri":128}
```

Update config:

```text
POST /json/cfg

{"hw":{"led":{"total":60}}}
```

## Config Keys

Stored under `cfg.json -> um -> BleApiBridge`:

- `enabled`
- `device-name`
- `max-request-bytes`
