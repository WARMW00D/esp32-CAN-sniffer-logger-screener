/*
  ESP32-S3 Super Mini — CAN → BLE шлюз для HUD (без SD-карты и часов)
  ------------------------------------------------------------------------------
  Упрощённая версия сниффера: только принимает шину CAN и отдаёт по BLE кадры,
  прошедшие фильтр ACL, который присылает HUD. Логов, портала и WiFi нет.

  Для HUD шлюз неотличим от сниффера: то же имя BLE (S3-CAN-Sniffer), те же
  UUID и тот же протокол (docs/BLE_ACL_protocol_ru.md):
    …0007  WRITE/READ  список правил ACL
    …0008  NOTIFY      кадры пачками
    …0002  NOTIFY      синхронизация времени {millis, unixEpoch} (для камеры);
                       эпоха берётся из кадра 0x6B2 (часы машины), до него — 0

  Питание — только от линии зажигания (ACC, клемма 15) через понижайку:
  включили зажигание — шлюз работает, выключили — обесточен. Сна, датчика
  ACC и потребления на стоянке нет.

  Подключение (ESP32-S3 Super Mini):
    IO4 (TWAI TX) ->  CTX (D, TXD) трансивера
    IO5 (TWAI RX) <-  CRX (R, RXD) трансивера
    Трансивер — рекомендуется TJA1051T/3 (CJMCU-1051; SN65HVD230 — на свой
    страх и риск): VCC 5 В, VIO 3.3 В,
    S -> VIO (аппаратный silent: передатчик отключён физически, шину
    не займёт ни при каком сбое или перезагрузке ESP32).
    !!! TX к TX, RX к RX — линии НЕ перекрещиваются, как в UART.
    !!! SN65HVD230 — чип на 3.3 В: VCC только от 3.3 В (НЕ 5 В!), Rs -> GND,
        выводов VIO и S нет. Схемы в docs/ — для TJA1051T/3.
    !!! Терминатор 120 Ом на модуле трансивера (резистор «121») выпаять ДО
        установки — даже если замер его не показывает (бывает непропай).
    Стенд с одним другим узлом: S -> GND и CAN_LISTEN_ONLY 0.

  Настройки Arduino IDE: ESP32S3 Dev Module, Flash 4MB, PSRAM "QSPI PSRAM"
  (или Disabled — PSRAM не нужна), USB CDC On Boot "Enabled".
  Библиотека: NimBLE-Arduino 2.x.
  Секреты: BLE_PASSKEY — в secrets.h (шаблон secrets.example.h, в репозиторий не
  попадает). Защита BLE: код доступа, один клиент, сброс сопряжений —
  удержание BOOT 5 с на работающем устройстве (см. docs/BLE_pairing_protocol_ru.md).

*/

#include <Arduino.h>
#include <NimBLEDevice.h>

#if __has_include("secrets.h")
  #include "secrets.h"
#else
  #error "Нет secrets.h: скопируйте secrets.example.h в secrets.h и задайте свои значения"
#endif
#include "esp_random.h"
#include <time.h>
#include "driver/twai.h"

#define FW_VERSION   "1.2.0"
#define FW_BUILD     __DATE__ " " __TIME__

// ---------- CAN ----------
#define TWAI_TX_PIN        GPIO_NUM_4    // -> CTX
#define TWAI_RX_PIN        GPIO_NUM_5    // <- CRX
#define CAN_LISTEN_ONLY    1             // 1 — машина, 0 — стенд с одним передатчиком
// Скорость: 1000000 / 800000 / 500000 / 250000 / 125000 / 100000 / 50000
#define CAN_BITRATE        500000

// ---------- Светодиод WS2812 на плате ----------
#define RGB_LED_PIN         48
#define RGB_LED_BRIGHTNESS  24

// ---------- BLE: как у сниффера ----------
#define BLE_NAME          "S3-CAN-Sniffer"     // HUD ищет это имя
#define SYNC_SERVICE_UUID "A1B2C3D4-0001-41A2-9E3B-000000000001"
#define SYNC_CHAR_UUID    "A1B2C3D4-0001-41A2-9E3B-000000000002"
#define ACL_CHAR_UUID     "A1B2C3D4-0001-41A2-9E3B-000000000007"
#define FRAMES_CHAR_UUID  "A1B2C3D4-0001-41A2-9E3B-000000000008"
#define ACL_VERSION       0x01
#define ACL_MAX_RULES     32
#define ACL_BATCH_MS      20
#define ACL_QUEUE_LEN     256
#define ACL_STATE_SLOTS   256
#define ACL_PERMIT    0x01
#define ACL_EXT       0x02
#define ACL_ONCHANGE  0x04
#define ACL_ANYFMT    0x08

