#include "settings.h"

#include <Arduino.h>
#include <Preferences.h>

namespace settings {

void load(Settings* s) {
  Preferences p;
  p.begin("csw", false);

  String name = p.getString("name", "");
  if (name.length() == 0) {
    // Default name from the chip's MAC so several unnamed units can be told apart.
    char buf[sizeof(s->name)];
    snprintf(buf, sizeof(buf), "CSW-%04X", (uint16_t)(ESP.getEfuseMac() >> 32));
    name = buf;
  }
  strlcpy(s->name, name.c_str(), sizeof(s->name));

  // A random passkey is created once, on first boot, and printed on the serial
  // log so it can be written on the unit's label during bench commissioning.
  if (!p.isKey("passkey")) p.putULong("passkey", 100000 + esp_random() % 900000);
  s->passkey = p.getULong("passkey");

  FilterConfig d;
  s->filter.invert = p.getBool("invert", d.invert);
  s->filter.debounce_ms = p.getUShort("debounce", d.debounce_ms);
  s->filter.settle_ms = p.getUShort("settle", d.settle_ms);
  p.end();
}

void save(const Settings& s) {
  Preferences p;
  p.begin("csw", false);
  p.putString("name", s.name);
  p.putULong("passkey", s.passkey);
  p.putBool("invert", s.filter.invert);
  p.putUShort("debounce", s.filter.debounce_ms);
  p.putUShort("settle", s.filter.settle_ms);
  p.end();
}

}  // namespace settings
