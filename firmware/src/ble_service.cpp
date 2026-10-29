#include "ble_service.h"

#include <NimBLEDevice.h>

namespace ble {

static NimBLEServer* server = nullptr;
static NimBLECharacteristic* chr_status = nullptr;
static uint32_t passkey = 0;
static volatile uint32_t cursor = 0;  // log download position (one client at a time)

// Read + notify/write permissions all require an encrypted, passkey-authenticated link.
static const uint32_t kReadSecure = NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_ENC | NIMBLE_PROPERTY::READ_AUTHEN;
static const uint32_t kWriteSecure = NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC | NIMBLE_PROPERTY::WRITE_AUTHEN;

static String advertisedName(const char* machine_name) {
  // The BLE name is what shows in the phone's scan list.
  String n = machine_name;
  return n.substring(0, 24);
}

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* s, NimBLEConnInfo& info) override {
    // One phone at a time keeps the download cursor simple and unambiguous.
    if (s->getConnectedCount() > 1) {
      s->disconnect(info.getConnHandle());
      return;
    }
    cursor = 0;
    Serial.printf("[ble] connected %s\n", info.getAddress().toString().c_str());
  }
  void onDisconnect(NimBLEServer* s, NimBLEConnInfo& info, int reason) override {
    Serial.printf("[ble] disconnected (reason 0x%02x)\n", reason);
    // advertiseOnDisconnect(true) restarts advertising automatically.
  }
  uint32_t onPassKeyDisplay() override { return passkey; }
  void onAuthenticationComplete(NimBLEConnInfo& info) override {
    if (!info.isEncrypted() || !info.isAuthenticated()) {
      Serial.println("[ble] pairing failed - wrong passkey? disconnecting");
      server->disconnect(info.getConnHandle());
      return;
    }
    Serial.println("[ble] paired and encrypted");
  }
};

class StatusCallbacks : public NimBLECharacteristicCallbacks {
  void onRead(NimBLECharacteristic* c, NimBLEConnInfo&) override {
    StatusPayload p = appStatus();
    c->setValue((const uint8_t*)&p, sizeof(p));
  }
};

class TimeCallbacks : public NimBLECharacteristicCallbacks {
  void onRead(NimBLECharacteristic* c, NimBLEConnInfo&) override {
    int64_t now = appClockNowMs();
    c->setValue((const uint8_t*)&now, sizeof(now));
  }
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo&) override {
    NimBLEAttValue v = c->getValue();
    if (v.length() != 8) return;
    int64_t t;
    memcpy(&t, v.data(), 8);
    appRequestTimeSet(t);
  }
};

class CursorCallbacks : public NimBLECharacteristicCallbacks {
  void onRead(NimBLECharacteristic* c, NimBLEConnInfo&) override {
    uint32_t v = cursor;
    c->setValue((const uint8_t*)&v, sizeof(v));
  }
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo&) override {
    NimBLEAttValue v = c->getValue();
    if (v.length() != 4) return;
    uint32_t seq;
    memcpy(&seq, v.data(), 4);
    cursor = seq;
  }
};

class PageCallbacks : public NimBLECharacteristicCallbacks {
  // Returns up to 32 records starting at the cursor. Reading does NOT move the
  // cursor: the app writes last_seq + 1 to the cursor before the next read.
  // That makes every read repeatable, so a retried read can never skip data.
  void onRead(NimBLECharacteristic* c, NimBLEConnInfo&) override {
    static EventRecord page[CSW_PAGE_RECORDS];
    size_t n = appReadLog(cursor, page, CSW_PAGE_RECORDS);
    c->setValue((const uint8_t*)page, n * sizeof(EventRecord));
  }
};

class ConfigCallbacks : public NimBLECharacteristicCallbacks {
  void onRead(NimBLECharacteristic* c, NimBLEConnInfo&) override {
    String js = appConfigJson();
    c->setValue((const uint8_t*)js.c_str(), js.length());
  }
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo&) override {
    NimBLEAttValue v = c->getValue();
    appRequestConfig((const char*)v.data(), v.length());
  }
};

void begin(const Settings& s) {
  passkey = s.passkey;
  NimBLEDevice::init(advertisedName(s.name).c_str());

  // bonding=true (remember the phone), mitm=true (require passkey),
  // sc=true (LE Secure Connections, the modern key exchange).
  NimBLEDevice::setSecurityAuth(true, true, true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);  // "I show a code, you type it"
  NimBLEDevice::setSecurityPasskey(passkey);

  server = NimBLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());
  server->advertiseOnDisconnect(true);

  NimBLEService* svc = server->createService(CSW_SERVICE_UUID);
  chr_status = svc->createCharacteristic(CSW_CHR_STATUS_UUID, kReadSecure | NIMBLE_PROPERTY::NOTIFY);
  chr_status->setCallbacks(new StatusCallbacks());
  svc->createCharacteristic(CSW_CHR_TIME_UUID, kReadSecure | kWriteSecure)->setCallbacks(new TimeCallbacks());
  svc->createCharacteristic(CSW_CHR_CURSOR_UUID, kReadSecure | kWriteSecure)->setCallbacks(new CursorCallbacks());
  svc->createCharacteristic(CSW_CHR_PAGE_UUID, kReadSecure, CSW_PAGE_RECORDS * sizeof(EventRecord))
      ->setCallbacks(new PageCallbacks());
  svc->createCharacteristic(CSW_CHR_CONFIG_UUID, kReadSecure | kWriteSecure, 512)->setCallbacks(new ConfigCallbacks());
  svc->start();

  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(CSW_SERVICE_UUID);
  adv->setName(advertisedName(s.name).c_str());
  adv->enableScanResponse(true);  // the 128-bit UUID fills the main packet; the name goes in the scan response
  adv->start();
  Serial.printf("[ble] advertising as \"%s\"\n", advertisedName(s.name).c_str());
}

void notifyStatus() {
  if (!chr_status) return;
  StatusPayload p = appStatus();
  chr_status->setValue((const uint8_t*)&p, sizeof(p));
  if (connected()) chr_status->notify();
}

void applySettings(const Settings& s) {
  if (s.passkey != passkey) {
    passkey = s.passkey;
    NimBLEDevice::setSecurityPasskey(passkey);
  }
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  bool was_advertising = adv->isAdvertising();
  if (was_advertising) adv->stop();
  NimBLEDevice::setDeviceName(advertisedName(s.name).c_str());
  adv->setName(advertisedName(s.name).c_str());
  if (was_advertising) adv->start();
}

bool connected() { return server && server->getConnectedCount() > 0; }

void clearBonds() {
  NimBLEDevice::deleteAllBonds();
  Serial.println("[ble] all pairings deleted");
}

}  // namespace ble
