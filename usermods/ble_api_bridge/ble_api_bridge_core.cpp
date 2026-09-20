#include "ble_api_bridge.h"

#ifdef ARDUINO_ARCH_ESP32

#include <algorithm>
#include <cstring>
#include <new>

namespace ble_api_bridge {

BleApiBridgeUsermod::BleApiBridgeUsermod() {
  _serviceUuid.u.type = BLE_UUID_TYPE_128;
  memcpy(_serviceUuid.value, SERVICE_UUID_BYTES, sizeof(SERVICE_UUID_BYTES));
  _rxUuid.u.type = BLE_UUID_TYPE_128;
  memcpy(_rxUuid.value, RX_UUID_BYTES, sizeof(RX_UUID_BYTES));
  _txUuid.u.type = BLE_UUID_TYPE_128;
  memcpy(_txUuid.value, TX_UUID_BYTES, sizeof(TX_UUID_BYTES));
  _liveUuid.u.type = BLE_UUID_TYPE_128;
  memcpy(_liveUuid.value, LIVE_UUID_BYTES, sizeof(LIVE_UUID_BYTES));
  g_bleApiBridge = this;
}

const char* BleApiBridgeUsermod::transportErrorMessage(PendingTransportError error) {
  switch (error) {
    case PendingTransportError::BadFrame: return "invalid BLE request frame";
    case PendingTransportError::Busy:     return "bridge busy";
    case PendingTransportError::TooLarge: return "request exceeds max-request-bytes";
    case PendingTransportError::Overflow: return "request overflow";
    case PendingTransportError::Timeout:  return "request assembly timed out";
    case PendingTransportError::None:     break;
  }
  return "unknown error";
}

void BleApiBridgeUsermod::setLastError(const String& error) {
  _lastError = error;
  DEBUG_PRINT(F("[BleApiBridge] "));
  DEBUG_PRINTLN(error);
}

void BleApiBridgeUsermod::clearRequestAssembly() {
  _request.expected = 0;
  _request.received = 0;
  _request.active = false;
  _request.ready = false;
  _request.started = 0;
}

void BleApiBridgeUsermod::clearResponse() {
  _response.payload = String();
  _response.offset = 0;
  _response.valueHandle = 0;
  _response.active = false;
  _response.firstChunk = true;
  _response.nextSendAt = 0;
}

void BleApiBridgeUsermod::syncDeviceName() {
  if (_deviceName.length() > 0) return;
  _deviceName = serverDescription;
  _deviceName.trim();
  if (_deviceName.length() == 0) {
    _deviceName = F("WLED-BLE");
  }
}

bool BleApiBridgeUsermod::allocateRequestBuffer() {
  _requestBuffer.reset(new (std::nothrow) uint8_t[_maxRequestBytes]);
  if (!_requestBuffer) {
    setLastError(F("failed to allocate request buffer"));
    return false;
  }
  return true;
}

void BleApiBridgeUsermod::setup() {
  syncDeviceName();
  if (!allocateRequestBuffer()) {
    _enabled = false;
    return;
  }

  _setupDone = true;
  if (_enabled) startBle();
}

void BleApiBridgeUsermod::loop() {
  if (!_enabled) {
    stopBle();
    return;
  }

  if (_request.active && millis() - _request.started > _requestTimeoutMs) {
    clearRequestAssembly();
    _pendingTransportError = PendingTransportError::Timeout;
  }

  handlePendingTransportError();

  if (_request.ready && !_response.active) {
    processBleRequest();
  }

  if (_livePushPending
      && !_response.active
      && !_request.active
      && !_request.ready
      && millis() >= _livePushDueAt) {
    if (queueLiveStatePush()) {
      _livePushPending = false;
      _livePushDueAt = 0;
    }
  }

  sendNextResponseChunk();

  if (_restartBlePending && !_response.active) {
    _restartBlePending = false;
    refreshBleConfiguration();
  }

  if (_advertisePending && !_bleConnected) {
    startAdvertising();
  }
}

void BleApiBridgeUsermod::addToJsonInfo(JsonObject& root) {
  JsonObject user = root["u"];
  if (user.isNull()) user = root.createNestedObject("u");

  JsonArray bridge = user.createNestedArray(F("BLE API"));
  bridge.add(_bleInitialized && _enabled ? F("enabled") : F("disabled"));
  bridge.add(F(" bridge"));
}

void BleApiBridgeUsermod::addToJsonState(JsonObject& root) {
  JsonObject state = root[UM_NAME];
  if (state.isNull()) state = root.createNestedObject(UM_NAME);

  state["enabled"] = _enabled;
  state["ble"] = _bleInitialized && _enabled;
  state["connected"] = _bleConnected;
  state["live"] = _liveUpdatesEnabled;
}

void BleApiBridgeUsermod::readFromJsonState(JsonObject&) {}

void BleApiBridgeUsermod::addToConfig(JsonObject& root) {
  JsonObject top = root.createNestedObject(UM_NAME);
  top["enabled"] = _enabled;
  top["device-name"] = _deviceName;
  top["max-request-bytes"] = _maxRequestBytes;
}

void BleApiBridgeUsermod::appendConfigData() {
  oappend(F("addInfo('BleApiBridge:device-name',1,'BLE passkey fixed at 123456');"));
  oappend(F("addInfo('BleApiBridge:max-request-bytes',1,'256-4096');"));
}

bool BleApiBridgeUsermod::readFromConfig(JsonObject& root) {
  JsonObject top = root[UM_NAME];
  if (top.isNull()) {
    return false;
  }

  bool oldEnabled = _enabled;
  String oldName = _deviceName;
  uint16_t oldMaxRequestBytes = _maxRequestBytes;

  _enabled = top["enabled"] | _enabled;
  _deviceName = top["device-name"] | _deviceName;
  _maxRequestBytes = top["max-request-bytes"] | _maxRequestBytes;
  _maxRequestBytes = constrain(_maxRequestBytes, 256, MAX_REQUEST_BYTES_LIMIT);

  bool configChanged = oldEnabled != _enabled
    || oldName != _deviceName
    || oldMaxRequestBytes != _maxRequestBytes;

  if (!_requestBuffer || oldMaxRequestBytes != _maxRequestBytes) {
    allocateRequestBuffer();
    clearRequestAssembly();
  }

  if (_setupDone && configChanged) {
    if (_bleInitialized) {
      _restartBlePending = true;
    }
  }

  return true;
}

void BleApiBridgeUsermod::onStateChange(uint8_t) {
  if (!_enabled || !_bleConnected || !_liveUpdatesEnabled) return;
  _livePushPending = true;
  _livePushDueAt = millis() + LIVE_PUSH_DEBOUNCE_MS;
}

uint16_t BleApiBridgeUsermod::getId() {
  return USERMOD_ID_BLE_API_BRIDGE;
}

} // namespace ble_api_bridge

#endif
