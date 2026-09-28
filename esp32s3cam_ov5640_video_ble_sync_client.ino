/*
  ESP32-S3-WROOM (N16R8) + OV5640 — запись кадров приборки с синхронизацией
  времени по BLE. Вариант скетча esp32cam_aithinker_video_ble_sync_client
  под плату ESP32-S3 CAM (Freenove ESP32-S3 WROOM CAM и её клоны,
  распиновка камеры как у ESP32-S3-EYE).
  ------------------------------------------------------------------------------
  Логика та же, что у версии для AI-Thinker:
    - при старте один раз подключается к снифферу "S3-CAN-Sniffer" по BLE,
      получает {millis, unixEpoch}, выравнивает millis() и выставляет
      системные часы (settimeofday) — дата для папки берётся из них;
    - кадры JPEG каждые CAPTURE_INTERVAL_MS в /YYYY-MM-DD/frames/f_<millis>.jpg,
      <millis> — на шкале времени СНИФФЕРА; без синхронизации — /no-sync/frames/;
    - при пропадании внешнего питания SD отмонтируется и чип уходит в сон.

  Отличия от версии для AI-Thinker:
    - другая распиновка камеры и SD (см. блоки ниже — ПРОВЕРЬТЕ по своей плате);
    - вспышки на плате нет; если на плате есть светодиод, который бликует
      в стекле приборки — укажите его пин в STATUS_LED_PIN;
    - контроль питания на GPIO1 (ADC1), GPIO13 здесь занят камерой;
    - OV5640 даёт больше разрешения: по умолчанию HD 1280×720 (FRAME_SIZE);
    - CAMERA_GRAB_LATEST: каждый кадр — свежий, а не лежавший в очереди,
      поэтому метка времени в имени файла соответствует моменту съёмки.

  Настройки Arduino IDE:
    Плата:            ESP32S3 Dev Module
    Flash Size:       16MB (128Mb)
    PSRAM:            OPI PSRAM        (у N16R8 — Octal, не QSPI!)
    Partition Scheme: 16M Flash (3MB APP/9.9MB FATFS) или Huge APP
    USB CDC On Boot:  Enabled — если Serial смотрите через порт "USB"
                      (родной USB чипа); Disabled — через порт "COM"/UART
                      (мост CH343 на плате)

  Для юстировки — отдельный скетч esp32s3cam_ov5640_aiming_stream.ino.

  Библиотеки: esp32-camera, SD_MMC.h — из ядра Arduino-ESP32 3.x,
  h2zero/NimBLE-Arduino 2.x — для BLE-клиента.
*/

#include "esp_camera.h"
#include "SD_MMC.h"
#include "FS.h"
#include <NimBLEDevice.h>
#include <sys/time.h>
#include <time.h>

// ---------- Распиновка камеры: ESP32-S3 CAM (Freenove / ESP32-S3-EYE) ----------
// Проверено по распиновке платы ESP32-S3-WROOM-1 CAM (N16R8, два USB-C).
#define PWDN_GPIO_NUM     -1
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM     15
#define SIOD_GPIO_NUM      4
#define SIOC_GPIO_NUM      5
#define Y9_GPIO_NUM       16
#define Y8_GPIO_NUM       17
#define Y7_GPIO_NUM       18
#define Y6_GPIO_NUM       12
#define Y5_GPIO_NUM       10
#define Y4_GPIO_NUM        8
#define Y3_GPIO_NUM        9
#define Y2_GPIO_NUM       11
#define VSYNC_GPIO_NUM     6
#define HREF_GPIO_NUM      7
#define PCLK_GPIO_NUM     13

// ---------- SD-карта (SD_MMC, 1-битный режим) ----------
// Слот microSD на Freenove ESP32-S3 WROOM CAM
#define SD_MMC_CLK_PIN    39
#define SD_MMC_CMD_PIN    38
#define SD_MMC_D0_PIN     40

