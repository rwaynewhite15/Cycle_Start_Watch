// BLE GATT server. Byte layouts are in protocol.h and docs/ble-protocol.md.
//
// Security: every characteristic requires an encrypted, authenticated
// (passkey) link. A phone that is not paired gets "insufficient
// authentication", and iOS/Android then show the pairing dialog by themselves.
// The passkey is fixed per unit (the ESP32 has no screen to show a random
// one), so treat it as a lock on the door, not a vault: it keeps casual
// passers-by out, and the device has no way to affect the machine anyway.
#pragma once
#include <Arduino.h>
#include "protocol.h"
#include "settings.h"

// Things the BLE layer needs from the application (implemented in main.cpp).
// They are called from the BLE task, so they must be quick and thread-safe.
StatusPayload appStatus();
size_t appReadLog(uint32_t from_seq, EventRecord* out, size_t max);
int64_t appClockNowMs();
void appRequestTimeSet(int64_t epoch_ms);              // applied on the main loop
String appConfigJson();
bool appRequestConfig(const char* json, size_t len);   // false = bad JSON / values

namespace ble {
void begin(const Settings& s);
void notifyStatus();                 // push the status characteristic to the phone
void applySettings(const Settings& s);  // name / passkey changed
bool connected();
void clearBonds();
}  // namespace ble
