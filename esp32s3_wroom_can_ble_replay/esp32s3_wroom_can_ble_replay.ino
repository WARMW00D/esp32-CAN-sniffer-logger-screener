/*
  ESP32-S3-WROOM-1 CAM (N16R8) — проигрыватель CAN-логов для HUD по BLE
  ------------------------------------------------------------------------------
  Стендовая замена снифферу: HUD подключается к нему так же, как к снифферу в
  машине (то же имя BLE, те же UUID и тот же протокол ACL), а проигрыватель
  отдаёт кадры из ранее записанных логов в реальном времени.

  Как пользоваться:
    1. Положите логи сниффера (can_log_NNNN.txt.lzma или .txt) в папку
       /replay на microSD — можно целыми папками по датам внутри неё.
       Порядок проигрывания — по именам (дата, затем номер файла).
    2. Включите проигрыватель, подключите HUD.
    3. HUD подписывается на поток кадров (…0008) и присылает список ACL
       (…0007) — проигрыватель начинает отдавать отфильтрованные кадры
       с теми же интервалами, что были в машине.
  Протокол — docs/BLE_ACL_protocol_ru.md. Новый список ACL (или
  переподключение HUD) — проигрывание с начала.

  Время: интервалы между кадрами берутся из millis в логе. Маркер BOOT
  (новое включение зажигания) — пауза REPLAY_SESSION_GAP_MS и отсчёт заново.
  Файлы-продолжения (CONTINUED) идут без паузы.

  Светодиод WS2812 (GPIO48), вспышка раз в секунду:
    синий   — ждём HUD (нет подписки или списка ACL);
    зелёный — идёт проигрывание (+ белая вспышка, если кадры уходят по BLE);
    красный — нет SD-карты или нет файлов в /replay.

  Настройки Arduino IDE: ESP32S3 Dev Module, Flash 16MB, PSRAM "OPI PSRAM",
  USB CDC On Boot "Enabled". Библиотека: NimBLE-Arduino 2.x.
  Декодер LZMA (LZMA SDK, public domain) — в папке src/lzma скетча.
*/

#include <Arduino.h>
#include <SD_MMC.h>
#include <NimBLEDevice.h>
#include <vector>
#include <algorithm>
#include "esp_heap_caps.h"
#include "src/lzma/LzmaDec.h"

#define FW_VERSION   "1.0.0"
#define FW_BUILD     __DATE__ " " __TIME__

// ---------- Проигрывание ----------
#define REPLAY_DIR              "/replay"
#define REPLAY_LOOP             1       // 1 — по кругу, 0 — один раз
#define REPLAY_SPEED            1.0f    // 2.0 — вдвое быстрее, 0.5 — вдвое медленнее
#define REPLAY_SESSION_GAP_MS   1000    // пауза между сессиями (маркер BOOT)
#define REPLAY_MAX_LAG_MS       500     // отстали больше — догоняем не пачкой, а сдвигом

// ---------- Пины платы ESP32-S3-WROOM-1 CAM ----------
#define SDMMC_CLK_PIN   39
#define SDMMC_CMD_PIN   38
#define SDMMC_D0_PIN    40
#define RGB_LED_PIN     48
#define RGB_LED_BRIGHTNESS  24
#define BOARD_LED_PIN   2

// ---------- BLE: как у сниффера ----------
#define BLE_NAME          "S3-CAN-Sniffer"   // HUD ищет это имя — менять не нужно
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

// Кадр из лога. Объявлен до первой функции: Arduino IDE вставляет
// автопрототипы перед первой функцией, и тип должен быть уже известен
struct RpFrame { uint32_t ms; uint32_t id; bool ext, rtr; uint8_t dlc; uint8_t data[8]; };

static void* rpBigAlloc(size_t n) {
  void* p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);   // словарь LZMA — в PSRAM
  return p ? p : malloc(n);
}

