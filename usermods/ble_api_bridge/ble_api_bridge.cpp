#include "ble_api_bridge.h"

#ifdef ARDUINO_ARCH_ESP32

namespace ble_api_bridge {

BleApiBridgeUsermod* g_bleApiBridge = nullptr;
static BleApiBridgeUsermod ble_api_bridge;
REGISTER_USERMOD(ble_api_bridge);

} // namespace ble_api_bridge

#else

class BleApiBridgeUsermod : public Usermod {
public:
  void setup() override {}
  void loop() override {}
};

static BleApiBridgeUsermod ble_api_bridge;
REGISTER_USERMOD(ble_api_bridge);

#endif