// =====================================================================
// ACL и поток кадров — тот же код, что в сниффере и проигрывателе
// =====================================================================
struct __attribute__((packed)) AclRule { uint8_t flags; uint8_t reserved; uint16_t minIntervalMs; uint32_t id; uint32_t mask; };
struct AclSet { uint8_t n; AclRule r[ACL_MAX_RULES]; };
static AclSet aclSets[2];
static AclSet* volatile aclCur = &aclSets[0];
struct __attribute__((packed)) AclFrame { uint16_t ts; uint32_t idf; uint8_t dlc; uint8_t data[8]; };
struct AclState { uint32_t key; uint32_t lastMs; uint8_t dlc; uint8_t data[8]; };
static AclState aclState[ACL_STATE_SLOTS];

QueueHandle_t bleQueue = nullptr;
NimBLECharacteristic* syncCharacteristic = nullptr;
NimBLECharacteristic* aclCharacteristic = nullptr;
NimBLECharacteristic* framesCharacteristic = nullptr;
volatile bool     framesSubscribed = false;
volatile uint16_t aclOwnerConn = 0xFFFF;
volatile uint16_t framesMtu = 23;
volatile bool     bleDropFlag = false;
volatile uint32_t bleSentTotal = 0, bleDroppedTotal = 0, lastBleSendMs = 0;

// =====================================================================
// Защита BLE: сопряжение по коду доступа (LE Secure Connections), один
// запомненный клиент, сброс сопряжений удержанием BOOT 5 с.
//   BLE_PASSKEY (secrets.h): шесть цифр; 0 — защита выключена, доступ
//   открыт всем (как раньше). Протокол для клиентов (HUD, камера):
//   docs/BLE_pairing_protocol_ru.md
//   - устройство "показывает" код (IO capability DisplayOnly), клиент
//     "вводит" его программно — человек не нужен;
//   - пока клиент сопряжён (слот занят), новые сопряжения не принимаются:
//     выдаётся случайный код. Сброс — BOOT 5 с на РАБОТАЮЩЕМ устройстве;
//   - соединение без сопряжения отключается через BLE_AUTH_TIMEOUT_MS;
//     5 неудач подряд — сопряжение блокируется на 60 с;
//   - подписки и запись ACL принимаются только по зашифрованному каналу.
// =====================================================================
#define BLE_SECURE             (BLE_PASSKEY != 0)
#define BLE_MAX_BONDS          1        // сколько клиентов можно запомнить
#define BLE_AUTH_TIMEOUT_MS    15000    // за это время клиент должен пройти сопряжение
#define BLE_MAX_AUTH_FAILS     5        // неудач подряд до блокировки
#define BLE_LOCK_MS            60000    // длительность блокировки сопряжения
#define BLE_RESET_PIN          0        // кнопка BOOT
#define BLE_RESET_HOLD_MS      5000     // сколько держать для сброса сопряжений

#if BLE_SECURE
  #define BLE_PROP_AUTH_RW  (NIMBLE_PROPERTY::READ_AUTHEN | NIMBLE_PROPERTY::WRITE_AUTHEN)
  #define BLE_PROP_AUTH_W   (NIMBLE_PROPERTY::WRITE_AUTHEN)
#else
  #define BLE_PROP_AUTH_RW  0
  #define BLE_PROP_AUTH_W   0
#endif

volatile uint16_t bleConnHandle = 0xFFFF;   // единственное допустимое соединение
volatile uint32_t bleConnAt = 0;            // когда оно установлено
volatile bool     bleLinkSecure = false;    // канал зашифрован и аутентифицирован
volatile bool     bleConnFailCounted = false; // неудача этого соединения уже учтена
volatile uint8_t  bleAuthFails = 0;
volatile uint32_t bleLockUntil = 0;         // до какого момента сопряжение заблокировано
volatile bool     bleResetHolding = false;  // идёт отсчёт удержания BOOT (для светодиода)
volatile uint32_t bleResetDoneAt = 0;       // когда выполнен сброс (для светодиода)

