/*
  AI-Thinker ESP32-CAM — видео с синхронизацией времени по BLE (+ даты)
  ------------------------------------------------------------------------------
  Дополнено: BLE-пакет синхронизации теперь содержит не только millis()
  сниффера, но и unix-эпоху с его DS3231. У камеры своего RTC нет —
  вместо этого при получении синхропакета она ОДИН РАЗ выставляет себе
  системные часы (settimeofday) на основе полученной эпохи, и дальше
  всю сессию использует localtime() для формирования папки по дате
  (/YYYY-MM-DD/frames/...), синхронно с тем, как это делает сниффер.

  Если синхронизация не удалась (сниффер не найден за SYNC_TIMEOUT_MS) —
  папка будет называться /no-sync/frames/, чтобы не путать с реальными
  датами при последующем разборе.

  Для юстировки камеры при монтаже используйте ОТДЕЛЬНЫЙ скетч
  esp32cam_aithinker_aiming_stream.ino — переключение между ним и этой
  рабочей прошивкой через физический переключатель на GPIO0 (BOOT/режим
  прошивки), а не джампер.

  Библиотеки: esp32-camera, SD_MMC.h — из ядра Arduino-ESP32,
  h2zero/NimBLE-Arduino — для BLE-клиента.
*/

#include "esp_camera.h"
#include "SD_MMC.h"
#include "FS.h"
#include <NimBLEDevice.h>
#include <sys/time.h>
#include <time.h>

// ---------- Стандартный pinout камеры AI-Thinker ESP32-CAM ----------
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

#define CAPTURE_INTERVAL_MS   500

// ---------- Встроенная вспышка (яркий белый LED) — принудительно гасим ----------
// На AI-Thinker ESP32-CAM вспышка висит на GPIO4. Явно инициализируем
// его как выход и держим LOW на протяжении всей работы — иначе пин
// может остаться плавающим/подхватывать наводки от LEDC-таймера камеры
// и вспышка будет подмигивать не пойми от чего, засвечивая кадры
// приборки прямо перед объективом.
#define FLASH_LED_PIN  4

// ---------- Контроль питания (диод Шоттки + суперконденсатор) ----------
// Точка съёма сигнала — ДО диода, на стороне платы COM-порта/FTDI, а не
// после него (после диода суперконденсатор будет держать напряжение
// искусственно высоким ещё какое-то время после реального пропадания
// внешнего питания — сигнал оттуда пришёл бы с запозданием).
// Напряжение делится резистивным делителем с 5В (или что там на выходе
// FTDI-платы) до безопасного для ADC уровня (макс. ~3.3В на пине).
// !!! Раз этот пин теперь занят, SD-карта возвращена в 1-битный режим
// (было полноценных 4 бита, пока CAN не переехал на отдельную плату) —
// это освобождает GPIO4/12/13, из них берём GPIO13 (не strapping-пин,
// без побочных эффектов вроде GPIO4, который делит линию со встроенным
// светодиодом-фонариком платы).
#define POWER_SENSE_PIN    13
#define POWER_LOST_THRESHOLD  1500   // подберите по факту делителя;
                                      // ниже этого значения ADC — считаем,
                                      // что внешнее питание пропало

// ---------- BLE синхронизация — те же UUID, что и на стороне сниффера ----------
#define SYNC_SERVICE_UUID   "A1B2C3D4-0001-41A2-9E3B-000000000001"
#define SYNC_CHAR_UUID      "A1B2C3D4-0001-41A2-9E3B-000000000002"
#define SNIFFER_DEVICE_NAME "S3-CAN-Sniffer"
#define SYNC_TIMEOUT_MS      10000

// Должно совпадать по раскладке байт с SyncPacket на стороне сниффера
struct __attribute__((packed)) SyncPacket {
  uint32_t millisValue;
  uint32_t unixEpoch;
};

volatile bool syncReceived = false;
volatile int64_t timeOffset = 0;   // для millis() — как и раньше
String frameFolder = "/no-sync/frames"; // дефолт на случай неудачной синхронизации