// =====================================================================
// ACL и поток кадров — та же логика, что в сниффере
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
volatile uint32_t aclGeneration = 0;              // растёт при каждом новом списке
volatile uint32_t bleSentTotal = 0, bleDroppedTotal = 0, lastBleSendMs = 0;

// Состояние проигрывателя (для светодиода и Serial)
volatile bool     sdOk = false, filesOk = false, playing = false;
volatile uint32_t framesPlayed = 0;
String            curFile;

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
  // Проигрыватель может подождать (в отличие от сниффера на живой шине)
  if (xQueueSend(bleQueue, &f, pdMS_TO_TICKS(50)) != pdTRUE) { bleDroppedTotal++; bleDropFlag = true; }
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

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer*, NimBLEConnInfo& ci) override {
    Serial.printf("BLE: подключился %s\n", ci.getAddress().toString().c_str());
    NimBLEDevice::startAdvertising();
  }
  void onDisconnect(NimBLEServer*, NimBLEConnInfo& ci, int) override {
    if (ci.getConnHandle() == aclOwnerConn) {
      aclCur->n = 0; framesSubscribed = false; aclOwnerConn = 0xFFFF;
      Serial.println("BLE: HUD отключился — проигрывание остановлено");
    }
    NimBLEDevice::startAdvertising();
  }
};

class AclCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& ci) override {
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
    aclGeneration++;
    Serial.printf("BLE ACL: принято правил %u — проигрывание с начала\n", (unsigned)n);
  }
};

class FramesCallbacks : public NimBLECharacteristicCallbacks {
  void onSubscribe(NimBLECharacteristic*, NimBLEConnInfo& ci, uint16_t sub) override {
    framesSubscribed = sub != 0;
    framesMtu = ci.getMTU();
    Serial.printf("BLE: подписка на поток %s, MTU %u\n", framesSubscribed ? "вкл" : "выкл", framesMtu);
  }
};

// Синхронизация времени, как у сниффера: millis проигрывателя, эпохи нет (0)
struct __attribute__((packed)) SyncPacket { uint32_t millisValue; uint32_t unixEpoch; };
class SyncCallbacks : public NimBLECharacteristicCallbacks {
  void onSubscribe(NimBLECharacteristic* c, NimBLEConnInfo&, uint16_t sub) override {
    if (!sub) return;
    SyncPacket p = { (uint32_t)millis(), 0 };
    c->setValue((uint8_t*)&p, sizeof(p));
    c->notify();
  }
};