// Клиент прошёл защиту? При BLE_PASSKEY 0 допускаются все.
static bool bleAuthorized(const NimBLEConnInfo& ci) {
#if BLE_SECURE
  return ci.isEncrypted() && ci.isAuthenticated();
#else
  (void)ci;
  return true;
#endif
}

static void bleAuthFailed(const char* why) {
  bleConnFailCounted = true;
  bleAuthFails++;
  Serial.printf("BLE: %s (неудач подряд: %u)\n", why, (unsigned)bleAuthFails);
  if (bleAuthFails >= BLE_MAX_AUTH_FAILS) {
    bleAuthFails = 0;
    bleLockUntil = millis() + BLE_LOCK_MS;
    Serial.println("BLE: слишком много неудач — сопряжение заблокировано на 60 с");
  }
}

// Стереть все сопряжения и отключить клиентов (BOOT 5 с)
static void bleDoResetPairings() {
  NimBLEDevice::deleteAllBonds();
  NimBLEServer* s = NimBLEDevice::getServer();
  if (s) {
    for (uint16_t h : s->getPeerDevices()) s->disconnect(h);
  }
  bleAuthFails = 0;
  bleLockUntil = 0;
  bleResetDoneAt = millis();
  Serial.println("BLE: все сопряжения сброшены (BOOT 5 с) — устройство готово к новому сопряжению");
}

// Клиент не прошёл сопряжение за BLE_AUTH_TIMEOUT_MS — отключаем
static void bleSecurityWatchdog() {
#if BLE_SECURE
  uint16_t h = bleConnHandle;
  if (h != 0xFFFF && !bleLinkSecure && millis() - bleConnAt > BLE_AUTH_TIMEOUT_MS) {
    bleConnHandle = 0xFFFF;
    if (!bleConnFailCounted) bleAuthFailed("клиент не прошёл сопряжение вовремя");
    Serial.println("BLE: отключаю клиента без сопряжения");
    NimBLEServer* s = NimBLEDevice::getServer();
    if (s) s->disconnect(h);
  }
#endif
}

// Отдельная задача: кнопка BOOT и сторожевой таймер сопряжения
static void bleResetTask(void*) {
  pinMode(BLE_RESET_PIN, INPUT_PULLUP);
  uint32_t t0 = 0;
  for (;;) {
    if (digitalRead(BLE_RESET_PIN) == LOW) {
      if (!t0) t0 = millis();
      uint32_t held = millis() - t0;
      bleResetHolding = held >= 500;                 // короче 0.5 с — не считаем
      if (held >= BLE_RESET_HOLD_MS) {
        bleResetHolding = false;
        bleDoResetPairings();
        while (digitalRead(BLE_RESET_PIN) == LOW) vTaskDelay(pdMS_TO_TICKS(50));   // ждём отпускания
        t0 = 0;
      }
    } else {
      t0 = 0;
      bleResetHolding = false;
    }
    bleSecurityWatchdog();
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// Вызывается из setupBLE() сразу после NimBLEDevice::init()
static void bleSecuritySetup() {
#if BLE_SECURE
  NimBLEDevice::setSecurityAuth(true, true, true);              // bonding, MITM, Secure Connections
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);       // мы "показываем" код, клиент вводит
  NimBLEDevice::setSecurityInitKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);
  NimBLEDevice::setSecurityRespKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);
  Serial.printf("BLE: защита включена (код доступа), запомнено клиентов: %d из %d\n",
                NimBLEDevice::getNumBonds(), BLE_MAX_BONDS);
  xTaskCreatePinnedToCore(bleResetTask, "bleReset", 3072, NULL, 1, NULL, 1);
#else
  Serial.println("BLE: защита ВЫКЛЮЧЕНА (BLE_PASSKEY 0 в secrets.h) — доступ открыт всем");
#endif
}


bool hudReady() { return framesSubscribed && aclCur->n > 0; }

