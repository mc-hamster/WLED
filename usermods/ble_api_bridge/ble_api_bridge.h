#pragma once

#include "wled.h"

#ifdef ARDUINO_ARCH_ESP32

extern "C" {
#include "esp_nimble_hci.h"
#include "host/ble_att.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/ble_hs_id.h"
#include "host/ble_hs_mbuf.h"
#include "host/ble_sm.h"
#include "host/ble_store.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "store/config/ble_store_config.h"
}

#include <memory>
namespace ble_api_bridge {

static constexpr char UM_NAME[] = "BleApiBridge";
static constexpr uint16_t USERMOD_ID_BLE_API_BRIDGE = 0xB106;
static constexpr uint16_t DEFAULT_MAX_REQUEST_BYTES = 2048;
static constexpr uint16_t MAX_REQUEST_BYTES_LIMIT   = 4096;
static constexpr uint16_t DEFAULT_MTU               = 247;
static constexpr uint32_t DEFAULT_PASSKEY           = 123456;
static constexpr uint32_t DEFAULT_REQUEST_TIMEOUT   = 5000;
static constexpr uint32_t RESPONSE_CHUNK_INTERVAL   = 8;
static constexpr uint32_t LIVE_PUSH_DEBOUNCE_MS     = 150;
static constexpr uint8_t  MAX_RESPONSE_CHUNK        = 244;

static constexpr uint8_t SERVICE_UUID_BYTES[] = {
  0x01, 0x01, 0x47, 0xF5, 0xC8, 0x0C, 0xC2, 0xB1,
  0xD0, 0x4F, 0x2B, 0x5D, 0x01, 0x00, 0x2E, 0x7C,
};

static constexpr uint8_t RX_UUID_BYTES[] = {
  0x01, 0x01, 0x47, 0xF5, 0xC8, 0x0C, 0xC2, 0xB1,
  0xD0, 0x4F, 0x2B, 0x5D, 0x02, 0x00, 0x2E, 0x7C,
};

static constexpr uint8_t TX_UUID_BYTES[] = {
  0x01, 0x01, 0x47, 0xF5, 0xC8, 0x0C, 0xC2, 0xB1,
  0xD0, 0x4F, 0x2B, 0x5D, 0x03, 0x00, 0x2E, 0x7C,
};

static constexpr uint8_t LIVE_UUID_BYTES[] = {
  0x01, 0x01, 0x47, 0xF5, 0xC8, 0x0C, 0xC2, 0xB1,
  0xD0, 0x4F, 0x2B, 0x5D, 0x04, 0x00, 0x2E, 0x7C,
};

enum class PendingTransportError : uint8_t {
  None = 0,
  BadFrame,
  Busy,
  TooLarge,
  Overflow,
  Timeout,
};

enum class CharacteristicRole : uint8_t {
  Rx = 1,
  Tx = 2,
  Live = 3,
};

class BleApiBridgeUsermod : public Usermod {
public:
  BleApiBridgeUsermod();

  void setup() override;
  void loop() override;
  void addToJsonInfo(JsonObject& root) override;
  void addToJsonState(JsonObject& root) override;
  void readFromJsonState(JsonObject& root) override;
  void addToConfig(JsonObject& root) override;
  void appendConfigData() override;
  bool readFromConfig(JsonObject& root) override;
  void onStateChange(uint8_t mode) override;
  uint16_t getId() override;

  void onBleReset(int reason);
  void onBleSync();
  void handleGattRegister(struct ble_gatt_register_ctxt* ctxt);
  int handleGattAccess(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt* ctxt, void* arg);
  int handleGapEvent(struct ble_gap_event* event);

  static void bleHostTask(void*);
  static void bleResetCallback(int reason);
  static void bleSyncCallback();
  static void bleGattRegisterCallback(struct ble_gatt_register_ctxt* ctxt, void* arg);
  static int bleGattAccess(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt* ctxt, void* arg);
  static int bleGapEvent(struct ble_gap_event* event, void*);

private:
  struct RequestAssembly {
    size_t expected = 0;
    size_t received = 0;
    bool active = false;
    bool ready = false;
    unsigned long started = 0;
  } _request;

  struct ResponseState {
    String payload;
    size_t offset = 0;
    uint16_t valueHandle = 0;
    bool active = false;
    bool firstChunk = true;
    unsigned long nextSendAt = 0;
  } _response;

  bool _enabled = true;
  bool _setupDone = false;
  bool _bleInitialized = false;
  bool _bleConnected = false;
  bool _advertising = false;
  bool _advertisePending = false;
  bool _hostSynced = false;
  bool _indicationsEnabled = false;
  bool _liveUpdatesEnabled = false;
  bool _livePushPending = false;
  bool _restartBlePending = false;
  bool _processingBleRequest = false;

  uint16_t _maxRequestBytes = DEFAULT_MAX_REQUEST_BYTES;
  uint16_t _mtu = DEFAULT_MTU;
  uint16_t _activeConnHandle = BLE_HS_CONN_HANDLE_NONE;
  uint8_t _ownAddrType = BLE_OWN_ADDR_PUBLIC;
  uint16_t _rxValueHandle = 0;
  uint16_t _txValueHandle = 0;
  uint16_t _liveValueHandle = 0;

  uint32_t _requestTimeoutMs = DEFAULT_REQUEST_TIMEOUT;
  unsigned long _livePushDueAt = 0;

  PendingTransportError _pendingTransportError = PendingTransportError::None;

  String _deviceName;
  String _lastError;

  std::unique_ptr<uint8_t[]> _requestBuffer;

  ble_uuid128_t _serviceUuid = {};
  ble_uuid128_t _rxUuid = {};
  ble_uuid128_t _txUuid = {};
  ble_uuid128_t _liveUuid = {};

  CharacteristicRole _rxRole = CharacteristicRole::Rx;
  CharacteristicRole _txRole = CharacteristicRole::Tx;
  CharacteristicRole _liveRole = CharacteristicRole::Live;
  ble_gatt_chr_def _characteristics[4] = {};
  ble_gatt_svc_def _services[2] = {};

  static const char* transportErrorMessage(PendingTransportError error);

  void setLastError(const String& error);
  void clearRequestAssembly();
  void clearResponse();

  void syncDeviceName();
  bool allocateRequestBuffer();

  void prepareGattDefinitions();
  void configureSecurity();
  bool initBleStack();
  bool startAdvertising();
  bool startBle();
  void stopBle();
  void refreshBleConfiguration();

  String buildSuccessEnvelope() const;
  String buildErrorEnvelope(uint16_t status, const String& message) const;
  bool queueResponse(uint16_t status, const char* contentType, const String& body);
  bool queueErrorResponse(uint16_t status, const String& message);
  bool queueLiveStatePush();
  bool buildJsonBody(const String& path, String& body);
  String normalizePath(const String& rawPath) const;
  bool dispatchGet(const String& rawPath);
  bool dispatchPostJsonState(const String& bodyText);
  bool dispatchPostJsonConfig(const String& bodyText);
  bool dispatchPost(const String& rawPath, const String& bodyText);
  bool processRequestText(const String& requestText);
  void processBleRequest();

  void sendNextResponseChunk();
  void handlePendingTransportError();
  int processWriteChunk(const uint8_t* data, size_t len);
  int handleRxAccess(struct ble_gatt_access_ctxt* ctxt);
  int handleTxAccess(struct ble_gatt_access_ctxt* ctxt);
  int handleLiveAccess(struct ble_gatt_access_ctxt* ctxt);
};

extern BleApiBridgeUsermod* g_bleApiBridge;

} // namespace ble_api_bridge

#endif