void syncNotifyCallback(NimBLERemoteCharacteristic* pChar, uint8_t* pData, size_t length, bool isNotify) {
  if (length >= sizeof(SyncPacket)) {
    SyncPacket packet;
    memcpy(&packet, pData, sizeof(packet));

    uint32_t localMillis = millis();
    timeOffset = (int64_t)packet.millisValue - (int64_t)localMillis;

    char buf[32];
    if (packet.unixEpoch == 0) {
      // Сниффер сам без достоверного времени (DS3231 не найден / потерял
      // питание) — шкала millis общая, а дату взять неоткуда. Папка та же,
      // что у сниффера в этом случае: /no-rtc
      snprintf(buf, sizeof(buf), "/no-rtc/frames");
    } else {
      // Выставляем системные часы платы по полученной эпохе — один раз,
      // дальше localtime() всю сессию будет отдавать корректную дату
      struct timeval tv = { .tv_sec = (time_t)packet.unixEpoch, .tv_usec = 0 };
      settimeofday(&tv, NULL);

      time_t t = (time_t)packet.unixEpoch;
      struct tm* tmInfo = localtime(&t);
      snprintf(buf, sizeof(buf), "/%04d-%02d-%02d/frames",
               tmInfo->tm_year + 1900, tmInfo->tm_mon + 1, tmInfo->tm_mday);
    }
    frameFolder = String(buf);

    syncReceived = true;
    Serial.printf("Синхронизация получена: millis_offset=%lld, epoch=%lu, папка=%s\n",
                  (long long)timeOffset, (unsigned long)packet.unixEpoch, buf);
  }
}

uint32_t syncedMillis() {
  return (uint32_t)((int64_t)millis() + timeOffset);
}

bool connectToSniffer() {
  NimBLEDevice::init("");
  NimBLEScan* pScan = NimBLEDevice::getScan();
  pScan->setActiveScan(true);

  Serial.println("Сканирую BLE в поисках сниффера...");
  // NimBLE-Arduino 2.x: время в МИЛЛИСЕКУНДАХ, вызов через getResults(),
  // не через start() — в 1.x было иначе (start(секунды, ...))
  NimBLEScanResults results = pScan->getResults(5000, false);

  Serial.printf("Найдено устройств при сканировании: %d\n", results.getCount());
  const NimBLEAdvertisedDevice* target = nullptr;
  for (int i = 0; i < results.getCount(); i++) {
    const NimBLEAdvertisedDevice* dev = results.getDevice(i);
    // Печатаем ВСЁ, что видим в эфире — так сразу понятно, светится ли
    // сниффер вообще под каким-то именем/адресом, или сигнала нет совсем
    Serial.printf("  [%d] имя:'%s' адрес:%s RSSI:%d\n",
                   i, dev->getName().c_str(),
                   dev->getAddress().toString().c_str(), dev->getRSSI());
    if (dev->getName() == SNIFFER_DEVICE_NAME) {
      target = dev;
    }
  }

  if (!target) {
    Serial.println("Сниффер не найден при сканировании");
    return false;
  }

  NimBLEClient* pClient = NimBLEDevice::createClient();
  if (!pClient->connect(target)) {
    Serial.println("Не удалось подключиться к снифферу");
    return false;
  }

  NimBLERemoteService* pService = pClient->getService(SYNC_SERVICE_UUID);
  if (!pService) {
    Serial.println("Сервис синхронизации не найден");
    return false;
  }

  NimBLERemoteCharacteristic* pChar = pService->getCharacteristic(SYNC_CHAR_UUID);
  if (!pChar) {
    Serial.println("Характеристика синхронизации не найдена");
    return false;
  }

  if (!pChar->subscribe(true, syncNotifyCallback)) {
    Serial.println("Не удалось подписаться на уведомления");
    return false;
  }

  Serial.println("Подписка оформлена, жду пакет синхронизации...");
  return true;
}

void setupCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;

  if (psramFound()) {
    config.frame_size = FRAMESIZE_SVGA;
    config.jpeg_quality = 10;
    config.fb_count = 2;
    config.fb_location = CAMERA_FB_IN_PSRAM;
  } else {
    config.frame_size = FRAMESIZE_VGA;
    config.jpeg_quality = 12;
    config.fb_count = 1;
    config.fb_location = CAMERA_FB_IN_DRAM;
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Ошибка инициализации камеры: 0x%x\n", err);
  } else {
    Serial.println("Камера инициализирована");
  }
}