void aclProcessFrame(uint32_t id, bool ext, bool rtr, uint8_t dlc, const uint8_t* data) {
  AclSet* set = aclCur;
  const AclRule* hit = nullptr;
  for (uint8_t i = 0; i < set->n; i++) {
    const AclRule& r = set->r[i];
    if (!(r.flags & ACL_ANYFMT) && (bool)(r.flags & ACL_EXT) != ext) continue;
    if (((id ^ r.id) & r.mask) == 0) { hit = &r; break; }
  }
  if (!hit || !(hit->flags & ACL_PERMIT)) return;
  if (dlc > 8) dlc = 8;
  if ((hit->flags & ACL_ONCHANGE) || hit->minIntervalMs) {
    uint32_t key = id | (ext ? 0x80000000u : 0) | 0x40000000u;
    uint32_t h = (key * 2654435761u) >> 24;
    AclState* st = nullptr;
    for (int p = 0; p < 8; p++) {
      AclState& c = aclState[(h + p) & (ACL_STATE_SLOTS - 1)];
      if (c.key == key || c.key == 0) { st = &c; break; }
    }
    if (st) {
      uint32_t now = millis();
      if (st->key != 0) {
        if ((hit->flags & ACL_ONCHANGE) && st->dlc == dlc && memcmp(st->data, data, dlc) == 0) return;
        if (hit->minIntervalMs && now - st->lastMs < hit->minIntervalMs) return;
      }
      st->key = key; st->lastMs = now; st->dlc = dlc;
      memcpy(st->data, data, dlc);
    }
  }
  AclFrame f;
  f.ts = (uint16_t)millis();
  f.idf = id | (ext ? 0x80000000u : 0) | (rtr ? 0x40000000u : 0);
  f.dlc = rtr ? 0 : dlc;
  memcpy(f.data, data, f.dlc);
  // Из задачи приёма CAN не ждём: очередь полна — кадр теряется и считается
  if (xQueueSend(bleQueue, &f, 0) != pdTRUE) { bleDroppedTotal++; bleDropFlag = true; }
}

void bleTxTask(void*) {
  static uint8_t buf[512];
  size_t len = 2; uint8_t cnt = 0; uint32_t first = 0;
  auto flush = [&]() {
    if (!cnt) return;
    buf[0] = cnt; buf[1] = bleDropFlag ? 0x01 : 0x00; bleDropFlag = false;
    framesCharacteristic->setValue(buf, len);
    framesCharacteristic->notify();
    bleSentTotal += cnt; lastBleSendMs = millis();
    len = 2; cnt = 0;
  };
  for (;;) {
    AclFrame f;
    bool got = xQueueReceive(bleQueue, &f, pdMS_TO_TICKS(cnt ? 5 : 200)) == pdTRUE;
    size_t maxPayload = framesMtu > 3 ? framesMtu - 3 : 20;
    if (maxPayload > sizeof(buf)) maxPayload = sizeof(buf);
    if (got && framesSubscribed) {
      size_t need = 7 + f.dlc;
      if (len + need > maxPayload || cnt == 255) flush();
      if (!cnt) first = millis();
      memcpy(buf + len, &f.ts, 2); memcpy(buf + len + 2, &f.idf, 4);
      buf[len + 6] = f.dlc; memcpy(buf + len + 7, f.data, f.dlc);
      len += need; cnt++;
    }
    if (cnt && (!framesSubscribed || millis() - first >= ACL_BATCH_MS)) {
      if (framesSubscribed) flush(); else { len = 2; cnt = 0; }
    }
  }
}


// Счётчики для Serial и светодиода
volatile uint32_t canFramesTotal = 0, lastCanFrameMs = 0;
volatile bool     canRunning = false;

// ---------- Время машины из 0x6B2 (для синхронизации камеры) ----------
volatile uint32_t carEpoch = 0, carEpochAtMs = 0;   // эпоха и millis момента приёма

static inline uint32_t canSig(const uint8_t* d, int start, int len) {
  uint64_t raw = 0;
  for (int i = 7; i >= 0; i--) raw = (raw << 8) | d[i];
  return (uint32_t)((raw >> start) & ((1ULL << len) - 1));
}

void carTimeFrame(const twai_message_t& m) {
  if (m.identifier != 0x6B2 || m.extd || m.rtr || m.data_length_code < 8) return;
  int Y = 2000 + canSig(m.data, 28, 7), M = canSig(m.data, 35, 4), D = canSig(m.data, 39, 5);
  int h = canSig(m.data, 44, 5), mi = canSig(m.data, 49, 6), s = canSig(m.data, 55, 6);
  if (Y < 2026 || Y > 2040 || M < 1 || M > 12 || D < 1 || D > 31 || h > 23 || mi > 59 || s > 59) return;
  struct tm t = {};
  t.tm_year = Y - 1900; t.tm_mon = M - 1; t.tm_mday = D; t.tm_hour = h; t.tm_min = mi; t.tm_sec = s;
  setenv("TZ", "UTC0", 1); tzset();
  time_t e = mktime(&t);
  if (e > 0) { carEpoch = (uint32_t)e; carEpochAtMs = millis(); }
}

