#include "ble_api_bridge.h"

#ifdef ARDUINO_ARCH_ESP32

namespace ble_api_bridge {

String BleApiBridgeUsermod::buildSuccessEnvelope() const {
  return F("{\"success\":true}");
}

String BleApiBridgeUsermod::buildErrorEnvelope(uint16_t status, const String& message) const {
  String payload;
  payload.reserve(message.length() + 48);
  payload += F("{\"success\":false,\"status\":");
  payload += status;
  payload += F(",\"error\":\"");
  for (size_t i = 0; i < message.length(); i++) {
    char c = message.charAt(i);
    if (c == '\\' || c == '"') payload += '\\';
    payload += c;
  }
  payload += F("\"}");
  return payload;
}

bool BleApiBridgeUsermod::queueResponse(uint16_t status, const char* contentType, const String& body) {
  String header;
  header.reserve(strlen(contentType) + 24);
  header += status;
  header += ' ';
  header += contentType;
  header += F("\n\n");

  if (header.length() + body.length() > UINT16_MAX) {
    setLastError(F("response exceeds BLE bridge size limit"));
    return false;
  }

  _response.payload = header + body;
  _response.offset = 0;
  _response.valueHandle = _txValueHandle;
  _response.active = true;
  _response.firstChunk = true;
  _response.nextSendAt = 0;
  return true;
}

bool BleApiBridgeUsermod::queueErrorResponse(uint16_t status, const String& message) {
  return queueResponse(status, "application/json", buildErrorEnvelope(status, message));
}

bool BleApiBridgeUsermod::queueLiveStatePush() {
  if (!_liveUpdatesEnabled || _liveValueHandle == 0) return false;

  String body;
  if (!buildJsonBody(F("/json"), body)) {
    return false;
  }

  if (body.length() > UINT16_MAX) {
    setLastError(F("live push exceeds BLE bridge size limit"));
    return false;
  }

  _response.payload = body;
  _response.offset = 0;
  _response.valueHandle = _liveValueHandle;
  _response.active = true;
  _response.firstChunk = true;
  _response.nextSendAt = 0;
  return true;
}

bool BleApiBridgeUsermod::buildJsonBody(const String& path, String& body) {
  JSONBufferGuard jsonGuard(JSON_LOCK_UNKNOWN);
  if (!jsonGuard) {
    return false;
  }

  pDoc->clear();

  if (path == F("/json/state")) {
    JsonObject root = pDoc->to<JsonObject>();
    serializeState(root);
  } else if (path == F("/json/info")) {
    JsonObject root = pDoc->to<JsonObject>();
    serializeInfo(root);
  } else if (path == F("/json/si") || path == F("/json")) {
    JsonObject root = pDoc->to<JsonObject>();
    JsonObject state = root.createNestedObject("state");
    serializeState(state);
    JsonObject info = root.createNestedObject("info");
    serializeInfo(info);
  } else if (path == F("/json/effects")) {
    JsonArray arr = pDoc->to<JsonArray>();
    serializeModeNames(arr);
  } else if (path == F("/json/fxdata")) {
    JsonArray arr = pDoc->to<JsonArray>();
    serializeModeData(arr);
  } else if (path == F("/json/pins")) {
    JsonObject root = pDoc->to<JsonObject>();
    serializePins(root);
  } else if (path == F("/json/cfg")) {
    JsonObject root = pDoc->to<JsonObject>();
    serializeConfig(root);
  } else {
    return false;
  }

  body = String();
  serializeJson(*pDoc, body);
  return true;
}

String BleApiBridgeUsermod::normalizePath(const String& rawPath) const {
  String path = rawPath;
  path.trim();

  if (path.startsWith(F("/json"))) {
    int queryPos = path.indexOf('?');
    if (queryPos >= 0) path.remove(queryPos);
    if (path.length() > 5 && path.endsWith(F("/"))) path.remove(path.length() - 1);
  }

  return path;
}

bool BleApiBridgeUsermod::dispatchGet(const String& rawPath) {
  String path = normalizePath(rawPath);

  String body;
  if (!buildJsonBody(path, body)) {
    return queueErrorResponse(404, F("unsupported GET path"));
  }

  return queueResponse(200, "application/json", body);
}

bool BleApiBridgeUsermod::dispatchPostJsonState(const String& bodyText) {
  JSONBufferGuard jsonGuard(JSON_LOCK_UNKNOWN);
  if (!jsonGuard) {
    return queueErrorResponse(503, F("json buffer busy"));
  }

  pDoc->clear();
  DeserializationError error = deserializeJson(*pDoc, bodyText);
  if (error) {
    return queueErrorResponse(400, String(F("invalid json: ")) + error.c_str());
  }

  JsonObject root = pDoc->as<JsonObject>();
  if (root.isNull()) {
    return queueErrorResponse(400, F("expected JSON object"));
  }

  bool verboseResponse = deserializeState(root);
  jsonGuard.release();

  if (!verboseResponse) {
    return queueResponse(200, "application/json", buildSuccessEnvelope());
  }

  String responseBody;
  if (!buildJsonBody(F("/json"), responseBody)) {
    return queueErrorResponse(500, F("failed to serialize state response"));
  }

  return queueResponse(200, "application/json", responseBody);
}

bool BleApiBridgeUsermod::dispatchPostJsonConfig(const String& bodyText) {
  JSONBufferGuard jsonGuard(JSON_LOCK_UNKNOWN);
  if (!jsonGuard) {
    return queueErrorResponse(503, F("json buffer busy"));
  }

  pDoc->clear();
  DeserializationError error = deserializeJson(*pDoc, bodyText);
  if (error) {
    return queueErrorResponse(400, String(F("invalid json: ")) + error.c_str());
  }

  JsonObject root = pDoc->as<JsonObject>();
  if (root.isNull()) {
    return queueErrorResponse(400, F("expected JSON object"));
  }

  if (root.containsKey("pin")) {
    checkSettingsPIN(root["pin"].as<const char*>());
  }

  if (!correctPIN && strlen(settingsPIN) > 0) {
    return queueErrorResponse(401, F("settings PIN required"));
  }

  bool needsSave = deserializeConfig(root);
  if (needsSave) configNeedsWrite = true;

  return queueResponse(200, "application/json", buildSuccessEnvelope());
}

bool BleApiBridgeUsermod::dispatchPost(const String& rawPath, const String& bodyText) {
  String path = normalizePath(rawPath);
  if (path == F("/json") || path == F("/json/state")) {
    return dispatchPostJsonState(bodyText);
  }
  if (path == F("/json/cfg")) {
    return dispatchPostJsonConfig(bodyText);
  }
  return queueErrorResponse(404, F("unsupported POST path"));
}

bool BleApiBridgeUsermod::processRequestText(const String& requestText) {
  int headerEnd = requestText.indexOf(F("\r\n\r\n"));
  int separatorSize = 4;
  if (headerEnd < 0) {
    headerEnd = requestText.indexOf(F("\n\n"));
    separatorSize = 2;
  }

  String header = requestText;
  String body;
  if (headerEnd >= 0) {
    header = requestText.substring(0, headerEnd);
    body = requestText.substring(headerEnd + separatorSize);
  }

  int lineEnd = header.indexOf('\n');
  String requestLine = (lineEnd >= 0) ? header.substring(0, lineEnd) : header;
  requestLine.trim();
  int split = requestLine.indexOf(' ');
  if (split < 0) {
    return queueErrorResponse(400, F("request line must be 'METHOD PATH'"));
  }

  String method = requestLine.substring(0, split);
  String path = requestLine.substring(split + 1);
  method.toUpperCase();
  path.trim();

  if (method == F("GET")) {
    return dispatchGet(path);
  }

  if (method == F("POST")) {
    return dispatchPost(path, body);
  }

  return queueErrorResponse(405, F("unsupported method"));
}

void BleApiBridgeUsermod::processBleRequest() {
  if (!_request.ready || !_requestBuffer) return;

  String requestText;
  requestText.reserve(_request.expected);
  for (size_t i = 0; i < _request.expected; i++) {
    requestText += static_cast<char>(_requestBuffer[i]);
  }

  clearRequestAssembly();
  _processingBleRequest = true;
  bool ok = processRequestText(requestText);
  _processingBleRequest = false;

  if (!ok && !_response.active) {
    queueErrorResponse(500, F("failed to queue response"));
  }
}

} // namespace ble_api_bridge

#endif