void setupBLE() {
  NimBLEDevice::init(BLE_NAME);
  NimBLEDevice::setMTU(247);
  bleQueue = xQueueCreate(ACL_QUEUE_LEN, sizeof(AclFrame));
  xTaskCreatePinnedToCore(bleTxTask, "bleTx", 4096, NULL, 2, NULL, 1);

  NimBLEServer* srv = NimBLEDevice::createServer();
  srv->setCallbacks(new ServerCallbacks());
  NimBLEService* svc = srv->createService(SYNC_SERVICE_UUID);
  syncCharacteristic = svc->createCharacteristic(SYNC_CHAR_UUID, NIMBLE_PROPERTY::NOTIFY);
  syncCharacteristic->setCallbacks(new SyncCallbacks());
  aclCharacteristic = svc->createCharacteristic(ACL_CHAR_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::READ,
                                                1 + ACL_MAX_RULES * sizeof(AclRule));
  aclCharacteristic->setCallbacks(new AclCallbacks());
  framesCharacteristic = svc->createCharacteristic(FRAMES_CHAR_UUID, NIMBLE_PROPERTY::NOTIFY, 512);
  framesCharacteristic->setCallbacks(new FramesCallbacks());
  svc->start();

  NimBLEService* dis = srv->createService("180A");
  dis->createCharacteristic("2A29", NIMBLE_PROPERTY::READ)->setValue("DIY");
  dis->createCharacteristic("2A24", NIMBLE_PROPERTY::READ)->setValue("S3-CAN-Replay");   // модель: проигрыватель
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

// ---------------------------------------------------------------------
// Чтение лога построчно: простой .txt или .txt.lzma (распаковка на лету).
// Оборванный .lzma (без маркера конца) читается до места обрыва.
// ---------------------------------------------------------------------
static void* rpAllocF(ISzAllocPtr, size_t n) { return rpBigAlloc(n); }
static void  rpFreeF(ISzAllocPtr, void* p) { free(p); }
static const ISzAlloc rpAlloc = { rpAllocF, rpFreeF };

template <class F>
class LogReader {
public:
  static const size_t IN_SZ = 4096, OUT_SZ = 8192;

  bool open(F file, bool lzma) {
    close();
    f = file; isLzma = lzma; outLen = outPos = 0; inLen = inPos = 0; eof = false; truncated = false;
    if (!inBuf)  inBuf  = (uint8_t*)malloc(IN_SZ);
    if (!outBuf) outBuf = (uint8_t*)malloc(OUT_SZ);
    if (!inBuf || !outBuf) return false;
    if (isLzma) {
      uint8_t hdr[13];
      if (f.read(hdr, 13) != 13) return false;
      LzmaDec_Construct(&dec);
      if (LzmaDec_Allocate(&dec, hdr, LZMA_PROPS_SIZE, &rpAlloc) != SZ_OK) return false;
      LzmaDec_Init(&dec);
      decOpen = true;
    }
    return true;
  }

  void close() {
    if (decOpen) { LzmaDec_Free(&dec, &rpAlloc); decOpen = false; }
  }

  bool wasTruncated() const { return truncated; }

  // Следующая строка без \r\n; false — конец файла
  bool readLine(char* line, size_t cap) {
    size_t n = 0;
    for (;;) {
      if (outPos >= outLen && !refill()) {
        if (n) { line[n] = 0; return true; }
        return false;
      }
      while (outPos < outLen) {
        char c = (char)outBuf[outPos++];
        if (c == '\n') { line[n] = 0; return true; }
        if (c != '\r' && n + 1 < cap) line[n++] = c;
      }
    }
  }

private:
  F f;
  bool isLzma = false, decOpen = false, eof = false, truncated = false;
  CLzmaDec dec;
  uint8_t* inBuf = nullptr; uint8_t* outBuf = nullptr;
  size_t inLen = 0, inPos = 0, outLen = 0, outPos = 0;

  bool refill() {
    outPos = outLen = 0;
    if (eof) return false;
    if (!isLzma) {
      int r = f.read(outBuf, OUT_SZ);
      if (r <= 0) { eof = true; return false; }
      outLen = r;
      return true;
    }
    while (outLen == 0) {
      if (inPos >= inLen) {
        int r = f.read(inBuf, IN_SZ);
        inLen = r > 0 ? r : 0; inPos = 0;
        if (inLen == 0) { eof = true; truncated = true; return false; }   // нет маркера конца
      }
      SizeT dstLen = OUT_SZ, srcLen = inLen - inPos;
      ELzmaStatus st;
      SRes res = LzmaDec_DecodeToBuf(&dec, outBuf, &dstLen, inBuf + inPos, &srcLen, LZMA_FINISH_ANY, &st);
      inPos += srcLen;
      outLen = dstLen;
      if (res != SZ_OK) { eof = true; truncated = true; return outLen > 0; }
      if (st == LZMA_STATUS_FINISHED_WITH_MARK) { eof = true; return outLen > 0; }
    }
    return true;
  }
};

// Разбор строки лога: "<millis> <S|X> <ID hex> [R]<DLC> <байты hex>" (RpFrame — в начале файла)

static int hexv(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

// 1 — кадр, 0 — пропустить, 2 — маркер BOOT (новая сессия, millis заново)
int parseLine(const char* s, RpFrame& fr) {
  if (s[0] == '#') return strstr(s, "BOOT") ? 2 : 0;
  char* e;
  unsigned long ms = strtoul(s, &e, 10);
  if (e == s || *e != ' ') return 0;
  s = e + 1;
  if (*s != 'S' && *s != 'X') return 0;
  fr.ext = (*s == 'X');
  s++; if (*s != ' ') return 0; s++;
  unsigned long id = strtoul(s, &e, 16);
  if (e == s || *e != ' ') return 0;
  s = e + 1;
  fr.rtr = false;
  if (*s == 'R') { fr.rtr = true; s++; }
  if (*s < '0' || *s > '8') return 0;
  fr.dlc = *s - '0'; s++;
  for (int i = 0; i < fr.dlc && !fr.rtr; i++) {
    if (*s != ' ') return 0;
    int h = hexv(s[1]), l = hexv(s[2]);
    if (h < 0 || l < 0) return 0;
    fr.data[i] = (uint8_t)(h << 4 | l);
    s += 3;
  }
  fr.ms = ms; fr.id = id;
  return 1;
}

// =====================================================================
// Список файлов для проигрывания: /replay и его подпапки, по именам
// =====================================================================
std::vector<String> playlist;

static bool isLogFile(const String& n) {
  return n.endsWith(".txt") || n.endsWith(".txt.lzma") || n.endsWith(".lzma");
}

void scanDir(const String& path, int depth) {
  File dir = SD_MMC.open(path);
  if (!dir || !dir.isDirectory()) return;
  File e = dir.openNextFile();
  while (e) {
    String name = String(e.name());
    if (name.lastIndexOf('/') != -1) name = name.substring(name.lastIndexOf('/') + 1);
    String full = path + "/" + name;
    if (e.isDirectory()) {
      if (depth < 2 && !name.startsWith(".")) scanDir(full, depth + 1);
    } else if (isLogFile(name)) {
      playlist.push_back(full);
    } else if (name.endsWith(".gz")) {
      Serial.printf("Пропуск %s: .gz не поддерживается — распакуйте (tools/log_unpack.py) или пишите в LZMA\n", full.c_str());
    }
    e.close();
    e = dir.openNextFile();
  }
  dir.close();
}

void buildPlaylist() {
  playlist.clear();
  scanDir(REPLAY_DIR, 0);
  std::sort(playlist.begin(), playlist.end(), [](const String& a, const String& b) { return a < b; });
  filesOk = !playlist.empty();
  Serial.printf("В %s найдено файлов: %u\n", REPLAY_DIR, (unsigned)playlist.size());
  for (auto& p : playlist) Serial.println("  " + p);
}

// =====================================================================
// Проигрывание: кадры уходят в ACL с теми же интервалами, что в машине
// =====================================================================
LogReader<File> reader;

void playTask(void*) {
  static char line[160];
  for (;;) {
    if (!hudReady() || !filesOk) { playing = false; vTaskDelay(pdMS_TO_TICKS(50)); continue; }

    uint32_t gen = aclGeneration;
    bool restart = false;
    bool first = true, sessionBreak = false;
    uint32_t baseLog = 0, baseReal = 0, lastLogMs = 0;
    playing = true;
    Serial.println("Проигрывание: старт");

    for (size_t i = 0; i < playlist.size() && !restart; i++) {
      File f = SD_MMC.open(playlist[i]);
      bool lz = playlist[i].endsWith(".lzma");
      if (!f || !reader.open(f, lz)) {
        Serial.printf("Не удалось открыть %s — пропуск\n", playlist[i].c_str());
        if (f) f.close();
        continue;
      }
      curFile = playlist[i];
      Serial.printf("Файл %u/%u: %s\n", (unsigned)(i + 1), (unsigned)playlist.size(), playlist[i].c_str());

      RpFrame fr;
      while (reader.readLine(line, sizeof(line))) {
        if (!hudReady() || aclGeneration != gen) { restart = true; break; }
        int k = parseLine(line, fr);
        if (k == 2) { sessionBreak = true; continue; }
        if (k != 1) continue;

        // Новая шкала времени: начало, новая сессия или millis пошли назад
        if (first || sessionBreak || fr.ms < lastLogMs) {
          baseLog = fr.ms;
          baseReal = millis() + (first ? 0 : REPLAY_SESSION_GAP_MS);
          first = false; sessionBreak = false;
        }
        lastLogMs = fr.ms;
        uint32_t target = baseReal + (uint32_t)((fr.ms - baseLog) / REPLAY_SPEED);

        // Ждём момента кадра (с проверкой, не ушёл ли HUD)
        for (;;) {
          int32_t wait = (int32_t)(target - millis());
          if (wait <= 0) break;
          vTaskDelay(pdMS_TO_TICKS(wait > 20 ? 20 : wait));
          if (!hudReady() || aclGeneration != gen) { restart = true; break; }
        }
        if (restart) break;
        // Сильно отстали (медленная карта/BLE) — сдвигаем шкалу, не шлём пачкой
        int32_t late = (int32_t)(millis() - target);
        if (late > REPLAY_MAX_LAG_MS) baseReal += late;

        aclProcessFrame(fr.id, fr.ext, fr.rtr, fr.dlc, fr.data);
        framesPlayed++;
      }
      if (reader.wasTruncated()) Serial.printf("  (файл оборван — проигран до места обрыва)\n");
      reader.close();
      f.close();
    }

    if (!restart) {
      Serial.println("Проигрывание: конец списка");
      if (!REPLAY_LOOP) {
        playing = false;
        while (hudReady() && aclGeneration == gen) vTaskDelay(pdMS_TO_TICKS(200));
      }
    }
  }
}

// =====================================================================
// Светодиод состояния
// =====================================================================
void ledTask(void*) {
  const uint8_t B = RGB_LED_BRIGHTNESS;
  for (;;) {
    if (!sdOk || !filesOk)   rgbLedWrite(RGB_LED_PIN, B, 0, 0);   // красный
    else if (!hudReady())    rgbLedWrite(RGB_LED_PIN, 0, 0, B);   // синий
    else                     rgbLedWrite(RGB_LED_PIN, 0, B, 0);   // зелёный
    vTaskDelay(pdMS_TO_TICKS(80));
    rgbLedWrite(RGB_LED_PIN, 0, 0, 0);
    uint32_t used = 80;
    if (playing && millis() - lastBleSendMs < 1000) {
      vTaskDelay(pdMS_TO_TICKS(150));
      rgbLedWrite(RGB_LED_PIN, B, B, B);                           // белый: кадры уходят
      vTaskDelay(pdMS_TO_TICKS(40));
      rgbLedWrite(RGB_LED_PIN, 0, 0, 0);
      used += 190;
    }
    vTaskDelay(pdMS_TO_TICKS(1000 - used));
  }
}

void setup() {
  pinMode(BOARD_LED_PIN, OUTPUT);
  digitalWrite(BOARD_LED_PIN, LOW);
  Serial.begin(115200);
  delay(300);
  Serial.printf("\n=== S3 CAN Replay %s (сборка %s) ===\n", FW_VERSION, FW_BUILD);

  SD_MMC.setPins(SDMMC_CLK_PIN, SDMMC_CMD_PIN, SDMMC_D0_PIN);
  sdOk = SD_MMC.begin("/sdcard", true, false, SDMMC_FREQ_DEFAULT);
  if (!sdOk) Serial.println("SD-карта не смонтирована!");
  else buildPlaylist();

  setupBLE();
  xTaskCreatePinnedToCore(playTask, "play", 8192, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(ledTask, "led", 3072, NULL, 1, NULL, 0);
}

void loop() {
  static uint32_t last = 0, lastPlayed = 0, lastSent = 0;
  if (millis() - last >= 10000) {
    uint32_t p = framesPlayed, s = bleSentTotal;
    if (playing)
      Serial.printf("[STAT] %s: кадров из лога %.0f/с, отправлено по BLE %.0f/с, потеряно %lu\n",
                    curFile.c_str(), (p - lastPlayed) / 10.0f, (s - lastSent) / 10.0f,
                    (unsigned long)bleDroppedTotal);
    lastPlayed = p; lastSent = s; last = millis();
  }
  delay(100);
}