// =====================================================================
// BLE
// =====================================================================
struct __attribute__((packed)) SyncPacket { uint32_t millisValue; uint32_t unixEpoch; };

class ServerCallbacks : public NimBLEServerCallbacks {
  // Один клиент на устройство: второго подключившегося сразу отключаем.
  // Реклама после подключения не возобновляется до отключения клиента
  // (advertiseOnDisconnect(false) задан при создании сервера).
  void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) override {
    if (pServer->getConnectedCount() > 1) {
      Serial.println("BLE: второй клиент — отключаю (устройство принимает одного)");
      pServer->disconnect(connInfo.getConnHandle());
      return;
    }
    bleConnHandle = connInfo.getConnHandle();
    bleConnAt = millis();
    bleLinkSecure = false;
    bleConnFailCounted = false;
    Serial.printf("BLE: подключился %s\n", connInfo.getAddress().toString().c_str());
#if BLE_SECURE
    NimBLEDevice::startSecurity(connInfo.getConnHandle());      // требуем сопряжение сразу
#endif
  }

  // Код, который должен ввести клиент. Слот занят (клиент уже сопряжён) или
  // идёт блокировка после неудач — выдаём случайное число: новое сопряжение
  // невозможно, а запомненный клиент входит по сохранённым ключам.
  uint32_t onPassKeyDisplay() override {
    bool slotFull = NimBLEDevice::getNumBonds() >= BLE_MAX_BONDS;
    bool locked = millis() < bleLockUntil;
    if (slotFull || locked) {
      Serial.println(slotFull ? "BLE: слот занят — новое сопряжение не принимается (сброс: BOOT 5 с)"
                              : "BLE: сопряжение временно заблокировано");
      return 100000 + (esp_random() % 900000);
    }
    return BLE_PASSKEY;
  }

  void onAuthenticationComplete(NimBLEConnInfo& connInfo) override {
    if (connInfo.isEncrypted() && connInfo.isAuthenticated()) {
      bleLinkSecure = true;
      bleAuthFails = 0;
      Serial.println("BLE: канал зашифрован, клиент прошёл сопряжение");
    } else {
      // Не отключаем сразу: клиент может сам повторить сопряжение (например,
      // после потери ключа). Недопущённого отключит сторожевой таймер, а
      // подписки и запись ACL до сопряжения всё равно отклоняются.
      bleAuthFailed("сопряжение не удалось");
    }
  }

  void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) override {
    bool wasMain = (connInfo.getConnHandle() == bleConnHandle);
    if (wasMain) { bleConnHandle = 0xFFFF; bleLinkSecure = false; }
    if (connInfo.getConnHandle() == aclOwnerConn) {      // автор списка ушёл — список сбрасываем
      aclCur->n = 0;
      framesSubscribed = false;
      aclOwnerConn = 0xFFFF;
      Serial.println("BLE: клиент ACL отключился — поток кадров остановлен");
    }
    // Рекламу возобновляем, только когда ушёл основной клиент
    if (wasMain || bleConnHandle == 0xFFFF) NimBLEDevice::startAdvertising();
  }
};

class AclCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& ci) override {
    if (!bleAuthorized(ci)) { Serial.println("BLE ACL: клиент не прошёл сопряжение — список не принят"); return; }
    NimBLEAttValue v = c->getValue();
    size_t L = v.length(); const uint8_t* d = v.data();
    if (L < 1 || d[0] != ACL_VERSION || (L - 1) % sizeof(AclRule) != 0) {
      Serial.printf("BLE ACL: неверный формат (%u байт)\n", (unsigned)L);
      return;
    }
    size_t n = (L - 1) / sizeof(AclRule);
    if (n > ACL_MAX_RULES) n = ACL_MAX_RULES;
    AclSet* next = (aclCur == &aclSets[0]) ? &aclSets[1] : &aclSets[0];
    memcpy(next->r, d + 1, n * sizeof(AclRule));
    next->n = n;
    memset(aclState, 0, sizeof(aclState));
    aclCur = next;
    aclOwnerConn = ci.getConnHandle();
    Serial.printf("BLE ACL: принято правил %u\n", (unsigned)n);
  }
};

