// User settings, kept in the ESP32's NVS (a small key-value store in flash
// that survives reboots and firmware updates).
#pragma once
#include <stdint.h>
#include "lamp_filter.h"

struct Settings {
  char name[25];       // machine name, shown in the app and in the BLE device name
  uint32_t passkey;    // 6-digit BLE pairing code
  FilterConfig filter;
};

namespace settings {
void load(Settings* s);  // fills defaults for anything not yet stored
void save(const Settings& s);
}  // namespace settings
