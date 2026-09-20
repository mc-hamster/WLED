#include "ble_api_bridge.h"

#ifdef ARDUINO_ARCH_ESP32

#include <algorithm>
#include <cstring>

namespace ble_api_bridge {

void BleApiBridgeUsermod::prepareGattDefinitions() {
  memset(_characteristics, 0, sizeof(_characteristics));
  memset(_services, 0, sizeof(_services));

  ble_gatt_chr_flags rxFlags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC | BLE_GATT_CHR_F_WRITE_AUTHEN;
  ble_gatt_chr_flags txFlags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_INDICATE | BLE_GATT_CHR_F_READ_AUTHEN;
  ble_gatt_chr_flags liveFlags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_INDICATE | BLE_GATT_CHR_F_READ_AUTHEN;

  _characteristics[0].uuid = &_rxUuid.u;
  _characteristics[0].access_cb = bleGattAccess;
  _characteristics[0].arg = &_rxRole;
  _characteristics[0].flags = rxFlags;
  _characteristics[0].val_handle = &_rxValueHandle;

  _characteristics[1].uuid = &_txUuid.u;
  _characteristics[1].access_cb = bleGattAccess;
  _characteristics[1].arg = &_txRole;
  _characteristics[1].flags = txFlags;
  _characteristics[1].val_handle = &_txValueHandle;

  _characteristics[2].uuid = &_liveUuid.u;
  _characteristics[2].access_cb = bleGattAccess;
  _characteristics[2].arg = &_liveRole;
  _characteristics[2].flags = liveFlags;
  _characteristics[2].val_handle = &_liveValueHandle;

  _services[0].type = BLE_GATT_SVC_TYPE_PRIMARY;
  _services[0].uuid = &_serviceUuid.u;
  _services[0].characteristics = _characteristics;
}

void BleApiBridgeUsermod::configureSecurity() {
  ble_hs_cfg.reset_cb = bleResetCallback;
  ble_hs_cfg.sync_cb = bleSyncCallback;
  ble_hs_cfg.gatts_register_cb = bleGattRegisterCallback;
  ble_hs_cfg.gatts_register_arg = this;
  ble_hs_cfg.store_read_cb = ble_store_config_read;
  ble_hs_cfg.store_write_cb = ble_store_config_write;
  ble_hs_cfg.store_delete_cb = ble_store_config_delete;
  ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
  ble_hs_cfg.store_status_arg = nullptr;

  ble_hs_cfg.sm_io_cap = BLE_HS_IO_DISPLAY_ONLY;
  ble_hs_cfg.sm_bonding = 1;
  ble_hs_cfg.sm_mitm = 1;
  ble_hs_cfg.sm_sc = 1;
  ble_hs_cfg.sm_keypress = 0;
  ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
  ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
}

bool BleApiBridgeUsermod::initBleStack() {
  if (_bleInitialized) return true;

  syncDeviceName();

  esp_err_t err = esp_nimble_hci_and_controller_init();
  if (err != ESP_OK) {
    setLastError(String(F("esp_nimble_hci_and_controller_init failed: ")) + err);
    return false;
  }

  nimble_port_init();
  configureSecurity();
  ble_svc_gap_init();
  ble_svc_gatt_init();
  ble_svc_gap_device_name_set(_deviceName.c_str());
  ble_att_set_preferred_mtu(DEFAULT_MTU);
  prepareGattDefinitions();

  int rc = ble_gatts_count_cfg(_services);
  if (rc != 0) {
    setLastError(String(F("ble_gatts_count_cfg failed: ")) + rc);
    nimble_port_deinit();
    esp_nimble_hci_and_controller_deinit();
    return false;
  }

  rc = ble_gatts_add_svcs(_services);
  if (rc != 0) {
    setLastError(String(F("ble_gatts_add_svcs failed: ")) + rc);
    nimble_port_deinit();
    esp_nimble_hci_and_controller_deinit();
    return false;
  }

  nimble_port_freertos_init(bleHostTask);

  _bleInitialized = true;
  _hostSynced = false;
  _advertising = false;
  _advertisePending = false;
  _indicationsEnabled = false;
  _liveUpdatesEnabled = false;
  _livePushPending = false;
  _bleConnected = false;
  _activeConnHandle = BLE_HS_CONN_HANDLE_NONE;
  _mtu = DEFAULT_MTU;
  DEBUG_PRINTLN(F("[BleApiBridge] BLE initialized"));
  return true;
}

bool BleApiBridgeUsermod::startAdvertising() {
  if (!_bleInitialized || !_hostSynced) {
    _advertisePending = true;
    return false;
  }

  if (_advertising) return true;

  struct ble_hs_adv_fields fields;
  memset(&fields, 0, sizeof(fields));
  fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
  fields.uuids128 = &_serviceUuid;
  fields.num_uuids128 = 1;
  fields.uuids128_is_complete = 1;

  int rc = ble_gap_adv_set_fields(&fields);
  if (rc != 0) {
    setLastError(String(F("ble_gap_adv_set_fields failed: ")) + rc);
    return false;
  }

  struct ble_hs_adv_fields rspFields;
  memset(&rspFields, 0, sizeof(rspFields));
  rspFields.name = reinterpret_cast<const uint8_t*>(_deviceName.c_str());
  rspFields.name_len = std::min<size_t>(_deviceName.length(), BLE_HS_ADV_MAX_FIELD_SZ);
  rspFields.name_is_complete = rspFields.name_len == _deviceName.length();

  rc = ble_gap_adv_rsp_set_fields(&rspFields);
  if (rc != 0) {
    setLastError(String(F("ble_gap_adv_rsp_set_fields failed: ")) + rc);
    return false;
  }

  struct ble_gap_adv_params advParams;
  memset(&advParams, 0, sizeof(advParams));
  advParams.conn_mode = BLE_GAP_CONN_MODE_UND;
  advParams.disc_mode = BLE_GAP_DISC_MODE_GEN;

  rc = ble_gap_adv_start(_ownAddrType, nullptr, BLE_HS_FOREVER, &advParams, bleGapEvent, this);
  if (rc != 0) {
    setLastError(String(F("ble_gap_adv_start failed: ")) + rc);
    return false;
  }

  _advertising = true;
  _advertisePending = false;
  DEBUG_PRINTLN(F("[BleApiBridge] advertising started"));
  return true;
}

bool BleApiBridgeUsermod::startBle() {
  if (!_enabled) return false;
  if (!initBleStack()) return false;
  if (_hostSynced) {
    ble_svc_gap_device_name_set(_deviceName.c_str());
    startAdvertising();
  } else {
    _advertisePending = true;
  }
  return true;
}

void BleApiBridgeUsermod::stopBle() {
  if (!_bleInitialized) return;

  if (_advertising && ble_gap_adv_active()) {
    ble_gap_adv_stop();
  }
  if (_activeConnHandle != BLE_HS_CONN_HANDLE_NONE) {
    ble_gap_terminate(_activeConnHandle, BLE_ERR_REM_USER_CONN_TERM);
  }

  _advertising = false;
  _advertisePending = false;
  _indicationsEnabled = false;
  _liveUpdatesEnabled = false;
  _livePushPending = false;
  _bleConnected = false;
  _activeConnHandle = BLE_HS_CONN_HANDLE_NONE;
  clearRequestAssembly();
  clearResponse();
  DEBUG_PRINTLN(F("[BleApiBridge] BLE bridge disabled"));
}

void BleApiBridgeUsermod::refreshBleConfiguration() {
  if (!_bleInitialized) return;

  configureSecurity();
  ble_svc_gap_device_name_set(_deviceName.c_str());

  if (_advertising && ble_gap_adv_active()) {
    ble_gap_adv_stop();
    _advertising = false;
  }
  if (_activeConnHandle != BLE_HS_CONN_HANDLE_NONE) {
    ble_gap_terminate(_activeConnHandle, BLE_ERR_REM_USER_CONN_TERM);
  }
  _advertisePending = true;
}

void BleApiBridgeUsermod::sendNextResponseChunk() {
  if (!_response.active || !_bleInitialized || !_bleConnected || _response.valueHandle == 0) return;
  if (_response.valueHandle == _txValueHandle && !_indicationsEnabled) return;
  if (_response.valueHandle == _liveValueHandle && !_liveUpdatesEnabled) return;
  if (millis() < _response.nextSendAt) return;

  size_t maxChunk = (_mtu > 3) ? (_mtu - 3) : 20;
  maxChunk = std::min<size_t>(maxChunk, MAX_RESPONSE_CHUNK);
  if (_response.firstChunk && maxChunk <= 2) {
    maxChunk = 20;
  }

  size_t remaining = _response.payload.length() - _response.offset;
  if (remaining == 0) {
    clearResponse();
    return;
  }

  uint8_t packet[MAX_RESPONSE_CHUNK + 2];
  size_t packetLen = 0;
  size_t dataOffset = _response.offset;
  bool firstChunk = _response.firstChunk;

  if (firstChunk) {
    uint16_t totalLen = _response.payload.length();
    packet[packetLen++] = totalLen & 0xFF;
    packet[packetLen++] = (totalLen >> 8) & 0xFF;
    size_t chunkLen = std::min(remaining, maxChunk - 2);
    memcpy(packet + packetLen, _response.payload.c_str() + dataOffset, chunkLen);
    packetLen += chunkLen;
  } else {
    size_t chunkLen = std::min(remaining, maxChunk);
    memcpy(packet, _response.payload.c_str() + dataOffset, chunkLen);
    packetLen = chunkLen;
  }

  struct os_mbuf* om = ble_hs_mbuf_from_flat(packet, packetLen);
  if (om == nullptr) {
    setLastError(F("failed to allocate indication buffer"));
    clearResponse();
    return;
  }

  int rc = ble_gattc_indicate_custom(_activeConnHandle, _response.valueHandle, om);
  if (rc == BLE_HS_EBUSY) {
    _response.nextSendAt = millis() + RESPONSE_CHUNK_INTERVAL;
    return;
  }
  if (rc != 0) {
    setLastError(String(F("failed to indicate response chunk: ")) + rc);
    clearResponse();
    return;
  }

  if (firstChunk) {
    _response.offset += packetLen - 2;
    _response.firstChunk = false;
  } else {
    _response.offset += packetLen;
  }
  _response.nextSendAt = millis() + RESPONSE_CHUNK_INTERVAL;
}

void BleApiBridgeUsermod::handlePendingTransportError() {
  if (_pendingTransportError == PendingTransportError::None || _response.active) return;
  PendingTransportError error = _pendingTransportError;
  _pendingTransportError = PendingTransportError::None;
  queueErrorResponse(error == PendingTransportError::Timeout ? 408 : 400, transportErrorMessage(error));
}

int BleApiBridgeUsermod::processWriteChunk(const uint8_t* data, size_t len) {
  if (_request.ready || _response.active || _processingBleRequest) {
    _pendingTransportError = PendingTransportError::Busy;
    return 0;
  }

  if (len == 0) {
    return 0;
  }

  if (!_request.active) {
    if (len < 2) {
      _pendingTransportError = PendingTransportError::BadFrame;
      return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    uint16_t expected = data[0] | (static_cast<uint16_t>(data[1]) << 8);
    if (expected == 0 || expected > _maxRequestBytes || !_requestBuffer) {
      _pendingTransportError = PendingTransportError::TooLarge;
      return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    _request.expected = expected;
    _request.received = 0;
    _request.active = true;
    _request.started = millis();

    size_t payloadLen = len - 2;
    if (payloadLen > expected) {
      clearRequestAssembly();
      _pendingTransportError = PendingTransportError::Overflow;
      return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    memcpy(_requestBuffer.get(), data + 2, payloadLen);
    _request.received = payloadLen;
  } else {
    size_t remaining = _request.expected - _request.received;
    if (len > remaining) {
      clearRequestAssembly();
      _pendingTransportError = PendingTransportError::Overflow;
      return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    memcpy(_requestBuffer.get() + _request.received, data, len);
    _request.received += len;
  }

  if (_request.received == _request.expected) {
    _request.ready = true;
    _request.active = false;
  }

  return 0;
}

int BleApiBridgeUsermod::handleRxAccess(struct ble_gatt_access_ctxt* ctxt) {
  if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
    return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
  }
  if (_activeConnHandle == BLE_HS_CONN_HANDLE_NONE) {
    return BLE_ATT_ERR_UNLIKELY;
  }

  const uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
  std::unique_ptr<uint8_t[]> buffer(new (std::nothrow) uint8_t[len]);
  if (!buffer) {
    return BLE_ATT_ERR_UNLIKELY;
  }
  if (os_mbuf_copydata(ctxt->om, 0, len, buffer.get()) != 0) {
    return BLE_ATT_ERR_UNLIKELY;
  }

  return processWriteChunk(buffer.get(), len);
}

int BleApiBridgeUsermod::handleTxAccess(struct ble_gatt_access_ctxt* ctxt) {
  if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
    return BLE_ATT_ERR_READ_NOT_PERMITTED;
  }

  static const char kReady[] = "ready";
  if (os_mbuf_append(ctxt->om, kReady, sizeof(kReady) - 1) != 0) {
    return BLE_ATT_ERR_UNLIKELY;
  }
  return 0;
}

int BleApiBridgeUsermod::handleLiveAccess(struct ble_gatt_access_ctxt* ctxt) {
  if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
    return BLE_ATT_ERR_READ_NOT_PERMITTED;
  }

  static const char kLive[] = "live";
  if (os_mbuf_append(ctxt->om, kLive, sizeof(kLive) - 1) != 0) {
    return BLE_ATT_ERR_UNLIKELY;
  }
  return 0;
}

void BleApiBridgeUsermod::onBleReset(int reason) {
  _hostSynced = false;
  _advertising = false;
  _advertisePending = _enabled;
  _bleConnected = false;
  _indicationsEnabled = false;
  _liveUpdatesEnabled = false;
  _livePushPending = false;
  _activeConnHandle = BLE_HS_CONN_HANDLE_NONE;
  clearRequestAssembly();
  clearResponse();
  setLastError(String(F("NimBLE reset: ")) + reason);
}

void BleApiBridgeUsermod::onBleSync() {
  int rc = ble_hs_id_infer_auto(0, &_ownAddrType);
  if (rc != 0) {
    setLastError(String(F("ble_hs_id_infer_auto failed: ")) + rc);
    return;
  }

  _hostSynced = true;
  _mtu = ble_att_preferred_mtu();
  ble_svc_gap_device_name_set(_deviceName.c_str());
  if (_enabled) {
    _advertisePending = true;
  }
}

void BleApiBridgeUsermod::handleGattRegister(struct ble_gatt_register_ctxt*) {
  // Value handles are populated through _rxValueHandle and _txValueHandle.
}

int BleApiBridgeUsermod::handleGattAccess(uint16_t conn_handle, uint16_t, struct ble_gatt_access_ctxt* ctxt, void* arg) {
  if (_activeConnHandle != BLE_HS_CONN_HANDLE_NONE && conn_handle != _activeConnHandle) {
    return BLE_ATT_ERR_UNLIKELY;
  }

  CharacteristicRole role = *static_cast<CharacteristicRole*>(arg);
  if (role == CharacteristicRole::Rx) return handleRxAccess(ctxt);
  if (role == CharacteristicRole::Tx) return handleTxAccess(ctxt);
  if (role == CharacteristicRole::Live) return handleLiveAccess(ctxt);
  return BLE_ATT_ERR_UNLIKELY;
}

int BleApiBridgeUsermod::handleGapEvent(struct ble_gap_event* event) {
  switch (event->type) {
    case BLE_GAP_EVENT_CONNECT: {
      if (event->connect.status != 0) {
        _bleConnected = false;
        _activeConnHandle = BLE_HS_CONN_HANDLE_NONE;
        _indicationsEnabled = false;
        if (_enabled) {
          _advertisePending = true;
        }
        return 0;
      }

      if (_activeConnHandle != BLE_HS_CONN_HANDLE_NONE && _activeConnHandle != event->connect.conn_handle) {
        ble_gap_terminate(event->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return 0;
      }

      struct ble_gap_conn_desc desc;
      memset(&desc, 0, sizeof(desc));
      ble_gap_conn_find(event->connect.conn_handle, &desc);

      _bleConnected = true;
      _advertising = false;
      _indicationsEnabled = false;
      _liveUpdatesEnabled = false;
      _livePushPending = false;
      _activeConnHandle = event->connect.conn_handle;
      _mtu = ble_att_mtu(event->connect.conn_handle);

      ble_gap_security_initiate(event->connect.conn_handle);
      return 0;
    }

    case BLE_GAP_EVENT_DISCONNECT:
      _bleConnected = false;
      _advertising = false;
      _indicationsEnabled = false;
      _liveUpdatesEnabled = false;
      _livePushPending = false;
      _activeConnHandle = BLE_HS_CONN_HANDLE_NONE;
      clearRequestAssembly();
      clearResponse();
      if (_enabled) {
        _advertisePending = true;
      }
      return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
      if (event->subscribe.attr_handle == _txValueHandle) {
        _indicationsEnabled = event->subscribe.cur_indicate != 0;
      } else if (event->subscribe.attr_handle == _liveValueHandle) {
        _liveUpdatesEnabled = event->subscribe.cur_indicate != 0;
        if (_liveUpdatesEnabled) {
          _livePushPending = true;
          _livePushDueAt = millis();
        } else {
          _livePushPending = false;
        }
      }
      return 0;

    case BLE_GAP_EVENT_MTU:
      if (event->mtu.conn_handle == _activeConnHandle) {
        _mtu = event->mtu.value;
      }
      return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
      _advertising = false;
      if (_enabled && !_bleConnected) {
        _advertisePending = true;
      }
      return 0;

    case BLE_GAP_EVENT_NOTIFY_TX:
      if (event->notify_tx.status != 0 && event->notify_tx.status != BLE_HS_EDONE) {
        setLastError(String(F("indication failed: ")) + event->notify_tx.status);
        clearResponse();
      }
      return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
      if (event->enc_change.status != 0) {
        setLastError(String(F("encryption change failed: ")) + event->enc_change.status);
      }
      return 0;

    case BLE_GAP_EVENT_PASSKEY_ACTION: {
      struct ble_sm_io io;
      memset(&io, 0, sizeof(io));
      io.action = event->passkey.params.action;

      switch (event->passkey.params.action) {
        case BLE_SM_IOACT_DISP:
        case BLE_SM_IOACT_INPUT:
          io.passkey = DEFAULT_PASSKEY;
          DEBUG_PRINT(F("[BleApiBridge] passkey "));
          DEBUG_PRINTLN(DEFAULT_PASSKEY);
          return ble_sm_inject_io(event->passkey.conn_handle, &io);
        case BLE_SM_IOACT_NUMCMP:
          io.numcmp_accept = 1;
          return ble_sm_inject_io(event->passkey.conn_handle, &io);
        default:
          return 0;
      }
    }

    case BLE_GAP_EVENT_REPEAT_PAIRING:
      return BLE_GAP_REPEAT_PAIRING_IGNORE;

    default:
      return 0;
  }
}

void BleApiBridgeUsermod::bleHostTask(void*) {
  nimble_port_run();
  nimble_port_freertos_deinit();
  vTaskDelete(nullptr);
}

void BleApiBridgeUsermod::bleResetCallback(int reason) {
  if (g_bleApiBridge != nullptr) {
    g_bleApiBridge->onBleReset(reason);
  }
}

void BleApiBridgeUsermod::bleSyncCallback() {
  if (g_bleApiBridge != nullptr) {
    g_bleApiBridge->onBleSync();
  }
}

void BleApiBridgeUsermod::bleGattRegisterCallback(struct ble_gatt_register_ctxt* ctxt, void* arg) {
  BleApiBridgeUsermod* self = static_cast<BleApiBridgeUsermod*>(arg);
  if (self != nullptr) {
    self->handleGattRegister(ctxt);
  }
}

int BleApiBridgeUsermod::bleGattAccess(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt* ctxt, void* arg) {
  if (g_bleApiBridge == nullptr) {
    return BLE_ATT_ERR_UNLIKELY;
  }
  return g_bleApiBridge->handleGattAccess(conn_handle, attr_handle, ctxt, arg);
}

int BleApiBridgeUsermod::bleGapEvent(struct ble_gap_event* event, void*) {
  if (g_bleApiBridge == nullptr) {
    return 0;
  }
  return g_bleApiBridge->handleGapEvent(event);
}

} // namespace ble_api_bridge

#endif
