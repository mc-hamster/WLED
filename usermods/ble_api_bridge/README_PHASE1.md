# Phase 1: unattended Python / ESP32 BLE tests

Phase 1 exercises the real BLE bridge with the reusable `BleApiBridgeClient` in
[`client_example.py`](client_example.py). Phase 2 uses the real iPhone. The
[protocol contract](README.md#gatt-contract-protocol-1) is the common reference;
Python-to-firmware agreement alone does not establish correctness on iOS.

`hil_runner.py` performs bounded tests, compares BLE state against an independent
HTTP or USB serial path, attempts limited recovery, restores the fields it
changed, and writes results even when setup fails. A recovered failure stays a
failure. There are no interactive input prompts in the runner.

## Commission the fixture once

Use a dedicated WLED ESP32 test board. Disconnect other BLE clients; the bridge
accepts only one. Install the BLE firmware matching the chip, flash and PSRAM.
The maintained profiles disable OTA; firmware installation and updates use USB.
Keep an external backup before repurposing a board or changing its partitions.

The Mac must have Bluetooth access. Scan for the peripheral and complete its
system pairing prompt before starting unattended runs. macOS owns the bond and
may request user input if it is missing or stale. The runner does not enter a
passkey, clear system bonds, dismiss system dialogs or retry pairing indefinitely.
A pairing failure or deadline expiration produces a setup failure report. If
commissioning needs a person, stop and finish that setup before restarting the
suite. The firmware's generated pairing code must not be put into logs.

Use Python 3.11 or newer. From the WLED repository root, use an ignored local environment:

```sh
python3 -m venv build_output/ble-hil/venv
build_output/ble-hil/venv/bin/pip install -r usermods/ble_api_bridge/requirements-hil.txt
build_output/ble-hil/venv/bin/python usermods/ble_api_bridge/client_example.py --scan
build_output/ble-hil/venv/bin/python usermods/ble_api_bridge/client_example.py --address DEVICE get /json/info
```

On macOS, `DEVICE` is the peripheral UUID printed by the scanner, not the Wi-Fi
MAC. Use the returned `info.mac` to identify the physical WLED board. The runner
checks that the independent oracle reports the same MAC before performing any
test state changes. It rechecks that identity during independent observations.

HTTP is preferred when the board is provisioned on the local network. USB serial
is an alternative for a board without Wi-Fi. Specify exactly one oracle:

```sh
build_output/ble-hil/venv/bin/python usermods/ble_api_bridge/hil_runner.py \
  --address DEVICE --http-url http://192.168.1.50 \
  --suite smoke --output build_output/ble-hil/run-001-smoke

build_output/ble-hil/venv/bin/python usermods/ble_api_bridge/hil_runner.py \
  --address DEVICE --serial-port /dev/cu.usbmodem1101 \
  --suite smoke --output build_output/ble-hil/run-002-serial
```

Use a separate output directory for each run. A nonempty directory is rejected
before hardware access, and an exclusive `.run-reserved` marker prevents two
runners from claiming the same empty directory. Stop serial monitors and other
tools that own the USB port before using `--serial-port`. The serial oracle keeps
one 115200-baud connection open for the run and uses WLED's `{"v":true}` JSON
interface. DTR/RTS are deasserted before opening, but opening native USB can still
reset the ESP32-S3; serial opens before BLE setup and baseline capture. Buffered
serial output is sampled during queries for panic, watchdog and brownout markers.
Those markers fail acceptance. This is not continuous serial logging and can
miss events if the OS buffer overflows. Raw serial output is never saved.

## Suites

For a complete unattended pass after commissioning, run the functional API,
configuration, reboot and soak suites sequentially. Use a fresh output root,
stop on any nonzero exit, and inspect that run's reports before retrying. The
configuration command below explicitly permits a temporary settings PIN on a
blank-PIN test fixture; its private recovery file and restoration checks are
described later in this document. An existing PIN must be supplied through
`WLED_HIL_SETTINGS_PIN` and is never replaced.

```sh
ble_hil_python=build_output/ble-hil/venv/bin/python
ble_hil_address=DEVICE
ble_hil_http=http://192.168.1.50
ble_hil_run=build_output/ble-hil/full-run-001

"$ble_hil_python" usermods/ble_api_bridge/api_hil.py \
  --address "$ble_hil_address" --http-url "$ble_hil_http" --output "$ble_hil_run/api" &&
"$ble_hil_python" usermods/ble_api_bridge/config_hil.py \
  --address "$ble_hil_address" --http-url "$ble_hil_http" --output "$ble_hil_run/config" \
  --allow-config-tests --provision-test-pin &&
"$ble_hil_python" usermods/ble_api_bridge/reboot_hil.py \
  --address "$ble_hil_address" --http-url "$ble_hil_http" --output "$ble_hil_run/reboot" \
  --allow-reboot-tests &&
"$ble_hil_python" usermods/ble_api_bridge/hil_runner.py \
  --address "$ble_hil_address" --http-url "$ble_hil_http" --output "$ble_hil_run/soak" \
  --suite soak --duration 1200 --seed 20260926
```

| Suite | Selected coverage |
| --- | --- |
| `smoke` | Identity/capabilities; supported reads and response shapes; power; brightness 0/1/128/255; RGB and preservation of the existing white channel; independent state readback; independent writes observed by BLE; concurrent TX/LIVE convergence; baseline restoration. |
| `regression` | Smoke plus semantic error statuses, path normalization, application chunk caps 3/20/65/182/244, encoded request boundaries up to the advertised maximum, concurrent API callers, repeated reconnects, partial requests discarded on disconnect, invalid frame disconnects, and the firmware's partial-frame stall deadline. |
| `soak` | Regression followed by a deterministic seeded workload of state writes, large reads, TX/LIVE activity, reconnects and independent state verification, with uptime/free-heap/latency sampling. |

```sh
build_output/ble-hil/venv/bin/python usermods/ble_api_bridge/hil_runner.py \
  --address DEVICE --http-url http://192.168.1.50 \
  --suite regression --seed 20260926 \
  --output build_output/ble-hil/run-003-regression

build_output/ble-hil/venv/bin/python usermods/ble_api_bridge/hil_runner.py \
  --address DEVICE --http-url http://192.168.1.50 \
  --suite soak --duration 28800 --seed 20260926 \
  --output build_output/ble-hil/run-004-soak
```

`--duration` is the soak workload duration in seconds, after regression. The
final operation finishes under its normal deadline, so the workload can extend
past that duration by one bounded operation and recovery. `--timeout` defaults
to 30 seconds per request; `--connect-timeout` to 90 seconds; `--case-timeout` to
180 seconds. Tiny-chunk maximum-size requests receive a finite larger request
deadline based on their packet count. The entire chunk matrix has a minimum
900-second case budget. Every failed ordinary case gets at most two reconnect
attempts. Exhausted recovery skips remaining selected cases, records that gap,
and still attempts baseline restoration and teardown.

Keep the laptop awake for the run. On macOS, prefix a test command with
`caffeinate -i` to prevent idle system sleep until that command exits.

The chunk cap is an application write limit, **not a forced ATT MTU**. Reports
record both the requested cap and the actual safe write budget exposed by the
client. Encoded request lengths exclude the two-byte framing prefix. Unicode
payloads and whitespace padding exercise exact byte counts without saving
presets or changing settings. Firmware-reported `maxRequest` governs the upper
boundary; a reduced maximum is respected.

LIVE tests allow intermediate updates to be coalesced. They require valid
independent LIVE/TX assembly and eventual convergence to the final state,
recording how many LIVE frames were consumed. They do not require one frame per
write or claim that coalescing occurs at an exact measured radio interval.

Malformed-frame cases use an exclusive raw BLE phase. A passive TX observer
prevents the client's unsolicited-response protection from creating a false
firmware-disconnect pass. Each case then establishes a fresh connection and
checks identity and uptime. Semantic errors use ordinary requests and must leave
the same connection usable.

## Results and restoration

The output directory contains:

- `results.json`: case results, bounded recovery attempts, last action metadata
  on failure, source hashes/revision captured once at runner initialization,
  oracle type, health samples and descriptive heap summaries. `lifecycle` and
  `outcome` stay `running` until cleanup finishes, including failed-setup cleanup;
  only then is a final passing, failing or incomplete outcome published.
- `junit.xml`: CI-compatible failures and explicit skips, with lifecycle/outcome
  properties. Collect it only after the run finishes.
- `report.md`: concise human-readable results and coverage limitations.
- `cases.jsonl`: each completed case appended immediately. Aggregate soak reports
  refresh at least every 30 seconds between completed operations and at shutdown.
- `baseline-state.json`: power, brightness, segment colors and any available
  segment freeze flags. This is not a complete device-configuration backup.

Configuration responses, credentials, pairing codes and raw request/response
bodies are not written into the normal runner's artifacts. The separate
configuration suite described below keeps an owner-only recovery directory.
Unknown library exception messages are
suppressed because they can include payloads; the exception type, exact known
client connection-error constants and recent method/path/status/length/latency
metadata remain available. Reports record
source hashes, not firmware binary hashes; keep the installation log and flashed
binary hash alongside the run when comparing builds.

Exit codes are:

- `0`: all selected cases passed; exclusions listed in the report still apply.
- `1`: a test, setup, recovery or restoration failed.
- `2`: selected coverage was incomplete, such as running without an independent
  oracle. A BLE-only run therefore cannot be presented as a full pass.
- `130`: interrupted by the operator; cleanup is attempted and interruption is
  recorded when the event loop receives it. Process kill/power loss cannot run
  cleanup; use the baseline artifact to restore the touched fields afterward.

Restoration only replays the fields the suite changes, with a temporary zero
transition, then checks each field through BLE and the configured independent
oracle. A final health sample checks uptime after restoration. Replaying an
entire `GET /json/state` object can have unintended WLED
side effects. The runner does not save presets, mutate persistent configuration,
change pairing codes, remove bonds, configure a settings PIN or reflash firmware.

## Acceptance boundaries

A passing selected suite is useful hardware evidence, not a claim of complete
BLE certification. Serial verification does not establish Wi-Fi coexistence.
The current runner does not provide physical power cuts, forced RF attenuation,
MTU negotiation control, observed LED light output, or automated wrong-passkey
and bond-reset flows. Heap summaries report first/last/minimum/maximum observed
free heap, medians across four consecutive sample windows, and the last window's
range and trend. These help distinguish initialization drops from later behavior;
they do not assert that a short plateau proves the absence of leaks. Firmware
minimum-heap and largest-block metrics are included only when exposed as numeric
fields. There is no arbitrary pass/fail threshold for heap slope or latency. An
unexpected sampled uptime decrease is a failure.

Phase 2 must still validate the real iPhone's Core Bluetooth transport, permissions,
pairing, system bonds, background/foreground behavior, lock transitions and UI.
Shared request/response and framing fixtures can verify the Swift protocol logic;
macOS BLE success does not establish those iOS behaviors.

The harness itself has hardware-free regression tests:

```sh
build_output/ble-hil/venv/bin/python -m unittest discover \
  -s usermods/ble_api_bridge/tests -p 'test_*.py' -v
```

Those tests cover reporting, preservation of failures, bounded recovery, identity
guards, safe restoration, exact UTF-8 lengths and suppression of configuration
secrets. They do not substitute for running the actual hardware suites.

## Software reboot recovery

`reboot_hil.py` is a separate, explicit reboot suite. Run it with an established
bond and a verified HTTP path after the normal control tests work:

```sh
build_output/ble-hil/venv/bin/python usermods/ble_api_bridge/reboot_hil.py \
  --address DEVICE --http-url http://192.168.1.50 --allow-reboot-tests \
  --connect-timeout 30 --output build_output/ble-hil/run-005-reboot
```

It tests three scenarios:

1. Reboot an idle, already bonded BLE connection using HTTP `GET /reset`.
2. Reboot after a partial RX frame, then require a clean first request after
   reconnection. Incomplete request bytes must not survive the restart.
3. Start a large `/json/fxdata` response, wait for its first incomplete TX packet,
   and request reboot. The old request must fail from disconnection. If the
   response completes before reset, retry the injection once; two missed
   injections produce incomplete coverage and exit `2`, never a passing
   interruption test.

Both BLE and HTTP identities are checked before every reset. The old BLE link
must disconnect within `--disconnect-deadline` (default 10 seconds), and HTTP
must return the same MAC with evidence of a new boot within `--reboot-timeout`
(default 45 seconds). New boot evidence compares uptime and elapsed time, allowing
integer-second rounding. A successful HTTP reply or a BLE disconnect alone is
insufficient. Very young boots get a bounded warm-up before another reset.

After HTTP recovery, the suite opens a fresh BLE connection, checks its identity
and new uptime, exercises state changes through both paths, and records restart
timing. The ordinary unexpected-restart guard is reset only after this proof.
Actual failures remain failures even if bounded reconnect recovery succeeds.
The suite stops further reset injections after a failed case and attempts state
restoration, final health sampling and teardown. Reports include the verified
`expected_reboots` and byte counts for the response interruption, without response
payloads.

This establishes **software reboot recovery**. It does not test a power cut,
brownout, wedged MCU, USB reset fallback or re-pairing after bond removal. No USB
port is opened by this suite. If HTTP and BLE recovery fail, the report preserves
that failure; a separately proven USB recovery mechanism can recover the fixture.
As with the normal runner, restoration covers power, brightness and segment
colors. Other volatile state, such as an active playlist or effect, can return to
its configured boot state during reboot and is outside this suite's restoration
claim. Use the dedicated controlled test fixture.

## Functional API scenarios

`api_hil.py` tests application behavior through the real BLE JSON API. Every
control change is checked against both BLE state and independent HTTP state:

```sh
build_output/ble-hil/venv/bin/python usermods/ble_api_bridge/api_hil.py \
  --address DEVICE --http-url http://192.168.1.50 --connect-timeout 30 \
  --output build_output/ble-hil/run-006-functional-api
```

The eight named scenarios cover global power/brightness and verbose `POST /json`;
existing-segment power, brightness and selection; three color slots using arrays,
partial channel objects and hex strings; advertised Solid/Blink/Breathe effects;
speed/intensity boundaries; advertised fixed palettes; HTTP changes observed
through BLE reads and LIVE; and rejected requests preserving state. Reports
describe the actions and values, instead of presenting generic transaction counts.

The suite follows the actual firmware semantics: zero brightness turns power off
while preserving the last positive brightness; negative speed/intensity values
are ignored; partial color objects preserve unspecified channels; an empty color
array preserves that slot; a six-digit hex color clears white where RGBW exists.
It does not claim that all out-of-range integers are rejected. Effects are selected
by name only after effect-name/metadata array lengths match `info.fxcount`.
Palette indices come from HTTP's fixed-name array, because custom palette IDs
are not a contiguous extension of `info.palcount`.

All segment writes use existing explicit IDs and `fxdef:false`. Bounds, mapping,
grouping, physical pins and LED length are never changed. The suite saves every
active segment's touched fields, including freeze flags cleared by global off/on,
then restores them with global power/brightness in the same request. BLE and HTTP
must both match the baseline, and invariant segment geometry/options must remain
unchanged. `api-baseline-state.json` contains these restorable state fields.
Per-request `udpn.nn:true` suppresses synchronization broadcasts.

An active preset, playlist, nightlight or realtime input produces incomplete
coverage before any functional mutation. Preset/configuration files are never
saved. API-visible state restoration does not restore animation phase or timebase,
and successful state readback does not verify physical light output, audio-reactive
behavior or 2D rendering. The suite requires the reporting lifecycle update that
provides `HilRunner.finish_reports()`.

## Temporary configuration and settings PIN tests

`config_hil.py` separately tests runtime BLE names and UTF-8 truncation, request
limits, disable/re-enable, and per-request settings PIN authorization. It requires
`--allow-config-tests`, the existing bond, and a verified HTTP recovery path:

```sh
build_output/ble-hil/venv/bin/python usermods/ble_api_bridge/config_hil.py \
  --address DEVICE --http-url http://192.168.1.50 --allow-config-tests \
  --provision-test-pin --output build_output/ble-hil/run-006-configuration
```

`--provision-test-pin` explicitly permits a temporary four-digit settings PIN
when both BLE and HTTP confirm that the fixture has no PIN. An existing PIN is
never replaced; supply its credential through the `WLED_HIL_SETTINGS_PIN`
environment variable (or the variable named by `--settings-pin-env`). Without
provisioning or an existing credential, PIN coverage is reported as incomplete.

Before writing, the suite saves `private-config/configuration.json` and, when
provisioning, `private-config/pin-recovery.json`. The directory is mode `0700`,
files are mode `0600`, and an ignore file excludes their contents from Git.
These private files can contain credentials; public reports include only their
paths and restoration status. The PIN recovery file records the generated PIN
and original security checkbox values for recovery after a killed process.

Provisioning uses the normal HTTP security form, preserving OTA lock, Wi-Fi
lock, ArduinoOTA and same-subnet settings. It never submits an OTA password or
factory-reset field. The suite respects the PIN retry cooldown, locks and
unlocks HTTP settings to prove the PIN works using the protected
`/settings/s.js?p=6` route. HTTP `GET /json/cfg` remains public in the core and
does not prove authentication. The suite then verifies that missing, wrong
and numeric BLE PIN values still fail while HTTP is unlocked. The numeric case
uses the correct PIN digits as a JSON number, proving type rejection. Existing
PINs with nonnumeric characters or leading zeros cannot establish that exact
coercion check and report incomplete coverage. A correct string PIN must authorize
only the submitted BLE configuration request.

Cleanup restores the original BLE fields first, then removes this run's
temporary PIN and verifies the original security flags and configuration access
through both transports. Lost replies and cancellation still trigger cleanup;
HTTP writes already in progress finish before restoration sends an opposite
change. PIN removal independently rechecks the BLE baseline and retries its HTTP
restoration when needed. If that baseline cannot be recovered, it retains the
temporary PIN and private recovery credentials rather than saving temporary
configuration. Live readback does not prove flash persistence; reboot the fixture
and recheck its original configuration and blank PIN afterward.

Failures remain failures even when restoration succeeds. Hard process
termination or power loss cannot execute cleanup; retain the private recovery
file and external board backup until restoration is verified.

Configuration HTTP requests use socket inactivity timeouts and a body-read budget
checked between reads. A socket read may exceed the body budget by one timeout;
slowly trickling response headers do not have a hard overall deadline. Cleanup
waits for outstanding HTTP writes before issuing an opposing mutation.

The security form saves the entire current configuration, so provision the PIN
before other temporary configuration changes and remove it after their
restoration. Run this suite alone on the dedicated fixture. It does not change
the Bluetooth pairing code, remove bonds or certify iOS authorization behavior.

The ordinary control suites preserve each serialized segment's `frz` flag when
present, because global off/on unfreezes segments. State mutations suppress UDP
sync broadcasts with per-request `udpn.nn:true`. Smoke/regression/soak/reboot skip active
preset/playlist, nightlight or realtime fixtures before mutation because their
runtime progression cannot be restored exactly. Soak cases and JSON operation
counts use descriptive names (`brightness_readback`, `effects_read`,
`live_controls`, `reconnect_readback`, `bidirectional_control`).