// ---------- Параметры съёмки ----------
#define CAPTURE_INTERVAL_MS   500
// Разрешение. OV5640 умеет до QSXGA 2560×1920; на 500 мс хватает с запасом
// HD. Варианты: FRAMESIZE_SVGA (800×600), FRAMESIZE_XGA (1024×768),
// FRAMESIZE_HD (1280×720), FRAMESIZE_SXGA (1280×1024), FRAMESIZE_UXGA (1600×1200),
// FRAMESIZE_FHD (1920×1080). Больше разрешение — больше файлы и дольше запись.
#define FRAME_SIZE            FRAMESIZE_HD
#define JPEG_QUALITY          10     // 0..63, меньше — лучше качество и больше файл
// Ориентация: модули OV5640 часто стоят "вверх ногами" относительно OV2640
#define CAM_VFLIP             0
#define CAM_HMIRROR           1   // у этого модуля OV5640 картинка по умолчанию зеркальная
// Сколько кадров выбросить после старта — пока OV5640 подстраивает
// экспозицию и баланс белого, первые кадры получаются засвеченными/зелёными
#define WARMUP_FRAMES         8

// ---------- Светодиод на плате, который нужно держать погашенным ----------
// Вспышки, как на AI-Thinker, тут нет. Если какой-то светодиод платы
// отражается в стекле приборки — укажите пин (например, 2), иначе -1.
#define STATUS_LED_PIN        2   // светодиод IO2 на плате — держим погашенным

// ---------- Контроль питания (диод Шоттки + суперконденсатор) ----------
// Точка съёма сигнала — ДО диода, иначе суперконденсатор будет держать
// напряжение искусственно высоким и сигнал придёт с запозданием.
// Делитель от 5В до безопасного для ADC уровня (макс. ~3.1В при 11 дБ).
// GPIO1 = ADC1_CH0 (ADC1 работает вместе с радио, ADC2 — нет).
// GPIO13, как на AI-Thinker, здесь занят камерой (PCLK).
#define POWER_SENSE_PIN        1
#define POWER_LOST_THRESHOLD   1500   // подберите по факту делителя;
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
  camera_config_t config = {};
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
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  // Всегда самый свежий кадр: метка времени в имени файла = момент съёмки,
  // а не момент, когда кадр был снят и полежал в очереди
  config.grab_mode = CAMERA_GRAB_LATEST;

  if (psramFound()) {
    config.frame_size   = FRAME_SIZE;
    config.jpeg_quality = JPEG_QUALITY;
    config.fb_count     = 2;
    config.fb_location  = CAMERA_FB_IN_PSRAM;
  } else {
    // Без PSRAM (не выставлен OPI PSRAM в настройках?) — скромный режим
    Serial.println("!!! PSRAM не найден — проверьте PSRAM: OPI PSRAM. Снимаю в VGA");
    config.frame_size   = FRAMESIZE_VGA;
    config.jpeg_quality = 12;
    config.fb_count     = 1;
    config.fb_location  = CAMERA_FB_IN_DRAM;
    config.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Ошибка инициализации камеры: 0x%x\n", err);
    return;
  }

  sensor_t* s = esp_camera_sensor_get();
  if (s) {
    const char* name = (s->id.PID == OV5640_PID) ? "OV5640" :
                       (s->id.PID == OV2640_PID) ? "OV2640" : "другой";
    Serial.printf("Камера инициализирована: сенсор %s (PID 0x%04X)\n", name, s->id.PID);
    s->set_vflip(s, CAM_VFLIP);
    s->set_hmirror(s, CAM_HMIRROR);
  }

  // Прогрев: даём автоэкспозиции и балансу белого устояться
  for (int i = 0; i < WARMUP_FRAMES; i++) {
    camera_fb_t* fb = esp_camera_fb_get();
    if (fb) esp_camera_fb_return(fb);
    delay(50);
  }
}

void setupSD() {
  // На S3 пины SD_MMC задаются явно; слот платы разведён под 1-битный режим
  SD_MMC.setPins(SD_MMC_CLK_PIN, SD_MMC_CMD_PIN, SD_MMC_D0_PIN);
  if (!SD_MMC.begin("/sdcard", true)) {
    Serial.println("Ошибка инициализации SD-карты!");
    return;
  }
  Serial.printf("SD-карта инициализирована (1-bit), %llu МБ\n",
                SD_MMC.cardSize() / (1024ULL * 1024ULL));
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
  Serial.println("\n=== ESP32-S3 CAM (OV5640) — запись кадров приборки ===");

#if STATUS_LED_PIN >= 0
  pinMode(STATUS_LED_PIN, OUTPUT);
  digitalWrite(STATUS_LED_PIN, LOW); // гасим сразу, до всего остального
#endif

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
}
