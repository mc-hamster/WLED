# iOS ↔ WLED Bluetooth review

Reviewed 2026-09-25 on the `ble` branches of `mc-hamster/WLED` and `mc-hamster/WLED-iOS`. This work is maintained as a fork; no pull request, push, device flash, or deployment was performed.

API 2 parity follow-up: 2026-09-26. The firmware now exposes the complete local settings and file interfaces to the iOS offline workspace, alongside native scene/effect/palette controls. The earlier Phase 1 scope and measurements below are historical; the API 2 verification section records this follow-up. Physical-device validation remains pending.

## Baseline and conclusion

Firmware upstream: [`wled/WLED` at `58dfd8ce52e40298cd6991e3d46f328117c7e873`](https://github.com/wled/WLED/commit/58dfd8ce52e40298cd6991e3d46f328117c7e873). App upstream: [`Moustachauve/WLED-iOS` at `16eee4cab5699e398b56c322a45e8354cda8df1b`](https://github.com/Moustachauve/WLED-iOS/commit/16eee4cab5699e398b56c322a45e8354cda8df1b). Both source tips were fetched and merged into the existing BLE branches, retaining their Bluetooth work and upstream app lifecycle/network improvements.

The app and firmware now implement the same service, security handshake, byte framing, write limits, JSON routes, and serialized request lifecycle. Previously the firmware used unavailable IDF 4 NimBLE entry points under the current Arduino 3 stack, and the app had continuation, timer, and readiness races. Those problems are fixed and covered by builds and software tests where practical.

**Physical interoperability is still unverified.** No flashable ESP32 board and available iPhone pair were connected. The locked Mac also prevented manual Simulator screen inspection. Passing builds and simulated callbacks establish software correctness within the tested cases; they do not prove radio coexistence, system pairing UX, or sustained on-device memory stability. Use the acceptance matrix below before distributing firmware.

## Findings and changes

| Priority | Original problem and impact | Resolution / evidence |
| --- | --- | --- |
| P0 | Raw `esp_nimble_hci_and_controller_init` and host APIs did not match the selected Arduino 3 / IDF 5 toolchain; effect-data serialization also referenced a missing function. | Explicit NimBLE-Arduino 2.5.1 dependency and public 2.x API; `/json/fxdata` uses WLED's actual mode metadata. Firmware build matrix verifies linkage. |
| P1 | Every device used `123456`; the app displayed security options that could not configure iOS pairing; numeric comparison was automatically accepted. | Persistent random per-installation passkey, protected pairing probe, authenticated encrypted characteristics, bonding and mandatory SC-only compile setting. Code changes revoke bonds. iOS owns the prompt; obsolete app secrets are cleared. |
| P1 | NimBLE callbacks and WLED's loop shared mutable strings, assemblies, and response state without synchronization. Runtime settings could resize buffers during writes. | Fixed-size event queue transfers host events to the loop. Only the loop owns request/response memory and invokes WLED. Atomic status fields and a bounded mutex protect configuration snapshots; request buffer stays fixed at 4096 bytes. |
| P1 | Requests arriving during live updates were discarded; response offsets advanced without confirmed delivery. Errors could be mistaken for the next request's reply. | One indication scheduler, confirmation of every packet including the last, separate pending request assembly, and commands prioritized over live updates. Ambiguous framing or transport failure disconnects before retry. |
| P1 | NimBLE 2.5.1's new `onStatus` overload passes a zero-initialized connection-info object. Assuming it identifies the peer can discard indication acknowledgements. | Adapter captures the accepted peer's real handle in ordered connect/disconnect callbacks. Exactly one peer is accepted even when a chip SDK reserves multiple slots. See [NimBLEServer.cpp](https://github.com/h2zero/NimBLE-Arduino/blob/2.5.1/src/NimBLEServer.cpp), `BLE_GAP_EVENT_NOTIFY_TX`. |
| P1 | Core Bluetooth's with-response write limit could produce a long write larger than the firmware's ATT chunk. Framing was not strict about truncation and overflow. | Write budget is the smaller Core Bluetooth limit, capped at 244; strict byte-count assemblers reject invalid frames. UTF-8 bodies remain bytes until full reassembly. Cross-language UUID checks and packet-boundary tests pass. |
| P1 | Cancelled timeout tasks continued, simultaneous connects/requests could replace continuations, and a response could complete before the last write acknowledgement. | Main-actor session claims a request slot before suspension, owns all continuations, checks cancellation in timers, and waits for both complete response and final write acknowledgement. Tests cover cancellation, concurrent readiness, response/write ordering, timeouts, and recovery. |
| P1 | Disconnects could leave the app showing connected; stale asynchronous work could revive a destroyed client; writes could overlap. | Immediate disconnect propagation, generation-guarded tasks, exponential reconnect, independent state-change coalescing, and one serialized command stream. Pairing/permission errors wait for explicit user action. |
| P1 | BLE metadata was added directly to the already-shipped Core Data v2 model, endangering upstream store migration. | Restored upstream v2 and made the BLE schema v3; migration flags are set before loading. An actual v2 SQLite store migrates with names, MACs, and Wi-Fi addresses preserved. Former BLE v2's schema remains represented by v3. |
| P1 | An unrelated HTTP client's global `correctPIN` could authorize a BLE configuration write. | `/json/cfg` writes independently validate the PIN in that request. Protected reads require the PIN-protected Wi-Fi settings path. No global unlock is borrowed or changed. |
| P1 | The move to the newer SDK exceeded the old OTA slots; the earlier workaround removed WLED features and filesystem capacity. | That workaround is retired. At the fork owner's explicit direction, all three supported BLE profiles use a single factory app partition and disable OTA. Every other base integration and the original data-partition capacity are preserved. C3 is excluded. Classic ESP32 rebuilds the current SDK for a BLE-only controller to retain DMX and AudioReactive within instruction RAM. |
| P2 | BLE device UUIDs were stored as hostnames; mDNS address changes could unnecessarily restart BLE connections. | UUIDs are excluded from Wi-Fi addresses; BLE connection signatures depend on the peripheral identity. Existing Wi-Fi addresses survive BLE registration. |
| P2 | Discovery lost scan intent while the radio initialized; early construction could prompt for permissions unexpectedly; pairing screens showed ineffective passkey controls. | Lazy central creation, remembered scan intent, explicit permission/radio states, clearer nearby names/signal labels, cancellable onboarding, and system-pairing guidance. Manual visual verification remains pending. |
| P2 | Bluetooth detail view did not expose useful native LED controls. | Native power, brightness, and main-segment RGB control preserving the white channel; authoritative state refresh after each write and optional live updates. Offline controls are disabled and reconnect/errors are visible. |
| P2 | Selecting another peripheral could silently associate it with the wrong saved light; stock OTA updates could remove BLE support. | Verify the selected light's MAC before replacing its identifier; cancel verification when leaving. Capability-aware stock update suppression protects BLE-enabled firmware. |
| P2 | Disabled-at-boot firmware could not enable Bluetooth later; advertising retry timestamps failed after long uptime; oversized names could break advertising. | Runtime start/stop/reconfigure, wrap-safe deadlines, bounded UTF-8 names with unique default suffix, and service UUID in advertising/name in scan response. Abandoned unauthenticated connections expire after 120 seconds. Configuration serialization preserves a newly requested code even if saving precedes its deferred application. |

## Dependency audit

Versions were checked against package registries and upstream release/branch metadata. Exact locks/pins retain reproducibility; “latest” here means current stable versions compatible with this fork's active BLE build, not floating dependency heads on every build.

| Dependency | Selected version / revision | Treatment |
| --- | --- | --- |
| [PlatformIO Core](https://pypi.org/project/platformio/) | 6.2.0 | Updated requirements and complete Python lock |
| [pioarduino ESP32 platform](https://github.com/pioarduino/platform-espressif32/releases/tag/55.03.312-1) | 55.03.312-1 | Arduino 3.3.12 / IDF 5.5.5, replacing the older Tasmota baseline |
| [NimBLE-Arduino](https://github.com/h2zero/NimBLE-Arduino/releases/tag/2.5.1) | 2.5.1 | Explicit bridge dependency; API and acknowledgement callback source reviewed |
| [AsyncTCP](https://github.com/ESP32Async/AsyncTCP/releases) | 3.5.0 | Updated |
| [AnimatedGIF](https://github.com/bitbank2/AnimatedGIF/releases) | 2.2.0 | Updated; enabled in every supported BLE profile |
| [ESPAsyncWebServer WLED fork](https://github.com/Aircoookie/ESPAsyncWebServer/commit/dbb7c33898902de66bb165060767f14ae4fb1cca) | 2.4.2, `dbb7c338…` | Updated to this fork's tip; retains WLED-specific APIs |
| [NeoPixelBus CORE3](https://github.com/Makuna/NeoPixelBus/tree/CORE3) | 2.9.0 branch, `76afe832…` | Already at the compatible branch tip; kept exact pin |
| [esp_dmx IDF 5 fork](https://github.com/netmindz/esp_dmx/tree/esp-idf-v5-fixes) | 4.1.0, `ed12a290…` | Already at the required fork tip; enabled in every supported BLE profile |
| [GifDecoder fork](https://github.com/Aircoookie/GifDecoder) | 1.1.0, `bc3af189…` | Already at fork tip |
| IRremoteESP8266 / AsyncMqttClient | 2.9.0 / 0.9.0 | Latest releases already selected |
| [Swift Collections](https://github.com/apple/swift-collections/releases) | 1.7.0 | Updated minimum and resolved pin |
| [SwiftLintPlugins](https://github.com/SimplyDanny/SwiftLintPlugins/releases) | 0.65.1 | Updated minimum and resolved pin |
| [MarkdownUI](https://github.com/gonzalezreal/swift-markdown-ui/releases) | 2.4.1 | Current release; raised minimum to match |
| NetworkImage / swift-cmark | 6.0.1 / 0.9.0 | Re-resolved transitive graph to current versions |
| clean-css / html-minifier-terser | 5.3.3 / 7.2.0 | Current stable, exact npm pins |
| nodemon / web-resource-inliner | 3.1.14 / 8.0.0 | Updated, npm lock regenerated; npm audit reported zero vulnerabilities |
| [Bleak](https://pypi.org/project/bleak/) | 3.0.2 | Reference client updated and tested against current API |

Compatibility exceptions are intentional and visible:

- PlatformIO 6.2.0 requires `chardet<6` on Apple Silicon macOS. The lock uses 5.2.0 rather than incompatible 7.6.0. All other packages in the Python lock matched the current PyPI versions during this audit.
- WLED's vendored ArduinoJson 6.18.1, Time/Timezone, Espalexa, and FastLED subset are customized source, not ordinary resolved packages. They remain as supplied by the fetched WLED tip. Replacing them with generic latest releases (including ArduinoJson 7) is a separate API/memory migration and is **not claimed complete** here.
- Generic ESP32Async WebServer releases do not replace WLED's specialized fork automatically. Its latest commit changes low-memory admission behavior; runtime heap-stress verification is still required.
- Optional usermods outside these profiles, ESP8266 compatibility toolchains, and older IDF 4 board profiles retain upstream pins. They were not mass-upgraded or certified by this BLE review.

## Verification performed

- WLED web build: `npm ci`, `npm run build`, and all 16 Node tests passed; no generated headers were committed.
- Firmware frame assembly: 24,576 native round trips covering every request size 1–4096 at packet budgets 3/20/65/180/182/244; invalid lengths, overflow canaries, overlapping requests, and `millis()` rollover. Clang AddressSanitizer and UndefinedBehaviorSanitizer passed.
- Python reference client: 8 tests passed for strict framing, UTF-8, concurrent request serialization, ATT budgeting, response-before-write completion, cancellation, timeout/reconnect, malformed packets, required pairing probe, and firmware UUID agreement.
- iOS: 44 tests passed (52 executions including parameterized ATT and OTA-capability cases) on the iOS 27 Simulator using Xcode 27. Tests cover the above session/client races, live/response separation, database migration and legacy secret cleanup, plus upstream regression tests. The deployment target remains iOS 16; older physical OS versions were not exercised.
- Unsigned generic iPhone build passed. This checks compilation/linkage for devices; it does not install or exercise Core Bluetooth radio traffic.
- Service/RX/TX/LIVE UUIDs were compared across Swift, C++, and Python and match exactly, ignoring UUID letter case.
- Profile regression tests require the inherited WLED feature flags, libraries, and usermods, with OTA as the sole allowed feature removal. They check exact factory-app/NVS/filesystem/coredump layouts, reject the earlier feature cuts and reduced storage, prevent ArduinoOTA from re-enabling updates, and exclude the C3 profile.
- Firmware build results for the USB-only profiles are recorded below. Static RAM is not a runtime heap measurement.

| Environment | Application binary / partition bytes | Spare app bytes | Static DRAM bytes |
| --- | --- | --- | --- |
| `esp32dev_ble_api_bridge` | 1,906,640 / 3,145,728 | 1,239,088 | 101,468 |
| `esp32dev_8MB_ble_api_bridge` | 1,878,816 / 4,194,304 | 2,315,488 | 101,060 |
| `esp32s3dev_ble_api_bridge` | 1,991,072 / 4,194,304 | 2,203,232 | 66,939 |
| `esp32dev` | 1,834,416 / 3,145,728 | 1,311,312 | 93,598 |
| `esp32dev_debug` | 1,857,632 / 3,145,728 | 1,288,096 | 93,678 |

All five builds passed. Sizes use the actual application binary, including its headers and padding. The generated binary partition tables were parsed and verified, image checksums/digests passed, and linked symbols confirmed DMX input, GIF, AudioReactive, IR, Alexa, Hue sync, 2D effects and ESP-NOW; the three BLE targets also contain the bridge. Firmware-upload and ArduinoOTA entry points are absent. Classic BLE instruction RAM usage is 104,599 bytes (4 MB) and 102,207 bytes (8 MB), within the 131,072-byte region. Runtime free heap and radio behavior remain hardware-test items.

## API 2 verification

The shared settings form adapter preserves HTTP behavior and allows the same routines to accept Bluetooth forms, including repeated usermod fields. New protected routes provide session PIN authorization, configuration backup, atomic file transfers with SHA-256 checks, large staged JSON/forms, presets and palettes, live pixels, and the existing DDP decoder. An explicit USB serial `B` query supplies the installation's pairing code without Wi-Fi. Protocol-1 frame lengths and UUIDs remain unchanged.

Settings and preset receipts now wait for actual persistence. Checked temporary writes and atomic replacement preserve originals after failed file operations. A failed save suppresses reboot; completed disruptive settings wait for the last response acknowledgement. A disconnect detaches the receipt while retaining the operation's success/failure tracking, preventing old responses from reaching a new connection. Reserved configuration restores reject array or incomplete configuration roots, and file paths exclude secrets and staging files.

- The complete firmware host suite passed 140 tests, including compiled production persistence scheduling and storage-ingress tests under AddressSanitizer/UndefinedBehaviorSanitizer. It covers stale write generations, failed saves, final-indication reboot ordering, disconnected saves, new-session receipt exclusion, path/range overflow, canonical base64, compiled transfer stack budgets and fault-injected hardware-harness recovery.
- The Node suite passed 17 tests, including failure injection for staged core preset writes. `npm ci` and `npm run build` passed before compilation.
- `parity_hil.py` checks catalogs/settings scripts and large staged requests, and uses a unique temporary file to test byte-perfect transfer, bad-checksum preservation, malformed/out-of-order chunks, abort, and cleanup over BLE. These seven checks passed on the corrected S3 image described below.
- `parity_persistence_hil.py` prepares private backups and verifies an independently supplied MAC before mutation. It exercises saved scene/playlist files, exact restoration, complete UI forms, optional uptime-proven reboots and a journaled temporary PIN only on an initially blank-PIN fixture. Twelve mock-only failure tests cover unknown PINs, wrong devices, lost replies, corrupt files, repeated cancellation, recovery credentials, UDP runtime restoration and reset evidence. All five hardware cases passed on the corrected S3 image, as detailed below.

Final API 2 image sizes and checksums are recorded in the local `build_output/ble-parity/manifest.json` after the sequential profile builds. These are test images for the physical acceptance stage, not hardware-certified releases.

The first ESP32-S3 hardware run on 2026-09-26 exposed a deterministic crash on a 1024-byte file read. Compiler disassembly showed nested transfer/read/hash frames consuming 5,728 bytes before filesystem/crypto calls on the 8 KiB loop-task stack. Checked RAII heap/PSRAM scratch buffers reduced those same frames to 1,392 bytes without increasing the global task stack. A compiled-image stack-budget regression fails the earlier image and passes the correction. After flashing and byte-verifying the corrected application, the real BLE-only smoke passed all seven checks, including multi-chunk reads, checksum rejection, abort, large staged state requests and temporary-file cleanup. The local evidence is `build_output/ble-parity-hardware/20260926-usb-preflight/mac-parity-stack-fixed.json`.

The same S3 application passed all five reversible persistence cases with no skips: independent identity/private backups; saved scene, rename, playlist-file storage and deletion with exact preset-file restoration; a complete UI description form saved across reboot; malformed/disconnected transfer cleanup; and temporary settings-PIN protection, retry cooldown, lock/reconnect/reboot isolation and restoration to the original blank PIN. Three reboots were verified using reset millisecond uptime, not merely connection loss. Cleanup verified the complete original public configuration, exact original preset bytes, security checkboxes and runtime lighting/UDP state; no recovery remained necessary. Evidence is `build_output/ble-parity-hardware/20260926-persistence/results.json`; private backups and the recovery journal remain owner-only in that run directory. This is BLE readback coverage on one S3 fixture, not a power-cut, full-filesystem, physical-pixel or other-board certification. iPhone acceptance is recorded separately.

The bounded DDP test also passed all three checks: a BLE-uploaded 30-pixel RGB sequence appeared in the live preview with the expected global-brightness scaling, an independent USB observer confirmed realtime mode, and a truncated packet was rejected. The original state was restored and independently verified on the first paced-USB attempt. Earlier test expectations incorrectly assumed unscaled RGB values; the firmware correctly produced `800000`, `008000`, `000080` at brightness 128. An unrelated unpaced serial test write exceeded the 256-byte receive buffer; the USB oracle now writes 64-byte chunks spaced by 10 ms. These were test-harness corrections and required no firmware change. Evidence is `build_output/ble-parity-hardware/20260926-usb-preflight/ddp-usb-final-result.json`.

## Why the firmware grew and why these builds use USB

A controlled rebuild used the current WLED/BLE source and updated application libraries with the previous upstream Tasmota platform (`2026.05.50`, Arduino 3.3.8 / IDF 5.5.4). It retained AudioReactive, GIF/2D, DMX input, IR, Alexa, Hue sync and ESP-NOW:

| Controlled build | Actual application binary bytes |
| --- | ---: |
| Full WLED, previous upstream platform, no BLE | 1,338,384 |
| Full WLED plus BLE, same previous platform | 1,515,488 |
| Increment attributable to BLE in that comparison | 177,104 |

The full BLE control build fits the original 1,572,864-byte OTA slot with 57,376 bytes left. The switch to the newer Arduino 3.3.12 / IDF 5.5.5 platform and its larger default SDK configuration was a major cause of the size regression; it was incorrect to present the earlier feature cuts as unavoidable Bluetooth overhead. This experiment does not isolate every individual SDK/library contribution.

The owner chose to retain the current SDK and remove OTA. The 4 MB profiles now have a 3,145,728-byte factory application partition and the original 983,040-byte filesystem. The 8 MB profiles have a 4,194,304-byte factory application partition and the original 4,063,232-byte filesystem plus 65,536-byte coredump. NVS remains at `0x9000`, size `0x5000`. Only the application/OTA layout changes. The ordinary `esp32dev` regression target also uses USB updates.

The stock current SDK also exceeded classic ESP32 instruction RAM with all integrations and BLE enabled. The classic BLE profiles rebuild that same SDK with a BLE-only controller and C++ exceptions/RTTI disabled; WLED does not use either C++ feature. SDK components are retained. All USB profiles explicitly name the factory partition for PlatformIO size checks; the platform otherwise looks only for an OTA slot and can report the entire flash capacity as the application limit. The helper script prevents cache reuse across incompatible board configurations and resolves duplicate release-name definitions during SDK compilation. S3 uses the stock SDK. No WLED integrations are removed for RAM savings.

The firmware clears the standard OTA capability bit, hides the web update section, and rejects `/update`; ArduinoOTA is disabled. The iOS app honors that bit, hides its release channel/update controls, explains USB updates, and rechecks eligibility before download/upload, including stale update screens. A matching BLE capability still blocks generic stock firmware updates independently of the OTA bit.

**Install the new layout over USB after backing up configuration/presets.** Factory-image padding can overwrite NVS, requiring Wi-Fi credentials and Bluetooth pairing to be set up again. Keeping the filesystem offset does not guarantee preservation under an erase-all flash operation. Subsequent matching application-only images can be written at `0x10000` over USB without changing the data partitions. See the [installation instructions](../usermods/ble_api_bridge/README.md#build-profiles-and-installation).

## Physical-device acceptance matrix (pending)

Record chip, board, flash/PSRAM size, firmware SHA, app SHA, iOS version, free/minimum/largest heap, and negotiated MTU for each run. Use the matching [build profile and setup instructions](../usermods/ble_api_bridge/README.md).

| Scenario | Acceptance condition |
| --- | --- |
| Fresh phone / fresh firmware | Device is discoverable by name; one system pairing flow accepts its unique code; app saves identity only after valid JSON; power/brightness/color work. |
| Wrong code / cancel / app dismissal | Helpful error or clean cancellation; no repeated automatic pairing prompts; an explicit retry succeeds. |
| Permission denied / Bluetooth off | Clear recovery guidance; permission/radio restoration and rescan/reconnect work; no endless loading. |
| Repeat launch / phone reboot / firmware reboot | Existing bond reconnects without entering the code again and displays authoritative state. |
| Bond forgotten on either side / pairing code changed / full NVS erase | Recoverable stale-bond guidance; no silent weakening of authentication; new code pairs after reset. |
| Two similar names / two phones / reference client connected | Correct identity remains associated with each saved light; duplicate names are distinguishable; only one active client is accepted. |
| MTU 23, 185, 247; large GET `/json` and `/json/fxdata` | Byte-perfect complete messages; one confirmation per indication; no truncation, timer collisions, or mixed live/response frames. |
| Rapid color/brightness changes with simultaneous Wi-Fi edits | Latest independent controls arrive, live changes appear, no stale responses or disconnected-but-online state. |
| Out of range / power loss during write or response | Pending operations terminate; restored connection starts with clean framing and correct status. |
| Brief app switch / long background / lock/unlock | Upstream grace period behaves; longer background releases link; foreground reconnects. Background continuous control is not promised. |
| Runtime enable/disable / name change / long UTF-8 name | Advertising follows configuration; disable drops connection; re-enable needs no reboot. |
| Settings PIN + another Wi-Fi client's unlocked session | Wrong/missing BLE PIN still returns 401; correct PIN authorizes only this encrypted BLE session. Disconnect/PIN change revokes authorization; reconnect does not bypass retry cooldown. |
| Wi-Fi off, full device workspace | Every compiled settings page loads bundled assets and current device values; ordered forms, scenes/playlists, custom palettes/maps, file tools and live previews operate over BLE. |
| Files larger than one frame, small request limit | Sequential chunks read back byte-for-byte with matching SHA-256; malformed input, bad digest, interruption, or insufficient space preserves the original. No secret/staging files are exposed. |
| Save failure, rebooting settings, disabling Bluetooth | UI reports saved only after persistence receipt; failure does not reboot; success acknowledges before disconnect. Reconnect never receives another session's stale response. |
| Rapid preset saves and rename | Same-second HTTP and BLE reads show current names/content; rename never applies a preset or loses it on a full filesystem. |
| Wi-Fi load + BLE + active effects, sustained soak | No watchdog/reset/heap degradation; acceptable control latency and LED output. S3 test should include PSRAM, DMX/GIF and selected integrations. |
| USB partition transition, backup/restore, subsequent USB update | One factory app; original filesystem capacity; configuration/presets restored; credentials and pairing re-established if NVS was overwritten. Matching application-only USB updates preserve data. Wi-Fi `/update` rejects uploads and no wireless update is offered by the app/web UI. |
| Small screen / large Dynamic Type / VoiceOver / light and dark appearance | Pairing instructions, errors, action buttons, and native controls remain readable and operable. |

API 2 exposes local control over BLE through native lighting/scene controls and a bundled offline workspace for the complete existing web interface, configuration and file tools. DDP pixel uploads use acknowledged requests with back-pressure; BLE throughput is lower than Wi-Fi and requires hardware measurement. Live updates remain intended for foreground use. External network integrations still require their network to communicate, although their configuration is available over BLE. Firmware updates require USB in both transports for the supported fork profiles.

## Local environment notes

A malformed duplicate Git ref that blocked fetching was preserved at `.git/ref-backups/main-2`. A stale BLE dependency cache that stalled package replacement was preserved at `.pio/cache-backup/esp32dev_ble_api_bridge-20260924`. Validation used fresh generated build/dependency directories under `/tmp`; the checked-in sample remains portable. Reviewed application and factory binaries are copied under ignored `build_output/ble-reviewed/`, with SHA-256 hashes in its manifest. These are test builds, not hardware-certified releases.