class FramesCallbacks : public NimBLECharacteristicCallbacks {
  void onSubscribe(NimBLECharacteristic*, NimBLEConnInfo& ci, uint16_t sub) override {
    if (sub != 0 && !bleAuthorized(ci)) { framesSubscribed = false; Serial.println("BLE: подписка отклонена — нет сопряжения (подписывайтесь после шифрования)"); return; }
    framesSubscribed = sub != 0;
    framesMtu = ci.getMTU();
    Serial.printf("BLE: подписка на поток %s, MTU %u\n", framesSubscribed ? "вкл" : "выкл", framesMtu);
  }
};

class SyncCallbacks : public NimBLECharacteristicCallbacks {
  void onSubscribe(NimBLECharacteristic* c, NimBLEConnInfo& ci, uint16_t sub) override {
    if (sub != 0 && !bleAuthorized(ci)) { Serial.println("BLE: подписка на время отклонена — нет сопряжения"); return; }
    if (!sub) return;
    uint32_t now = millis();
    uint32_t epoch = carEpoch ? carEpoch + (now - carEpochAtMs) / 1000 : 0;
    SyncPacket p = { now, epoch };
    c->setValue((uint8_t*)&p, sizeof(p));
    c->notify();
    Serial.printf("BLE: синхронизация времени отправлена (эпоха %lu)\n", (unsigned long)epoch);
  }
};

void setupBLE() {
  NimBLEDevice::init(BLE_NAME);
  bleSecuritySetup();                 // защита: код доступа, один клиент, сброс по BOOT
  NimBLEDevice::setMTU(247);
  bleQueue = xQueueCreate(ACL_QUEUE_LEN, sizeof(AclFrame));
  xTaskCreatePinnedToCore(bleTxTask, "bleTx", 4096, NULL, 1, NULL, 1);

  NimBLEServer* srv = NimBLEDevice::createServer();
  srv->setCallbacks(new ServerCallbacks());
  srv->advertiseOnDisconnect(false);   // рекламу возобновляем сами (один клиент)
  NimBLEService* svc = srv->createService(SYNC_SERVICE_UUID);
  syncCharacteristic = svc->createCharacteristic(SYNC_CHAR_UUID, NIMBLE_PROPERTY::NOTIFY);
  syncCharacteristic->setCallbacks(new SyncCallbacks());
  aclCharacteristic = svc->createCharacteristic(ACL_CHAR_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::READ | BLE_PROP_AUTH_RW,
                                                1 + ACL_MAX_RULES * sizeof(AclRule));
  aclCharacteristic->setCallbacks(new AclCallbacks());
  framesCharacteristic = svc->createCharacteristic(FRAMES_CHAR_UUID, NIMBLE_PROPERTY::NOTIFY, 512);
  framesCharacteristic->setCallbacks(new FramesCallbacks());
  svc->start();

  NimBLEService* dis = srv->createService("180A");
  dis->createCharacteristic("2A29", NIMBLE_PROPERTY::READ)->setValue("DIY");
  dis->createCharacteristic("2A24", NIMBLE_PROPERTY::READ)->setValue("S3-CAN-Gateway");
  dis->createCharacteristic("2A26", NIMBLE_PROPERTY::READ)->setValue(FW_VERSION);
  dis->createCharacteristic("2A28", NIMBLE_PROPERTY::READ)->setValue(FW_BUILD);
  dis->start();

  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->setName(BLE_NAME);
  adv->enableScanResponse(true);
  adv->addServiceUUID(SYNC_SERVICE_UUID);
  adv->start();
  Serial.println("BLE: реклама запущена (" BLE_NAME ")");
}

// =====================================================================
// CAN: приём и фильтр ACL
// =====================================================================
twai_timing_config_t canTiming() {
  switch (CAN_BITRATE) {
    case 1000000: { twai_timing_config_t t = TWAI_TIMING_CONFIG_1MBITS();   return t; }
    case  800000: { twai_timing_config_t t = TWAI_TIMING_CONFIG_800KBITS(); return t; }
    case  250000: { twai_timing_config_t t = TWAI_TIMING_CONFIG_250KBITS(); return t; }
    case  125000: { twai_timing_config_t t = TWAI_TIMING_CONFIG_125KBITS(); return t; }
    case  100000: { twai_timing_config_t t = TWAI_TIMING_CONFIG_100KBITS(); return t; }
    case   50000: { twai_timing_config_t t = TWAI_TIMING_CONFIG_50KBITS();  return t; }
    default:      { twai_timing_config_t t = TWAI_TIMING_CONFIG_500KBITS(); return t; }
  }
}