void setupSD() {
  // 1-битный режим (true) — обязательно, GPIO13 нужен под контроль питания.
  // CAN на этой плате больше не используется, GPIO12 (strapping-риск)
  // трогать по-прежнему не будем, оставляем незанятым про запас.
  if (!SD_MMC.begin("/sdcard", true)) {
    Serial.println("Ошибка инициализации SD-карты!");
    return;
  }
  Serial.println("SD-карта инициализирована (1-bit режим — из-за контроля питания)");
}

void ensureFrameFolder() {
  // Создаём папку по дате (или /no-sync) непосредственно перед первой
  // записью — раньше не получится, т.к. дата становится известна только
  // после успешной BLE-синхронизации (или после таймаута)
  String datePart = frameFolder.substring(0, frameFolder.lastIndexOf('/'));
  if (datePart.length() > 0 && !SD_MMC.exists(datePart)) {
    SD_MMC.mkdir(datePart);
  }
  if (!SD_MMC.exists(frameFolder)) {
    SD_MMC.mkdir(frameFolder);
  }
}

void checkPowerAndShutdownIfNeeded() {
  int raw = analogRead(POWER_SENSE_PIN);

  if (raw < POWER_LOST_THRESHOLD) {
    Serial.println("!!! Внешнее питание пропало — работаем от суперконденсатора,");
    Serial.println("успеваем корректно завершить запись...");

    // Больше не открываем новых файлов — предыдущий кадр (если был)
    // уже закрыт сразу после записи (см. loop()), так что "зависших"
    // открытых файлов тут в норме нет. Явно отмонтируем SD, чтобы
    // сбросить любые внутренние кэши библиотеки на уровне ФС.
    SD_MMC.end();
    Serial.println("SD отмонтирована штатно. Ложимся в глубокий сон.");
    delay(50); // дать Serial дописать перед тем, как всё обесточится

    // Источник пробуждения не задаём — тут и не нужен: следующий раз
    // чип получит обычный holodный старт, когда на плату COM-порта
    // снова придёт внешнее питание (зажигание/прикуриватель включат).
    // Пока суперконденсатор не разрядится окончательно, deep sleep
    // просто сведёт потребление к минимуму, чтобы не жечь его энергию
    // впустую на работающий процессор.
    esp_deep_sleep_start();
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);

  pinMode(FLASH_LED_PIN, OUTPUT);
  digitalWrite(FLASH_LED_PIN, LOW); // гасим вспышку сразу же, до всего остального

  analogReadResolution(12);
  analogSetPinAttenuation(POWER_SENSE_PIN, ADC_11db);

  setupSD();
  setupCamera();

  bool connected = connectToSniffer();

  uint32_t waitStart = millis();
  while (connected && !syncReceived && (millis() - waitStart) < SYNC_TIMEOUT_MS) {
    delay(100);
  }

  if (syncReceived) {
    Serial.println("Запись начинается с синхронизированным временем и датой");
  } else {
    Serial.println("!!! ВНИМАНИЕ: синхронизация НЕ получена, папка будет /no-sync/frames");
  }

  ensureFrameFolder();
}

void loop() {
  checkPowerAndShutdownIfNeeded(); // проверяем на КАЖДОЙ итерации — суперконденсатор
                                    // даёт мало времени, реагировать нужно быстро

  static uint32_t lastCapture = 0;
  uint32_t now = millis();

  if (now - lastCapture < CAPTURE_INTERVAL_MS) {
    return;
  }
  lastCapture = now;

  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("Ошибка захвата кадра");
    return;
  }

  uint32_t ts = syncedMillis();
  char filename[64];
  snprintf(filename, sizeof(filename), "%s/f_%010lu.jpg", frameFolder.c_str(), (unsigned long)ts);

  File f = SD_MMC.open(filename, FILE_WRITE);
  if (f) {
    f.write(fb->buf, fb->len);
    f.close();
  } else {
    Serial.println("Не удалось открыть файл кадра");
  }

  esp_camera_fb_return(fb);
  digitalWrite(FLASH_LED_PIN, LOW); // на всякий случай, после каждого кадра
}