void canTask(void*) {
#if CAN_LISTEN_ONLY
  twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT(TWAI_TX_PIN, TWAI_RX_PIN, TWAI_MODE_LISTEN_ONLY);
#else
  twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT(TWAI_TX_PIN, TWAI_RX_PIN, TWAI_MODE_NORMAL);
#endif
  g.rx_queue_len = 64;
  twai_timing_config_t t = canTiming();
  twai_filter_config_t f = TWAI_FILTER_CONFIG_ACCEPT_ALL();
  esp_err_t err = twai_driver_install(&g, &t, &f);
  if (err == ESP_OK) err = twai_start();
  if (err != ESP_OK) {
    Serial.printf("CAN: ОШИБКА запуска TWAI (%s)\n", esp_err_to_name(err));
    vTaskDelete(NULL);
  }
  canRunning = true;
  Serial.printf("CAN: запущен, %s, %d бит/с\n", CAN_LISTEN_ONLY ? "LISTEN_ONLY" : "NORMAL", CAN_BITRATE);

  twai_message_t m;
  for (;;) {
    if (twai_receive(&m, pdMS_TO_TICKS(1000)) != ESP_OK) continue;
    canFramesTotal++;
    lastCanFrameMs = millis();
    carTimeFrame(m);
    if (framesSubscribed && aclCur->n)
      aclProcessFrame(m.identifier, m.extd, m.rtr, m.data_length_code, m.data);
  }
}

// =====================================================================
// Светодиод: красный — CAN не запустился; синий — ждём HUD;
// зелёный — HUD подключён (+ белый, пока кадры уходят по BLE)
// =====================================================================
void ledTask(void*) {
  const uint8_t B = RGB_LED_BRIGHTNESS;
  for (;;) {
    // Сброс сопряжений BLE: фиолетовое мигание — держите BOOT; белое — сброшено
    if (bleResetHolding || (bleResetDoneAt && millis() - bleResetDoneAt < 1500)) {
      bool done = !bleResetHolding;
      rgbLedWrite(RGB_LED_PIN, B, done ? B : 0, B);
      vTaskDelay(pdMS_TO_TICKS(100));
      rgbLedWrite(RGB_LED_PIN, 0, 0, 0);
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }
    bool hud = framesSubscribed && aclCur->n;
    if (!canRunning) rgbLedWrite(RGB_LED_PIN, B, 0, 0);
    else if (!hud)   rgbLedWrite(RGB_LED_PIN, 0, 0, B);
    else             rgbLedWrite(RGB_LED_PIN, 0, B, 0);
    vTaskDelay(pdMS_TO_TICKS(80));
    rgbLedWrite(RGB_LED_PIN, 0, 0, 0);
    uint32_t used = 80;
    if (hud && millis() - lastBleSendMs < 1000) {
      vTaskDelay(pdMS_TO_TICKS(150));
      rgbLedWrite(RGB_LED_PIN, B, B, B);
      vTaskDelay(pdMS_TO_TICKS(40));
      rgbLedWrite(RGB_LED_PIN, 0, 0, 0);
      used += 190;
    }
    vTaskDelay(pdMS_TO_TICKS(1000 - used));
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.printf("\n=== S3 CAN→BLE Gateway %s (сборка %s) ===\n", FW_VERSION, FW_BUILD);

  setupBLE();
  xTaskCreatePinnedToCore(canTask, "canTask", 4096, NULL, 2, NULL, 0);
  xTaskCreatePinnedToCore(ledTask, "ledTask", 3072, NULL, 1, NULL, 1);
}

void loop() {
  // Раз в минуту — статистика
  static uint32_t last = 0, lastFr = 0, lastSent = 0, lastDrop = 0;
  if (millis() - last >= 60000) {
    uint32_t fr = canFramesTotal, se = bleSentTotal, dr = bleDroppedTotal;
    if (last) Serial.printf("[STAT] CAN %.0f кадров/с, BLE отправлено %.0f/с, потеряно %lu (всего %lu)\n",
                            (fr - lastFr) / 60.0f, (se - lastSent) / 60.0f,
                            (unsigned long)(dr - lastDrop), (unsigned long)dr);
    last = millis(); lastFr = fr; lastSent = se; lastDrop = dr;
  }
  delay(50);
}
