/*
  ESP32-S3 Super Mini — автономный CAN-сниффер под приборкой (RTC + Web + BLE sync)
  ------------------------------------------------------------------------------
  Дополнено относительно предыдущей версии:
    - DS3231 (RTC-модуль по I2C) — эталонное время, держит ход даже без
      питания платы (собственная батарейка CR2032 на модуле)
    - Веб-страница для установки даты/времени на DS3231 (WiFi AP,
      мощность передачи снижена до 8.5dBm)
    - BLE-синхронизация теперь передаёт не только millis(), но и
      unix-время (эпоху) — камера-компаньон использует его для создания
      СВОИХ папок по датам, хотя у неё самой RTC нет
    - Лог пишется в папку по дате (/YYYY-MM-DD/can_log_NNNN.txt, ротация
      по LOG_MAX_BYTES и при каждом старте), а не в
      общий файл в корне — как и файлы кадров на камере

  Архитектура приёма/записи CAN та же, что и раньше: canTask (ядро 0)
  только читает TWAI и кладёт кадр в очередь, sdTask (ядро 1) разбирает
  очередь и пишет на SD — разделение специально ради того, чтобы
  запись на SD не блокировала приём при плотном трафике.

  Подключение (ESP32-S3 Super Mini):
    Трансивер — рекомендуется TJA1051T/3 (например, плата CJMCU-1051);
    SN65HVD230 тоже работает, но на свой страх и риск (нет вывода silent)
    -----------------------------
    IO4 (TWAI TX) -> CTX (TXD), IO5 (TWAI RX) <- CRX (RXD)
    VCC -> 5 В, VIO -> 3.3 В (питание логики — уровни 3.3 В для ESP32),
    S   -> VIO: аппаратный silent mode — передатчик отключён физически,
           трансивер не может занять шину ни при каком сбое, загрузке
           или перезагрузке ESP32. В машине рекомендуется именно так.
    !!! TX к TX, RX к RX — линии НЕ перекрещиваются, как в UART.
    !!! SN65HVD230 — чип на 3.3 В: VCC только от 3.3 В (НЕ 5 В!), Rs -> GND,
        выводов VIO и S нет. Схемы в docs/ — для TJA1051T/3.
    !!! Терминатор 120 Ом на модуле трансивера (резистор «121») выпаять ДО
        установки — даже если замер его не показывает (бывает непропай).
    Стенд с единственным другим узлом: S -> GND и CAN_LISTEN_ONLY 0
    (нужен ACK). В машине при S = VIO режим NORMAL работать не будет:
    контроллер не увидит своих ACK и уйдёт в ошибки — оставляйте LISTEN_ONLY.

    ESP32-S3           SD-модуль (SPI)
    -----------------------------
    IO10 -> CS     IO11 -> MOSI   IO12 -> SCK    IO13 -> MISO

    Часы (необязательно), DS3231 или PCF8563 — определяются автоматически:
    IO8 -> SDA     IO9 -> SCL     3.3V/GND -> VCC/GND

    ACC (оптрон, LOW = зажигание вкл.): IO7.

  Настройки Arduino IDE: ESP32S3 Dev Module, Flash 4MB, PSRAM "QSPI PSRAM",
  Partition "Minimal SPIFFS (1.9MB APP with OTA/190KB SPIFFS)", USB Mode
  "Hardware CDC and JTAG", USB CDC On Boot "Enabled".
  Вариант для платы ESP32-S3-WROOM-1 CAM — esp32s3_wroom_can_sd_ble_sync_server.

  Библиотеки (Arduino Library Manager):
    - adafruit/RTClib          (DS3231 / PCF8563)
    - h2zero/NimBLE-Arduino    (BLE-синхронизация)
    - SD.h, SPI.h, WiFi.h, WebServer.h, Wire.h, driver/twai.h —
      часть ядра Arduino-ESP32

  !!! РЕЖИМ ШИНЫ !!! Задаётся CAN_LISTEN_ONLY ниже:
    1 — машина (TWAI_MODE_LISTEN_ONLY, в шину не передаём даже ACK)
    0 — стенд  (TWAI_MODE_NORMAL, нужен, если на стенде всего один
        передатчик: без чужого ACK он будет бесконечно повторять кадр)

  Формат строки SD-лога (для bap_nav_decode.py):
    <millis> <S|X> <ID hex> [R]<DLC> <байты hex>
      S — 11-bit, ID 3 hex-цифры;  X — 29-bit, ID 8 hex-цифр
      R перед DLC — remote-кадр (байтов данных нет)
    Пример:  123456 S 30B 8 00 11 22 33 44 55 66 77
             123470 X 17333210 8 80 0A 4C 92 00 00 01 F4
    Служебные строки начинаются с '#' — парсеру их пропускать.

  Время: внешние часы необязательны. DS3231 или PCF8563 на I2C находятся
  автоматически; без них время берётся из кадра 0x6B2 с шины (CAN_TIME_SYNC),
  после сна — из системных часов ESP32, либо ставится на портале.

  Язык портала: RU/EN, красная кнопка вверху каждой страницы, хранится
  в NVS (ui/lang), по умолчанию русский. Serial-вывод всегда на русском.

  Скрытая сеть: WIFI_AP_HIDDEN 1 — SSID точки доступа не транслируется.

  Скорость CAN: выбирается на главной странице портала (http://192.168.4.1),
  хранится в NVS (ключ sniffer/can_bps), применяется перезапуском.
  Нет значения в NVS — CAN_BITRATE_DEFAULT (500 кбит/с). Текущая скорость
  пишется в Serial и в маркер BOOT в SD-логе (can_bps=...).

  Прошивка по воздуху (два способа, пароль общий — OTA_PASSWORD):
    - http://192.168.4.1/update — из браузера, логин admin (OTA_WEB_USER)
    - espota / Arduino IDE (сетевой порт s3-can-sniffer)
    Обоим нужен Partition Scheme с двумя app-слотами, например
    "Minimal SPIFFS (1.9MB APP with OTA/190KB SPIFFS)" для 4 МБ flash.
    Схему разделов меняет ТОЛЬКО прошивка по USB — один раз.

  Сон и USB:
    - Пока к USB подключён хост (комп), плата не уходит в deep sleep,
      даже если ACC выключено — удобно на стенде (Serial, прошивка).
      Детект по SOF-пакетам USB (Serial.isPlugged()), поэтому от
      "тупой" USB-зарядки в машине сон работает как обычно.
    - Требуемые настройки Arduino IDE:
        USB Mode:          Hardware CDC and JTAG
        USB CDC On Boot:   Enabled
    - Подтяжка ACC_PIN в сне делается через RTC-домен
      (rtc_gpio_pullup_en), иначе пин висит в воздухе и ext0 будит
      плату от наводок ("проснулись, но ACC уже выключено" по кругу).
    - ВНИМАНИЕ: если в машине питать сниффер от USB магнитолы/MIB,
      магнитола выступит хостом и сниффер не уснёт никогда.
      Для такой схемы питания выставьте USB_HOST_KEEPS_AWAKE в 0.
  Секреты: BLE_PASSKEY, AP_PASSWORD, OTA_PASSWORD, OTA_WEB_USER — в secrets.h (шаблон secrets.example.h, в репозиторий не
  попадает). Защита BLE: код доступа, один клиент, сброс сопряжений —
  удержание BOOT 5 с на работающем устройстве (см. docs/BLE_pairing_protocol_ru.md).

*/

// =====================================================================
// ВЕРСИЯ ПРОШИВКИ — менять при каждой сборке, которая уходит на плату.
//   MAJOR — несовместимые изменения (формат лога, HudState, BLE-UUID)
//   MINOR — новая функциональность
//   PATCH — исправления без изменения поведения/форматов
// Дата/время сборки подставляются компилятором автоматически.
// =====================================================================
#define FW_VERSION   "2.9.0"
#define FW_BUILD     __DATE__ " " __TIME__



#include "driver/twai.h"
#include <SPI.h>
#include <SD.h>
#include <Wire.h>
#include <RTClib.h>
#include <WiFi.h>
#include <WebServer.h>
#include <NimBLEDevice.h>
#include <vector>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <Update.h>
#include <sys/time.h>
#include <Preferences.h>

#if __has_include("secrets.h")
  #include "secrets.h"
#else
  #error "Нет secrets.h: скопируйте secrets.example.h в secrets.h и задайте свои значения"
#endif
#include "esp_random.h"
#include "freertos/stream_buffer.h"
// Кодер LZMA (LZMA SDK, public domain) — исходники в папке src/lzma скетча
#include "src/lzma/LzmaEnc.h"
#include "esp_app_format.h"
#include "driver/rtc_io.h"
#include "esp_sleep.h"

// Файловая система логов: SD_MMC на WROOM CAM, SD (SPI) на Super Mini.
// Весь код ниже работает через LOGFS — API у обеих одинаковый.
#define LOGFS SD

// ---------- Время ----------
// Источники времени (по убыванию приоритета при старте):
//   1. Часы на I2C, если стоят: DS3231 или PCF8563 — определяются сами.
//   2. После пробуждения из сна — системные часы ESP32 (идут и во сне).
//   3. Кадр 0x6B2 (Diagnose_01) с шины: дата/время машины, раз в секунду.
//   4. Портал: вручную или кнопкой "Время телефона".
// CAN_TIME_SYNC:
//   0 — время из CAN не использовать;
//   1 — брать из CAN, только если достоверного времени ещё нет;
//   2 — брать из CAN и подводить часы, если расходятся > CAN_TIME_MAX_DIFF_S.
// Внимание: после отключения аккумулятора часы машины сбиваются — тогда
// режим 2 выставит неверное время. Выставьте часы в машине (или GPS-синхро
// в MMI) либо переключитесь на 1/0.
// Время с годом меньше этого — ошибка: такое время не принимается ни от
// часов на I2C, ни из CAN, ни с портала (лог идёт в /no-rtc, светодиод жёлтый)
#define TIME_MIN_YEAR         2026
#define TIME_MAX_YEAR         2040

#define CAN_TIME_SYNC         2
#define CAN_TIME_ID           0x6B2
#define CAN_TIME_MAX_DIFF_S   5
// Если машина шлёт UTC, а нужно местное — сдвиг в часах (Москва: 3)
#define CAN_TIME_TZ_HOURS     0
// Защита от сбитых часов машины (например, после отключения аккумулятора):
//   0  — доверять машине полностью (время машины главнее всего);
//   >0 — если своё достоверное время уже есть (часы на I2C, сон, портал),
//        а машина расходится с ним больше чем на столько секунд — считаем
//        часы машины сбитыми и НЕ берём их время (пишем в Serial).
// Время машины раньше даты сборки прошивки отбрасывается всегда.
#define CAN_TIME_MAX_JUMP_S   0

// Сколько ждать кадр времени из CAN перед открытием первого файла, мс.
// Кадры за это время копятся в очереди и не теряются.
#define CAN_TIME_WAIT_MS      4500
// Защита от глюков (GPS, шина): время из CAN принимается, только если
// столько кадров 0x6B2 ПОДРЯД согласованы между собой — каждый отстоит от
// предыдущего ровно на прошедшее время (±2 с). Одиночный выброс не пройдёт.
// Кадр идёт раз в секунду: 3 кадра — ~2–3 с после появления шины.
#define CAN_TIME_CONFIRM_FRAMES  3

// ---------- RGB-светодиод состояния (WS2812 на плате) ----------
// Пока плата не спит — раз в секунду вспышка СОСТОЯНИЯ (по приоритету):
//   красный — ошибка, мешающая работе (SD не смонтирована, не открывается
//             файл лога, не запустился приём CAN);
//   жёлтый  — частичная работоспособность: время не установлено (лог идёт
//             в /no-rtc) или часы останавливались и время под вопросом;
//   синий   — к точке доступа подключён клиент (работает портал);
//   зелёный — всё в порядке.
// Следом — короткая БЕЛАЯ вспышка, если по шине идут данные CAN (кадр
// был за последнюю секунду). Едем: зелёный + белый. Шина молчит: только
// зелёный.
// Перед сном светодиод гасится (WS2812 без команды держит цвет).
#define RGB_LED_ENABLE      1
#define RGB_LED_PIN         48    // WS2812 на плате
#define RGB_LED_BRIGHTNESS  24    // 0..255 — под рулём хватает и малой яркости
#define RGB_LED_FLASH_MS    80    // длительность вспышки
#define RGB_LED_PERIOD_MS   1000  // период

// Флаги состояния для индикации
volatile bool sdMounted  = false;   // SD смонтирована
volatile bool canRunning = true;    // приём CAN запущен
volatile bool sdPortalMode = false;  // запись остановлена, SD на скорости портала
volatile uint8_t webSdActive = 0;    // сколько обработчиков портала сейчас работают с SD
volatile bool sdBusy     = false;   // идёт форматирование — sdTask к карте не лезет
volatile bool ledStopped = false;   // светодиод выключен перед сном/перезагрузкой
volatile uint32_t lastCanFrameMs = 0;   // время последнего принятого кадра CAN

// ---------- Статистика и журнал ошибок ----------
// Раз в STATS_PERIOD_MS в Serial выводится строка [STAT]: поток кадров,
// скорость записи/сжатия, максимальное заполнение буферов и потери кадров.
// Ошибки и важные события пишутся в /errors.log в корне SD-карты.
#define STATS_PERIOD_MS      60000
#define ERRLOG_PATH          "/errors.log"
#define ERRLOG_OLD_PATH      "/errors.old.log"
#define ERRLOG_MAX_BYTES     (256UL * 1024UL)   // потом — в errors.old.log
volatile uint32_t canFramesTotal  = 0;   // принято кадров с момента запуска
volatile uint32_t canDroppedTotal = 0;   // потеряно: очередь canQueue была полна
volatile uint32_t canQueueMax     = 0;   // макс. заполнение очереди за период
volatile uint32_t logInTotal      = 0;   // байт текста лога (до сжатия)
volatile uint32_t logOutTotal     = 0;   // байт на карте (после сжатия)
volatile uint32_t lzSbMax         = 0;   // макс. заполнение буфера кодера за период
#define RGB_LED_GAP_MS      150   // пауза между вспышкой состояния и белой

// ---------- Очередь кадров CAN ----------
#define CAN_QUEUE_LEN  1024

// ---------- Ротация лога ----------
// Новый файл открывается при каждом старте и при достижении LOG_MAX_BYTES:
//   /YYYY-MM-DD/can_log_0001.txt, can_log_0002.txt, ...
// На I-CAN это ~1 МБ в минуту, 4 МБ — около 4 минут записи на файл.
// При смене даты (поездка через полночь) следующий файл уходит в новую папку.
#define LOG_MAX_BYTES   (4UL * 1024UL * 1024UL)

// ---------- Сжатие лога ----------
// 2 — LZMA (по умолчанию): can_log_NNNN.txt.lzma, сжатие ~8 раз. Открывается
//     7-Zip / FAR (ArcLite) / xz / Python (lzma). Кодер — LZMA SDK Игоря
//     Павлова (public domain), файлы в папке src/lzma скетча. Памяти ~1 МБ
//     (PSRAM), по нагрузке на процессор — как gzip ниже.
// 1 — gzip: can_log_NNNN.txt.gz, сжатие ~3.5 раза, памяти ~130 КБ.
// 0 — без сжатия: can_log_NNNN.txt.
// LOG_MAX_BYTES считается по размеру файла НА КАРТЕ (сжатому).
#define LOG_COMPRESS     2
// LZMA: словарь. 64 КБ — оптимум для CAN-логов (больше почти не даёт),
// 32 КБ — ~7.7 раза и чуть меньше памяти
#define LOG_LZMA_DICT    (64UL * 1024UL)
// gzip (LOG_COMPRESS 1): глубина поиска повторов
#define LOG_GZ_DEPTH     4
// Раз в столько мс данные "проталкиваются" на карту (sync flush): при сбое
// питания теряется не больше этого интервала, файл распаковывается до обрыва
#define LOG_GZ_SYNC_MS   1000

// ---------- Частота SPI для SD ----------
// 20 МГц — нормально для коротких проводов на макетке; не заведётся —
// автоматически будет 4 МГц (как было раньше)
// Частота SPI карты. Лог — единицы КБ/с, 4 МГц хватает с запасом; 20 МГц на
// проводах давали сбои записи (LZMA код 9). Поднимайте (10/20 МГц) только при
// коротком надёжном монтаже — это ускорит скачивание, но не запись.
#define SD_SPI_FREQ_FAST    4000000   // запись лога
#define SD_SPI_FREQ_PORTAL 20000000   // пока работает портал (запись остановлена): быстрое скачивание
#define SD_FREQ_LOG    SD_SPI_FREQ_FAST
#define SD_FREQ_PORTAL SD_SPI_FREQ_PORTAL
#define SD_SPI_FREQ_SAFE    4000000

// ---------- Режим CAN ----------
// 1 = машина (LISTEN_ONLY), 0 = стенд (NORMAL). См. шапку.
#define CAN_LISTEN_ONLY  1

// ---------- Пины CAN ----------
// TX -> CTX трансивера, RX <- CRX трансивера (без перекрёста, см. шапку)
#define TWAI_TX_PIN   GPIO_NUM_4   // -> CTX (D / TXD)
#define TWAI_RX_PIN   GPIO_NUM_5   // <- CRX (R / RXD)

// ---------- Скорость CAN ----------
// Выбирается на портале и хранится в NVS. Если в NVS ничего нет (или там
// значение не из таблицы) — используется CAN_BITRATE_DEFAULT.
#define CAN_BITRATE_DEFAULT   500000UL   // I-CAN MLB-Evo

// ---------- Пины SD (SPI) ----------
#define SD_CS_PIN    10
#define SD_MOSI_PIN  11
#define SD_SCK_PIN   12
#define SD_MISO_PIN  13
// ---------- Пины часов (I2C) ----------
#define I2C_SDA  8
#define I2C_SCL  9

// ---------- Контроль ACC (зажигание) через оптрон ----------
// GPIO7 — свободен, входит в RTC-домен S3 (обязательно для ext0-пробуждения
// из глубокого сна). Ожидаемая полярность: оптрон открыт (ACC есть,
// зажигание включено) -> транзистор оптрона тянет пин к GND -> LOW.
// ACC отсутствует (зажигание выключено) -> внутренний pull-up держит HIGH.
// Если у вашей схемы оптрона логика обратная — поменяйте местами
// ACC_ON_LEVEL/ACC_OFF_LEVEL ниже, остальной код трогать не придётся.
#define ACC_PIN         GPIO_NUM_7
#define ACC_ON_LEVEL       LOW
#define ACC_OFF_LEVEL      HIGH

// ---------- Не спать, пока подключён USB-хост (комп) ----------
// 1 — при подключённом компе сон блокируется (стенд/отладка)
// 0 — сон строго по ACC, USB игнорируется (например, если в машине
//     питание идёт от USB-порта магнитолы, который тоже является хостом)
#define USB_HOST_KEEPS_AWAKE   1

// Сколько ждать энумерации USB после пробуждения/старта, мс
#define USB_ENUM_WAIT_MS       800

// Сколько USB-хост должен отсутствовать НЕПРЕРЫВНО, прежде чем уснуть, мс.
// Защита от кратких провалов SOF (USB selective suspend на компе,
// сон/блокировка компа, помехи) — без неё плата засыпает от любого сбоя.
#define USB_LOST_GRACE_MS      5000

// ---------- Не спать, пока пользуются веб-порталом ----------
// Сон блокируется, если к AP подключён клиент И был HTTP-запрос к порталу
// не позже WEB_HOLD_MS назад. Просто подключённый телефон (автоподключение
// к сохранённой сети в машине) сон НЕ держит — нужны запросы к страницам,
// иначе телефон в кармане разряжал бы аккумулятор на стоянке.
#define WEB_HOLD_MS            (5UL * 60UL * 1000UL)   // 5 минут

// Пока портал в работе, запись лога на SD приостанавливается (файл закрывается
// и целиком доступен для скачивания; карта занята только порталом). Как только
// запросы к порталу прекращаются на LOG_PORTAL_HOLD_MS (или клиент ушёл с точки
// доступа), запись продолжается новым файлом (маркер CONTINUED). 0 — не останавливать.
#define LOG_PAUSE_WHILE_PORTAL 1
#define LOG_PORTAL_HOLD_MS     (60UL * 1000UL)   // 60 с без запросов — портал свободен

#if USB_HOST_KEEPS_AWAKE
  #if !ARDUINO_USB_CDC_ON_BOOT || !ARDUINO_USB_MODE
    #error "Для USB_HOST_KEEPS_AWAKE нужны: USB Mode = Hardware CDC and JTAG, USB CDC On Boot = Enabled"
  #endif
#endif

// ---------- WiFi точка доступа (веб-портал) ----------
const char* AP_SSID     = "S3-CAN-Sniffer-Setup";
// AP_PASSWORD (пароль точки доступа) — в secrets.h
// Канал точки доступа:
//   0    — выбрать автоматически при старте: сканирование эфира (~2–3 с) и
//          выбор самого тихого из неперекрывающихся 1 / 6 / 11 с учётом
//          уровня сигнала соседних сетей и перекрытия каналов;
//   1–13 — фиксированный канал.
#define AP_CHANNEL      0
// Итог автовыбора (для портала и Serial)
int   apChannelUsed = 1;
float apChanNoiseDbm[3] = {-100, -100, -100};   // суммарная помеха, дБм-экв.
int   apChanNets[3] = {0, 0, 0};                 // сетей, задевающих канал
const int AP_CANDIDATES[3] = {1, 6, 11};
// 1 — скрытая сеть: SSID не транслируется, в списке сетей её не видно,
//     подключаться вручную ("Добавить сеть", имя и пароль вводятся руками).
//     Телефон, который уже запомнил сеть, подключится и к скрытой.
// 0 — обычная видимая сеть (по умолчанию)
#define WIFI_AP_HIDDEN  0
// Мощность передатчика WiFi. Меньше мощность — меньше броски тока при
// передаче (по умолчанию до ~19.5 дБм, пики сотни мА) и меньше просадки
// питания; цена — дальность и скорость портала. Варианты из WiFi.h:
// WIFI_POWER_19_5dBm, _17dBm, _15dBm, _13dBm, _11dBm, _8_5dBm, _7dBm, _5dBm, _2dBm
#define WIFI_TX_POWER   WIFI_POWER_8_5dBm
// Сколько минут после включения зажигания работают точка доступа WiFi и
// портал (и OTA через espota). Потом WiFi выключается до следующего
// включения зажигания — меньше нагрев и потребление, эфир чище для BLE.
// Пока портал реально используется (были запросы за последние WEB_HOLD_MS),
// WiFi не выключается. 0 — WiFi работает всё время, как раньше.
#define WIFI_ACTIVE_MINUTES   5

// ---------- BLE синхронизация времени ----------
#define SYNC_SERVICE_UUID   "A1B2C3D4-0001-41A2-9E3B-000000000001"
#define SYNC_CHAR_UUID      "A1B2C3D4-0001-41A2-9E3B-000000000002"

// ---------- BLE выбор наблюдаемого ID + живые сырые данные (для LCD) ----------
// Раньше тут была жёстко зашитая "скорость" по одному ID — теперь
// универсальный механизм: LCD-экран сам выбирает, какой ID сейчас
// смотреть (через WATCH_SELECT_CHAR_UUID, запись 4 байт = uint32 ID),
// а сниффер просто пересылает СЫРЫЕ байты любого кадра, совпавшего
// с текущим выбором, через WATCH_DATA_CHAR_UUID. Расшифровка/парсинг —
// уже на стороне LCD, где человек визуально сверяет с реальным событием.
#define WATCH_SELECT_CHAR_UUID   "A1B2C3D4-0001-41A2-9E3B-000000000004"
#define WATCH_DATA_CHAR_UUID     "A1B2C3D4-0001-41A2-9E3B-000000000005"

volatile uint32_t watchedCanId = 0; // 0 = ничего не наблюдаем

// =====================================================================
// Поток кадров по BLE с фильтром в стиле ACL (для HUD и отладки).
// Клиент подписывается на FRAMES (…0008) и пишет список правил в ACL
// (…0007); сниффер шлёт совпавшие кадры пачками. Протокол — в
// docs/BLE_ACL_protocol_ru.md. Список правил живёт до отключения клиента,
// который его записал: после переподключения его нужно прислать заново.
// =====================================================================
#define ACL_CHAR_UUID     "A1B2C3D4-0001-41A2-9E3B-000000000007"
#define FRAMES_CHAR_UUID  "A1B2C3D4-0001-41A2-9E3B-000000000008"
#define ACL_VERSION       0x01
#define ACL_MAX_RULES     32
#define ACL_BATCH_MS      20      // макс. задержка пачки, мс
#define ACL_QUEUE_LEN     256     // кадров в очереди на отправку по BLE
#define ACL_STATE_SLOTS   256     // память "по изменению / интервал" на ID

// Флаги правила (байт 0)
#define ACL_PERMIT    0x01        // 1 — разрешить, 0 — запретить
#define ACL_EXT       0x02        // правило для 29-битных ID (иначе 11-бит)
#define ACL_ONCHANGE  0x04        // слать, только если данные изменились
#define ACL_ANYFMT    0x08        // формат ID не важен (11 и 29 бит)

struct __attribute__((packed)) AclRule {   // 12 байт, little-endian
  uint8_t  flags;
  uint8_t  reserved;
  uint16_t minIntervalMs;   // 0 — без ограничения
  uint32_t id;
  uint32_t mask;            // 1 — бит должен совпасть; 0 — любой
};
struct AclSet { uint8_t n; AclRule r[ACL_MAX_RULES]; };
static AclSet aclSets[2];                    // двойной буфер: пишем в свободный,
static AclSet* volatile aclCur = &aclSets[0];// потом одним присваиванием переключаем

struct __attribute__((packed)) AclFrame { uint16_t ts; uint32_t idf; uint8_t dlc; uint8_t data[8]; };
struct AclState { uint32_t key; uint32_t lastMs; uint8_t dlc; uint8_t data[8]; };
static AclState aclState[ACL_STATE_SLOTS];

QueueHandle_t bleQueue = nullptr;
NimBLECharacteristic* aclCharacteristic = nullptr;
NimBLECharacteristic* framesCharacteristic = nullptr;
volatile bool     framesSubscribed = false;
volatile uint16_t aclOwnerConn = 0xFFFF;     // кто записал список
volatile uint16_t framesMtu = 23;
volatile bool     bleDropFlag = false;
volatile uint32_t bleSentTotal = 0, bleDroppedTotal = 0;

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

// Кадр CAN в очереди canTask -> sdTask. Определён ДО первой функции:
// Arduino IDE вставляет автопрототипы функций перед первой функцией файла,
// и тип из прототипа (canTimeFrame, logWriteFrame) должен быть уже известен.
struct CanLogEntry {
  uint32_t timestamp;
  uint32_t id;
  uint8_t  dlc;
  uint8_t  flags;     // бит0 = 29-bit (extended), бит1 = remote (RTR)
  uint8_t  data[8];
};
#define LOGF_EXTD  0x01
#define LOGF_RTR   0x02

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

// Пакет с сырыми данными наблюдаемого кадра
struct __attribute__((packed)) WatchedFramePacket {
  uint32_t id;
  uint8_t  dlc;
  uint8_t  data[8];
};

// Пакет синхронизации: millis() платы + unix-эпоха с DS3231.
// packed — чтобы гарантировать одинаковую раскладку байт при передаче
// между двумя независимыми платами (не полагаемся на выравнивание
// компилятора по умолчанию).
struct __attribute__((packed)) SyncPacket {
  uint32_t millisValue;
  uint32_t unixEpoch;
};

NimBLECharacteristic* syncCharacteristic;
NimBLECharacteristic* watchSelectCharacteristic;
NimBLECharacteristic* watchDataCharacteristic;
File canLogFile;
RTC_DS3231  rtcDs;    // 0x68
RTC_PCF8563 rtcPcf;   // 0x51
WebServer webServer(80);
String currentDateFolder;
String   currentLogName;          // имя текущего файла без папки

// Папка, в которой sdTask сейчас ведёт запись — копия для веб-потока
// (удалять её нельзя). Фиксированный буфер под спинлоком: String между
// задачами без блокировки делить нельзя.
char activeLogFolder[24] = "";
portMUX_TYPE activeLogMux = portMUX_INITIALIZER_UNLOCKED;
void setActiveLogFolder(const char* f) {
  portENTER_CRITICAL(&activeLogMux);
  strncpy(activeLogFolder, f, sizeof(activeLogFolder) - 1);
  activeLogFolder[sizeof(activeLogFolder) - 1] = 0;
  portEXIT_CRITICAL(&activeLogMux);
}
String getActiveLogFolder() {
  char tmp[24];
  portENTER_CRITICAL(&activeLogMux);
  memcpy(tmp, activeLogFolder, sizeof(tmp));
  portEXIT_CRITICAL(&activeLogMux);
  return String(tmp);
}
// Полный путь файла, который сейчас открыт на запись ("" — ни одного).
// Нужен порталу: такой файл не отдаётся недописанным.
char activeLogPath[48] = "";
void setActiveLogPath(const char* p) {
  portENTER_CRITICAL(&activeLogMux);
  strncpy(activeLogPath, p, sizeof(activeLogPath) - 1);
  activeLogPath[sizeof(activeLogPath) - 1] = 0;
  portEXIT_CRITICAL(&activeLogMux);
}
String getActiveLogPath() {
  char tmp[48];
  portENTER_CRITICAL(&activeLogMux);
  memcpy(tmp, activeLogPath, sizeof(tmp));
  portEXIT_CRITICAL(&activeLogMux);
  return String(tmp);
}
uint32_t currentLogBytes = 0;     // сколько записано в текущий файл

// Структура одного кадра для передачи между задачами через очередь

QueueHandle_t canQueue;
SemaphoreHandle_t shutdownDoneSemaphore;

// Служебный "poison pill" — специальный ID, которым основной loop()
// сигнализирует sdTask, что пора закрыть файл перед сном. Реальные CAN ID
// укладываются в 11/29 бит, это значение физически недостижимо на шине.
#define SHUTDOWN_SENTINEL_ID  0xFFFFFFFF
// Второй служебный ID: портал просит закрыть текущий файл (перед скачиванием),
// запись продолжится в следующий файл с первого же кадра
#define ROTATE_SENTINEL_ID    0xFFFFFFFE
SemaphoreHandle_t rotateDoneSemaphore;
// Причина закрытия лога — передаётся в поле dlc служебного кадра
#define CLOSE_REASON_SLEEP  0
#define CLOSE_REASON_OTA    1
#define CLOSE_REASON_CFG    2

// Прототипы функций, определённых ниже по файлу
inline void touchWeb();
void closeLogForRestart(uint8_t reason);
const char* resetReasonStr(esp_reset_reason_t r);


// =====================================================================
// DS3231
// =====================================================================
// ---------------------------------------------------------------------
// Время: DS3231 читается ОДИН раз при старте и переносится в системные
// часы ESP32 (settimeofday). Дальше все — веб, BLE-синхронизация камеры,
// ротация лога — берут время из системных часов, без обращений к I2C.
// Раньше rtc.now() дёргали из трёх задач сразу (веб, BLE, sdTask), и
// перезапуск посреди I2C-транзакции оставлял DS3231 с зажатой SDA —
// после этого "DS3231 не найден" до отключения питания.
// DS3231 хранит местное время; системные часы держим в "UTC" с тем же
// значением — так же, как было (unixtime() от местного времени).
// ---------------------------------------------------------------------
bool timeValid = false;          // системные часы достоверны
const char* timeSource = "—";     // откуда взято время (для портала/Serial)

enum RtcKind { RTC_NONE, RTC_KIND_DS3231, RTC_KIND_PCF8563 };
RtcKind rtcKind = RTC_NONE;
bool rtcOscStopped = false;       // DS3231: OSF / PCF8563: VL — часы останавливались

// Переживает deep sleep: время было достоверным перед сном. Системные
// часы ESP32 во сне идут (RTC-таймер), поэтому после пробуждения их можно
// считать достоверными даже без внешних часов (уход — секунды за ночь,
// а режим CAN_TIME_SYNC 2 подведёт их по машине).
RTC_DATA_ATTR bool rtcTimeValidBeforeSleep = false;

// Отложенная запись во внешние часы: время из CAN приходит в sdTask,
// а шиной I2C владеет только loop() — там и пишем
volatile bool     rtcWritePending = false;
volatile uint32_t rtcWriteValue   = 0;

DateTime nowTime() {
  return DateTime((uint32_t)time(nullptr));
}

void setSystemTime(uint32_t unixLocal) {
  struct timeval tv = { (time_t)unixLocal, 0 };
  settimeofday(&tv, nullptr);
}

// ---------------------------------------------------------------------
// Журнал ошибок /errors.log. Пишется из любой задачи (под мьютексом).
// До монтирования SD строки копятся в памяти и сбрасываются потом.
// Формат: "2026-09-28 21:05:13 [fw 2.2.0, 123456 мс] текст"
// ---------------------------------------------------------------------
static SemaphoreHandle_t errMutex = nullptr;
static String errPending[12];
static int    errPendingN = 0;

static void errWriteLine(const String& line) {
  File f = LOGFS.open(ERRLOG_PATH, FILE_APPEND);
  if (!f) return;
  f.print(line);
  size_t sz = f.size();
  f.close();
  if (sz > ERRLOG_MAX_BYTES) {             // ротация: одна старая копия
    LOGFS.remove(ERRLOG_OLD_PATH);
    LOGFS.rename(ERRLOG_PATH, ERRLOG_OLD_PATH);
  }
}

void errLog(const char* fmt, ...) {
  char msg[200];
  va_list ap; va_start(ap, fmt); vsnprintf(msg, sizeof(msg), fmt, ap); va_end(ap);
  char ts[48] = "время неизвестно";   // UTF-8: кириллица по 2 байта
  if (timeValid) {
    DateTime n((uint32_t)time(nullptr));
    snprintf(ts, sizeof(ts), "%04d-%02d-%02d %02d:%02d:%02d", n.year(), n.month(), n.day(),
             n.hour(), n.minute(), n.second());
  }
  String line = String(ts) + " [fw " FW_VERSION ", " + String(millis()) + " мс] " + msg + "\r\n";
  Serial.print("[ERR] " + line);
  if (!errMutex) errMutex = xSemaphoreCreateMutex();
  if (xSemaphoreTake(errMutex, pdMS_TO_TICKS(1000)) != pdTRUE) return;
  if (!sdMounted) {
    if (errPendingN < 12) errPending[errPendingN++] = line;
  } else {
    errWriteLine(line);
  }
  xSemaphoreGive(errMutex);
}

// Сбросить накопленное до монтирования SD
void errFlushPending() {
  if (!errMutex) errMutex = xSemaphoreCreateMutex();
  xSemaphoreTake(errMutex, portMAX_DELAY);
  for (int i = 0; i < errPendingN; i++) errWriteLine(errPending[i]);
  errPendingN = 0;
  xSemaphoreGive(errMutex);
}

// Освобождение шины I2C: если ESP32 перезагрузился посреди транзакции,
// DS3231 может держать SDA в нуле, ожидая недополученные такты. 9 тактов
// SCL + STOP возвращают его в исходное состояние без снятия питания.
void i2cBusRecover() {
  pinMode(I2C_SDA, INPUT_PULLUP);
  pinMode(I2C_SCL, INPUT_PULLUP);
  delayMicroseconds(50);
  if (digitalRead(I2C_SDA) == HIGH) return;   // шина свободна

  Serial.println("I2C: SDA зажата — освобождаю шину");
  pinMode(I2C_SCL, OUTPUT_OPEN_DRAIN);
  for (int i = 0; i < 9 && digitalRead(I2C_SDA) == LOW; i++) {
    digitalWrite(I2C_SCL, LOW);  delayMicroseconds(10);
    digitalWrite(I2C_SCL, HIGH); delayMicroseconds(10);
  }
  // STOP: SDA 0 -> 1 при SCL = 1
  pinMode(I2C_SDA, OUTPUT_OPEN_DRAIN);
  digitalWrite(I2C_SDA, LOW);  delayMicroseconds(10);
  digitalWrite(I2C_SCL, HIGH); delayMicroseconds(10);
  digitalWrite(I2C_SDA, HIGH); delayMicroseconds(10);
  pinMode(I2C_SDA, INPUT_PULLUP);
  pinMode(I2C_SCL, INPUT_PULLUP);
  Serial.printf("I2C: после восстановления SDA=%d\n", digitalRead(I2C_SDA));
}

bool plausible(const DateTime& t) {
  return t.year() >= TIME_MIN_YEAR && t.year() <= TIME_MAX_YEAR && t.month() >= 1 && t.month() <= 12 &&
         t.day() >= 1 && t.day() <= 31 && t.hour() <= 23 && t.minute() <= 59 && t.second() <= 59;
}

// Прямое чтение регистра по I2C (для диагностики). -1 — ошибка.
int i2cReadReg(uint8_t addr, uint8_t reg) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return -1;
  if (Wire.requestFrom(addr, (uint8_t)1) != 1) return -1;
  return Wire.read();
}

bool i2cPresent(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

DateTime rtcRead() {
  return rtcKind == RTC_KIND_DS3231 ? rtcDs.now() : rtcPcf.now();
}

void rtcWrite(const DateTime& t) {
  if (rtcKind == RTC_KIND_DS3231) {
    rtcDs.adjust(t);            // сбрасывает OSF
  } else if (rtcKind == RTC_KIND_PCF8563) {
    rtcPcf.adjust(t);           // сбрасывает VL
    rtcPcf.start();             // у PCF8563 генератор после записи запускаем явно
  }
  rtcOscStopped = false;
}

const char* rtcName() {
  return rtcKind == RTC_KIND_DS3231 ? "DS3231" : rtcKind == RTC_KIND_PCF8563 ? "PCF8563" : "нет";
}

// Единая точка установки времени (портал, CAN)
void applyTime(uint32_t unixLocal, const char* source, bool writeRtc) {
  setSystemTime(unixLocal);
  timeValid = true;
  timeSource = source;
  if (writeRtc && rtcKind != RTC_NONE) {
    rtcWriteValue = unixLocal;
    rtcWritePending = true;     // запишет loop()
  }
}

// Вызывается из loop(): единственный, кто пишет во внешние часы после старта
void rtcServiceLoop() {
  if (!rtcWritePending) return;
  rtcWritePending = false;
  rtcWrite(DateTime(rtcWriteValue));
  Serial.printf("Часы %s подведены\n", rtcName());
}

void setupRTC() {
  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();

  // Внешние часы: ищем DS3231 (0x68) и PCF8563 (0x51). Несколько попыток
  // с переподнятием шины — после сбоя модуль может держать SDA.
  for (int attempt = 1; attempt <= 3 && rtcKind == RTC_NONE; attempt++) {
    if (attempt > 1) { Wire.end(); delay(20); }
    i2cBusRecover();
    Wire.begin(I2C_SDA, I2C_SCL);
    Wire.setTimeOut(50);
    if (i2cPresent(0x68) && rtcDs.begin())       rtcKind = RTC_KIND_DS3231;
    else if (i2cPresent(0x51) && rtcPcf.begin()) rtcKind = RTC_KIND_PCF8563;
  }
  Serial.printf("Внешние часы: %s\n", rtcName());

  if (rtcKind != RTC_NONE) {
    int st = -1;
    if (rtcKind == RTC_KIND_DS3231) {
      for (int i = 0; i < 3 && st < 0; i++) { st = i2cReadReg(0x68, 0x0F); if (st < 0) delay(20); }
      rtcOscStopped = (st >= 0) && (st & 0x80);            // OSF
    } else {
      rtcPcf.writeSqwPinMode(PCF8563_SquareWaveOFF);        // CLKOUT выкл — лишний ток
      for (int i = 0; i < 3 && st < 0; i++) { st = i2cReadReg(0x51, 0x02); if (st < 0) delay(20); }
      rtcOscStopped = (st >= 0) && (st & 0x80);            // VL
    }

    // Два чтения подряд должны совпасть (±1 с) и быть правдоподобными
    DateTime now, a, b;
    bool ok = false;
    for (int i = 0; i < 3 && !ok; i++) {
      a = rtcRead(); delay(5); b = rtcRead();
      ok = plausible(a) && plausible(b) && (b.unixtime() - a.unixtime() <= 1);
      now = b;
    }
    if (rtcOscStopped) errLog("%s: часы останавливались (OSF/VL) — время под вопросом", rtcName());
    Serial.printf("%s: статус=0x%02X (стоп=%d), %04d-%02d-%02d %02d:%02d:%02d -> %s\n",
                  rtcName(), st < 0 ? 0xFF : st, rtcOscStopped ? 1 : 0,
                  now.year(), now.month(), now.day(), now.hour(), now.minute(), now.second(),
                  ok ? "OK" : "НЕКОРРЕКТНО");
    if (ok) {
      applyTime(now.unixtime(), rtcName(), false);
      return;
    }
  }

  // Без внешних часов (или они врут): после пробуждения из сна системные
  // часы ESP32 продолжают идти — берём их
  if (cause != ESP_SLEEP_WAKEUP_UNDEFINED && rtcTimeValidBeforeSleep && plausible(nowTime())) {
    timeValid = true;
    timeSource = "ESP32 (после сна)";
    DateTime n = nowTime();
    Serial.printf("Время из системных часов после сна: %04d-%02d-%02d %02d:%02d:%02d\n",
                  n.year(), n.month(), n.day(), n.hour(), n.minute(), n.second());
    return;
  }

  Serial.println(CAN_TIME_SYNC ? "Время пока не известно — жду кадр 0x6B2 с шины или установку на портале"
                               : "Время не известно — установите его на портале. До этого лог пойдёт в /no-rtc");
}

// ---------- Время из CAN: кадр 0x6B2 (Diagnose_01) ----------
static inline uint32_t canSig(const uint8_t* d, int start, int len) {
  uint64_t raw = 0;
  for (int i = 7; i >= 0; i--) raw = (raw << 8) | d[i];
  return (uint32_t)((raw >> start) & ((1ULL << len) - 1));
}

// Вызывается из sdTask для каждого кадра. true — время только что выставлено.
bool canTimeFrame(const CanLogEntry& e) {
#if CAN_TIME_SYNC
  if (e.id != CAN_TIME_ID || (e.flags & (LOGF_EXTD | LOGF_RTR)) || e.dlc < 8) return false;
  if (CAN_TIME_SYNC == 1 && timeValid) return false;

  int Y = 2000 + canSig(e.data, 28, 7), M = canSig(e.data, 35, 4), D = canSig(e.data, 39, 5);
  int h = canSig(e.data, 44, 5), m = canSig(e.data, 49, 6), sec = canSig(e.data, 55, 6);
  // Вне допустимого интервала или несуществующая дата (31 апреля, 30
  // февраля) — не принимаем и сбрасываем серию подтверждений
  static const uint8_t mdays[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
  static uint32_t candT = 0, candMs = 0;
  static int      candCount = 0;
  bool leap = (Y % 4 == 0 && (Y % 100 != 0 || Y % 400 == 0));
  if (Y < TIME_MIN_YEAR || Y > TIME_MAX_YEAR || M < 1 || M > 12 || D < 1 ||
      D > mdays[M - 1] + ((M == 2 && leap) ? 1 : 0) || h > 23 || m > 59 || sec > 59) {
    candCount = 0;
    return false;
  }

  // Время кадра на шкале сниффера (момент приёма, e.timestamp)
  uint32_t tFrame = DateTime(Y, M, D, h, m, sec).unixtime() + (int32_t)(CAN_TIME_TZ_HOURS * 3600);

  // Серия подтверждений: кадр должен продолжать предыдущий
  if (candCount > 0) {
    int32_t expected = (int32_t)candT + (int32_t)((e.timestamp - candMs) / 1000);
    if (abs((int32_t)tFrame - expected) <= 2) candCount++;
    else candCount = 1;              // выброс или скачок — начинаем серию заново
  } else {
    candCount = 1;
  }
  candT = tFrame;
  candMs = e.timestamp;
  if (candCount < CAN_TIME_CONFIRM_FRAMES) return false;

  // Поправка на момент обработки: кадр ждал в очереди
  uint32_t age = millis() - e.timestamp;
  uint32_t t = tFrame + age / 1000;

  // Раньше сборки прошивки — заведомо сбитые часы машины
  static const uint32_t buildTime = DateTime(__DATE__, __TIME__).unixtime();
  if (t < buildTime) {
    static bool warned = false;
    if (!warned) {
      errLog("Время из CAN (%04d-%02d-%02d) раньше сборки прошивки — часы машины сбиты, игнорирую", Y, M, D);
      warned = true;
    }
    return false;
  }

  if (timeValid) {
    int32_t diff = (int32_t)(t - (uint32_t)time(nullptr));
    if (abs(diff) <= CAN_TIME_MAX_DIFF_S) return false;
#if CAN_TIME_MAX_JUMP_S > 0
    if (abs(diff) > CAN_TIME_MAX_JUMP_S) {
      static bool warned = false;
      if (!warned) {
        errLog("Время из CAN расходится на %ld с (> CAN_TIME_MAX_JUMP_S) — часы машины сбиты? Не трогаю", (long)diff);
        warned = true;
      }
      return false;
    }
#endif
    Serial.printf("Время из CAN расходится на %ld с — подвожу часы\n", (long)diff);
  }
  applyTime(t, "CAN 0x6B2", true);
  DateTime n(t);
  Serial.printf("Время из CAN: %04d-%02d-%02d %02d:%02d:%02d\n",
                n.year(), n.month(), n.day(), n.hour(), n.minute(), n.second());
  return true;
#else
  (void)e;
  return false;
#endif
}

// Папка по дате; без достоверного времени — /no-rtc (а не мусорная дата
// вроде /2130-33-44 или 2000-01-01)
String dateFolderName(DateTime dt) {
  if (!timeValid || !plausible(dt)) return String("/no-rtc");
  char buf[16];
  snprintf(buf, sizeof(buf), "/%04d-%02d-%02d", dt.year(), dt.month(), dt.day());
  return String(buf);
}

// =====================================================================
// Веб-сервер для установки даты/времени
// =====================================================================

// Общая "тёмная" тема оформления, единая для всех страниц —
// чёрный фон, моноширинный шрифт, светлый текст, акцент зелёным
// =====================================================================
// Язык веб-портала (RU/EN). Хранится в NVS (namespace "ui", ключ "lang").
// Нет значения в NVS — русский. Красная кнопка вверху каждой страницы
// показывает ПРОТИВОПОЛОЖНЫЙ язык; переключение мгновенное, без перезагрузки.
// Serial-вывод остаётся на русском.
// =====================================================================
bool uiEn = false;                          // false = RU, true = EN
#define TR(ru, en)  String(uiEn ? (en) : (ru))

void loadUiLanguage() {
  Preferences prefs;
  String lang = "ru";
  if (prefs.begin("ui", true)) {
    lang = prefs.getString("lang", "ru");
    prefs.end();
  }
  uiEn = (lang == "en");                    // всё, что не "en", — русский
}

void saveUiLanguage(bool en) {
  uiEn = en;
  Preferences prefs;
  if (prefs.begin("ui", false)) {
    prefs.putString("lang", en ? "en" : "ru");
    prefs.end();
  }
  Serial.printf("Язык портала: %s\n", en ? "English" : "русский");
}

const char* PAGE_STYLE =
  "<style>"
  "body{background:#0d0d0d;color:#d8d8d8;font-family:'Consolas','Courier New',monospace;"
  "padding:16px;max-width:720px;margin:0 auto;}"
  "h2,h3{color:#4caf50;}"
  "a{color:#4caf50;}"
  "input,button,select{background:#1a1a1a;color:#d8d8d8;border:1px solid #4caf50;"
  "padding:6px 10px;font-family:inherit;border-radius:4px;max-width:100%;box-sizing:border-box;}"

  "button{cursor:pointer;}"
  "button:hover{background:#4caf50;color:#0d0d0d;}"
  ".langbar{text-align:right;margin:0 0 8px 0;}"
  "button.lang{background:#c62828;border-color:#c62828;color:#fff;font-weight:bold;}"
  "button.lang:hover{background:#e53935;border-color:#e53935;color:#fff;}"
  "ul{list-style:none;padding-left:0;}"
  // Сворачиваемые папки на странице логов: "+" / "−" вместо стрелки
  ".fold{display:flex;align-items:flex-start;gap:8px;margin:6px 0;}"
  ".fold input{margin-top:5px;}"
  "details{flex:1;}"
  "summary{cursor:pointer;list-style:none;font-weight:bold;}"
  "summary::-webkit-details-marker{display:none;}"
  "summary::before{content:'+';display:inline-block;width:1.2em;color:#4caf50;font-weight:bold;}"
  "details[open] summary::before{content:'\\2212';}"
  "details ul{margin:6px 0 10px 1.2em;}"
  ".meta{font-weight:normal;color:#9a9a9a;}"
  "li{padding:2px 0;border-bottom:1px solid #222;}"
  "hr{border-color:#333;}"
  "</style>";

// =====================================================================
// Скорость CAN: таблица поддерживаемых значений и хранение в NVS
// =====================================================================
struct CanBitrate {
  uint32_t    bps;
  const char* label_ru;
  const char* label_en;
  const char* hint_ru;   // где обычно встречается (подсказка на портале)
  const char* hint_en;
};

static const CanBitrate CAN_BITRATES[] = {
  { 1000000, "1000 кбит/с", "1000 kbit/s", "", "" },
  {  800000, "800 кбит/с",  "800 kbit/s",  "", "" },
  {  500000, "500 кбит/с",  "500 kbit/s",
    "VAG MLB/MQB, большинство HS-CAN, OBD", "VAG MLB/MQB, most HS-CAN, OBD" },
  {  250000, "250 кбит/с",  "250 kbit/s",
    "J1939 (грузовики), некоторые HS-CAN", "J1939 (trucks), some HS-CAN" },
  {  125000, "125 кбит/с",  "125 kbit/s",
    "Ford MS-CAN, Mazda, Opel MS-CAN", "Ford MS-CAN, Mazda, Opel MS-CAN" },
  {  100000, "100 кбит/с",  "100 kbit/s",
    "VAG Comfort/Infotainment (PQ, старые)", "VAG Comfort/Infotainment (PQ, older)" },
  {   83333, "83.3 кбит/с", "83.3 kbit/s",
    "Mercedes B-CAN, Chrysler (нестандартная)", "Mercedes B-CAN, Chrysler (non-standard)" },
  {   50000, "50 кбит/с",   "50 kbit/s",   "", "" },
  {   33333, "33.3 кбит/с", "33.3 kbit/s",
    "GMLAN Single-Wire (нужен SW-трансивер!)", "GMLAN Single-Wire (needs SW transceiver!)" },
  {   25000, "25 кбит/с",   "25 kbit/s",   "", "" },
  {   20000, "20 кбит/с",   "20 kbit/s",   "", "" },
};
static const size_t CAN_BITRATES_N = sizeof(CAN_BITRATES) / sizeof(CAN_BITRATES[0]);

uint32_t canBitrate = CAN_BITRATE_DEFAULT;   // актуальная скорость (bps)

bool canBitrateSupported(uint32_t bps) {
  for (size_t i = 0; i < CAN_BITRATES_N; i++) if (CAN_BITRATES[i].bps == bps) return true;
  return false;
}

// Подпись скорости на языке портала
const char* canBitrateLabel(uint32_t bps) {
  for (size_t i = 0; i < CAN_BITRATES_N; i++)
    if (CAN_BITRATES[i].bps == bps) return uiEn ? CAN_BITRATES[i].label_en : CAN_BITRATES[i].label_ru;
  return "?";
}

// Подпись для Serial — всегда по-русски
const char* canBitrateLabelRu(uint32_t bps) {
  for (size_t i = 0; i < CAN_BITRATES_N; i++)
    if (CAN_BITRATES[i].bps == bps) return CAN_BITRATES[i].label_ru;
  return "?";
}

// Чтение из NVS; нет ключа или мусор — значение по умолчанию
void loadCanBitrate() {
  Preferences prefs;
  uint32_t v = CAN_BITRATE_DEFAULT;
  if (prefs.begin("sniffer", true)) {           // read-only
    v = prefs.getULong("can_bps", CAN_BITRATE_DEFAULT);
    prefs.end();
  }
  if (!canBitrateSupported(v)) {
    Serial.printf("NVS: скорость CAN %lu не поддерживается — беру по умолчанию\n", (unsigned long)v);
    v = CAN_BITRATE_DEFAULT;
  }
  canBitrate = v;
}

bool saveCanBitrate(uint32_t bps) {
  Preferences prefs;
  if (!prefs.begin("sniffer", false)) return false;
  bool ok = prefs.putULong("can_bps", bps) == sizeof(uint32_t);
  prefs.end();
  return ok;
}

// Настройки битового тайминга. APB 80 МГц, 20 квантов на бит (кроме
// стандартных макросов IDF), точка выборки 80% — как в макросах IDF.
twai_timing_config_t canTimingFor(uint32_t bps) {
  switch (bps) {
    case 1000000: { twai_timing_config_t t = TWAI_TIMING_CONFIG_1MBITS(); return t; }
    case 800000: { twai_timing_config_t t = TWAI_TIMING_CONFIG_800KBITS(); return t; }
    case 250000: { twai_timing_config_t t = TWAI_TIMING_CONFIG_250KBITS(); return t; }
    case 125000: { twai_timing_config_t t = TWAI_TIMING_CONFIG_125KBITS(); return t; }
    case 100000: { twai_timing_config_t t = TWAI_TIMING_CONFIG_100KBITS(); return t; }
    case 50000: { twai_timing_config_t t = TWAI_TIMING_CONFIG_50KBITS(); return t; }
    case 25000: { twai_timing_config_t t = TWAI_TIMING_CONFIG_25KBITS(); return t; }
    case 20000: { twai_timing_config_t t = TWAI_TIMING_CONFIG_20KBITS(); return t; }
    case   83333: {
      // Нестандартная: 80 МГц / 48 = 1.667 МГц квант, 20 квантов = 83.33 кбит/с
      twai_timing_config_t t = TWAI_TIMING_CONFIG_100KBITS();
      t.quanta_resolution_hz = 0;   // 0 → драйвер берёт brp напрямую
      t.brp = 48; t.tseg_1 = 15; t.tseg_2 = 4; t.sjw = 3;
      return t;
    }
    case   33333: {
      // Нестандартная: 80 МГц / 120 = 666.7 кГц квант, 20 квантов = 33.33 кбит/с
      twai_timing_config_t t = TWAI_TIMING_CONFIG_100KBITS();
      t.quanta_resolution_hz = 0;
      t.brp = 120; t.tseg_1 = 15; t.tseg_2 = 4; t.sjw = 3;
      return t;
    }
    case  500000:
    default: { twai_timing_config_t t = TWAI_TIMING_CONFIG_500KBITS(); return t; }
  }
}

// Шапка каждой страницы + красная кнопка переключения языка.
// back — куда вернуться после переключения (текущая страница).
String htmlHead(const String& title) {
  String back = webServer.uri();
  return "<!DOCTYPE html><html lang='" + String(uiEn ? "en" : "ru") + "'><head><meta charset='utf-8'>"
         "<meta name='viewport' content='width=device-width, initial-scale=1'>"
         "<title>" + title + "</title>" + String(PAGE_STYLE) + "</head><body>"
         "<form class='langbar' method='POST' action='/set-lang'>"
         "<input type='hidden' name='back' value='" + back + "'>"
         "<button class='lang' type='submit' name='lang' value='" + String(uiEn ? "ru" : "en") + "'>" +
         String(uiEn ? "Русский" : "English") + "</button></form>";
}

// POST /set-lang: сохранить язык и вернуться на ту же страницу.
// Возвращаемся только на GET-страницы; после POST-результатов — на главную.
void handleSetLang() {
  touchWeb();
  saveUiLanguage(webServer.arg("lang") == "en");
  String back = webServer.arg("back");
  if (back != "/logs" && back != "/update" && back != "/errors") back = "/";
  webServer.sendHeader("Location", back);
  webServer.send(303);
}

// Время последнего HTTP-запроса к порталу (millis)
volatile uint32_t lastWebActivityMs = 0;
volatile bool     webActivitySeen   = false;

inline void touchWeb() {
  lastWebActivityMs = millis();
  webActivitySeen   = true;
}

// Портал сейчас "в работе": есть клиент на AP и недавняя активность
bool webPortalActive() {
  if (!webActivitySeen) return false;
  if (WiFi.softAPgetStationNum() == 0) return false;
  return (millis() - lastWebActivityMs) < WEB_HOLD_MS;
}

// Портал держит карту: есть клиент на точке доступа и запросы за последнее время
bool logPortalPause() {
#if LOG_PAUSE_WHILE_PORTAL
  if (webSdActive) return true;                 // идёт обработчик (скачивание может быть долгим)
  if (!webActivitySeen) return false;
  if (WiFi.softAPgetStationNum() == 0) return false;
  return (millis() - lastWebActivityMs) < LOG_PORTAL_HOLD_MS;
#else
  return false;
#endif
}

// Обработчик, работающий с SD: на время его работы запись лога остановлена, а карта
// переведена на скорость портала. sdTask делает это сам (закрывает файл, перемонтирует);
// обработчик ждёт до 3 с, чтобы не читать карту посреди перемонтирования.
struct PortalSdGuard {
  PortalSdGuard() {
    touchWeb();
#if LOG_PAUSE_WHILE_PORTAL
    webSdActive++;
    uint32_t t0 = millis();
    while (!sdPortalMode && millis() - t0 < 3000) delay(20);
#endif
  }
  ~PortalSdGuard() {
#if LOG_PAUSE_WHILE_PORTAL
    if (webSdActive) webSdActive--;
#endif
    touchWeb();
  }
};

String canBitrateFormHtml() {
  String h = "<h2>" + TR("Скорость CAN-шины", "CAN bus bitrate") + "</h2>"
             "<p>" + TR("Сейчас", "Current") + ": <b>" + String(canBitrateLabel(canBitrate)) + "</b>";
  if (canBitrate == CAN_BITRATE_DEFAULT) h += " (" + TR("по умолчанию", "default") + ")";
  // Полные подписи с описанием видны при раскрытии списка; в свёрнутом
  // виде список ограничен шириной экрана (CSS max-width), браузер сам
  // обрезает длинную строку внутри поля, а не выталкивает её за край
  h += "</p><form action='/can' method='POST'><select name='bps' style='width:100%'>";
  for (size_t i = 0; i < CAN_BITRATES_N; i++) {
    const CanBitrate& b = CAN_BITRATES[i];
    const char* hint = uiEn ? b.hint_en : b.hint_ru;
    h += "<option value='" + String(b.bps) + "'";
    if (b.bps == canBitrate) h += " selected";
    h += ">" + String(uiEn ? b.label_en : b.label_ru);
    if (b.bps == CAN_BITRATE_DEFAULT) h += " — " + TR("по умолчанию", "default");
    if (hint[0]) h += " (" + String(hint) + ")";
    h += "</option>";
  }
  h += "</select><br><br>"
       "<button type='submit' onclick=\"return confirm('" +
       TR("Сохранить скорость и перезапустить сниффер?", "Save bitrate and restart the sniffer?") +
       "');\">" + TR("Сохранить и перезапустить", "Save and restart") + "</button></form>";
#if CAN_LISTEN_ONLY
  h += "<p><small>" +
       TR("Режим LISTEN_ONLY: при неверной скорости сниффер просто ничего не примет, шине это не мешает.",
          "LISTEN_ONLY mode: with a wrong bitrate the sniffer simply receives nothing; the bus is not affected.") +
       "</small></p>";
#else
  h += "<p><b>" + TR("ВНИМАНИЕ", "WARNING") + ":</b> " +
       TR("прошивка собрана в режиме NORMAL (стенд). Неверная скорость на реальной шине вызовет "
          "кадры ошибок и может нарушить работу блоков машины!",
          "firmware is built in NORMAL mode (bench). A wrong bitrate on a real bus will cause "
          "error frames and may disrupt the car's control units!") + "</p>";
#endif
  return h;
}

void handleCanBitrate() {
  touchWeb();
  uint32_t bps = (uint32_t)webServer.arg("bps").toInt();
  if (!canBitrateSupported(bps)) {
    webServer.send(400, "text/html; charset=utf-8",
      htmlHead(TR("Ошибка", "Error")) + "<h2>" + TR("Недопустимая скорость", "Invalid bitrate") +
      "</h2><p><a href='/'>" + TR("Назад", "Back") + "</a></p></body></html>");
    return;
  }
  if (bps == canBitrate) {
    webServer.sendHeader("Location", "/");
    webServer.send(303);
    return;
  }
  if (!saveCanBitrate(bps)) {
    webServer.send(500, "text/html; charset=utf-8",
      htmlHead(TR("Ошибка", "Error")) + "<h2>" + TR("Не удалось записать в NVS", "Failed to write to NVS") +
      "</h2><p><a href='/'>" + TR("Назад", "Back") + "</a></p></body></html>");
    return;
  }
  Serial.printf("Скорость CAN: %s -> %s, перезапуск\n",
                canBitrateLabelRu(canBitrate), canBitrateLabelRu(bps));
  webServer.send(200, "text/html; charset=utf-8",
    htmlHead(TR("Готово", "Done")) + "<h2>" + TR("Скорость сохранена", "Bitrate saved") + ": " +
    String(canBitrateLabel(bps)) + "</h2><p>" +
    TR("Перезапуск… Через ~10 с переподключитесь к точке доступа.",
       "Restarting… Reconnect to the access point in ~10 s.") + "</p></body></html>");
  delay(300);
  ledOff();
  closeLogForRestart(CLOSE_REASON_CFG);
  delay(100);
  ESP.restart();
}

void handleRoot() {
  PortalSdGuard portalGuard;
  DateTime now = nowTime();
  char nowStr[32];
  snprintf(nowStr, sizeof(nowStr), "%04d-%02d-%02dT%02d:%02d:%02d",
           now.year(), now.month(), now.day(), now.hour(), now.minute(), now.second());

  String html = htmlHead("CAN Sniffer") +
                "<h2>" + TR("Дата и время", "Date and time") + "</h2>"
                "<p>" + TR("Текущее время", "Current time") + ": <b>" +
                (timeValid ? String(nowStr) + (rtcOscStopped ? TR(" (часы останавливались — сверьте!)",
                                                                 " (clock was stopped — please check!)") : String(""))
                           : TR("НЕ УСТАНОВЛЕНО — лог пишется в /no-rtc",
                                                 "NOT SET — logging to /no-rtc")) + "</b><br><small>" +
                TR("Источник", "Source") + ": " + String(timeSource) + "; " +
                TR("внешние часы", "external RTC") + ": " + String(rtcName()) + "</small></p>"
                "<form action='/set' method='POST'>"
                "<input type='datetime-local' id='dt' name='dt' value='" + String(nowStr) + "' step='1'> "
                // Время телефона: местное, с секундами, в формате datetime-local
                "<button type='button' onclick=\"var d=new Date();d.setMinutes(d.getMinutes()-d.getTimezoneOffset());"
                "document.getElementById('dt').value=d.toISOString().slice(0,19);\">" +
                TR("Время телефона", "Phone time") + "</button>"
                "<br><br><button type='submit'>" + TR("Установить", "Set") + "</button>"
                "</form>"
                "<hr>" + canBitrateFormHtml() +
                "<hr>" + (sdMounted ? String("") :
                  "<p><b style='color:#ff6b6b'>" + TR("SD-карта не смонтирована.", "SD card is not mounted.") + "</b> " +
                  TR("Если карта новая (от 64 ГБ она обычно в exFAT) — её нужно отформатировать в FAT32. Все данные на карте будут удалены.",
                     "If the card is new (64 GB+ cards usually come as exFAT), it must be formatted as FAT32. All data on the card will be erased.") +
                  "</p><form method='POST' action='/sd-format' onsubmit=\"return confirm('" +
                  TR("Отформатировать SD-карту в FAT32? Все данные на ней будут удалены.", "Format the SD card as FAT32? All data on it will be erased.") +
                  "');\"><button type='submit' style='border-color:#c62828;color:#ff6b6b'>" +
                  TR("Форматировать карту в FAT32", "Format card as FAT32") + "</button></form>") +
                "<p><a href='/logs'>" + TR("Список логов на SD-карте", "Logs on SD card") + "</a></p>"
                "<p><a href='/update'>" + TR("Обновление прошивки", "Firmware update") + "</a></p>"
                "<p><small>WiFi: " + TR("канал", "channel") + " " + String(apChannelUsed) +
                (AP_CHANNEL == 0 ? " (" + TR("авто", "auto") + "; " + TR("помеха", "noise") + " 1/6/11: " +
                   String(apChanNoiseDbm[0], 0) + " / " + String(apChanNoiseDbm[1], 0) + " / " +
                   String(apChanNoiseDbm[2], 0) + " dBm)" : String("")) + "</small><br>" +
                (WIFI_ACTIVE_MINUTES > 0 ? "<small>" + TR("WiFi выключится через ", "WiFi turns off in ") +
                   String(millis() < (uint32_t)WIFI_ACTIVE_MINUTES * 60000UL ?
                          ((uint32_t)WIFI_ACTIVE_MINUTES * 60000UL - millis()) / 60000UL + 1 : 0) +
                   TR(" мин (продлевается, пока вы пользуетесь порталом)", " min (extended while you use the portal)") + "</small><br>"
                 : String("")) +
                "<small>CAN: " + TR("принято кадров", "frames received") + " " + String(canFramesTotal) + ", " +
                TR("потеряно", "dropped") + " " + String(canDroppedTotal) + " · <a href='/errors'>" +
                TR("журнал ошибок", "error log") + "</a></small></p>"
                "<hr><p><small>" + TR("Прошивка", "Firmware") + " " FW_VERSION " (" +
                TR("сборка", "build") + " " FW_BUILD ")</small></p>"
                "</body></html>";

  webServer.send(200, "text/html; charset=utf-8", html);
}

void handleSet() {
  touchWeb();
  if (!webServer.hasArg("dt")) {
    webServer.send(400, "text/plain; charset=utf-8", TR("Нет параметра dt", "Missing dt parameter"));
    return;
  }

  String dt = webServer.arg("dt"); // формат YYYY-MM-DDTHH:MM[:SS] из datetime-local
  int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
  int got = sscanf(dt.c_str(), "%d-%d-%dT%d:%d:%d", &year, &month, &day, &hour, &minute, &second);
  // Без секунд браузер присылает "YYYY-MM-DDTHH:MM" — это 5 полей, нормально.
  // Всё остальное (пусто, мусор, 2000-й год) в RTC не пишем.
  if (got < 5 || year < TIME_MIN_YEAR || year > TIME_MAX_YEAR || month < 1 || month > 12 || day < 1 || day > 31 ||
      hour > 23 || minute > 59 || second > 59) {
    Serial.printf("Портал: отклонено некорректное время \"%s\"\n", dt.c_str());
    webServer.send(400, "text/plain; charset=utf-8", TR("Некорректные дата/время", "Invalid date/time"));
    return;
  }

  DateTime t(year, month, day, hour, minute, second);
  applyTime(t.unixtime(), "портал", true);   // системные часы + внешние (через loop)
  // Папка по дате пересчитается при следующем открытии файла лога
  // (ротация или перезапуск) — отсюда её не трогаем: с ней работает sdTask

  Serial.printf("Время установлено вручную: %04d-%02d-%02d %02d:%02d:%02d\n",
                year, month, day, hour, minute, second);

  webServer.sendHeader("Location", "/");
  webServer.send(303);
}

// =====================================================================
// Список папок/файлов на SD + скачивание
// =====================================================================

// Простая защита от выхода за пределы SD через параметр file= —
// на скрытом устройстве, доступном только по вашему собственному WiFi,
// риск невелик, но лишняя проверка не помешает
bool isPathSafe(const String& path) {
  return path.indexOf("..") == -1 && path.startsWith("/");
}

// Служебные папки карты (Windows, macOS) не показываем и не трогаем
bool isSystemFolder(const String& name) {
  String n = name.startsWith("/") ? name.substring(1) : name;
  return n.startsWith(".") || n.startsWith("System Volume Information") || n.length() == 0;
}

// Рекурсивное удаление: сначала собираем имена (удалять во время обхода
// каталога FAT нельзя), потом удаляем файлы и подпапки, затем саму папку
bool removeTree(const String& path, uint32_t& filesDeleted) {
  File dir = LOGFS.open(path);
  if (!dir) return false;
  if (!dir.isDirectory()) {
    dir.close();
    bool ok = LOGFS.remove(path);
    if (ok) filesDeleted++;
    return ok;
  }
  std::vector<String> files, dirs;
  File e = dir.openNextFile();
  while (e) {
    String n = String(e.name());
    if (n.lastIndexOf('/') != -1) n = n.substring(n.lastIndexOf('/') + 1);
    String full = path + "/" + n;
    if (e.isDirectory()) dirs.push_back(full); else files.push_back(full);
    e.close();
    e = dir.openNextFile();
  }
  dir.close();

  bool ok = true;
  for (const String& f : files) {
    if (LOGFS.remove(f)) filesDeleted++; else ok = false;
    touchWeb();
  }
  for (const String& d : dirs) {
    if (!removeTree(d, filesDeleted)) ok = false;
  }
  if (!LOGFS.rmdir(path)) ok = false;
  return ok;
}

// POST /delete: удалить выбранные папки целиком
void handleDelete() {
  PortalSdGuard portalGuard;
  String active = getActiveLogFolder();
  String report;
  uint32_t totalFiles = 0;
  int foldersDone = 0;

  for (int i = 0; i < webServer.args(); i++) {
    if (webServer.argName(i) != "folder") continue;
    String f = webServer.arg(i);
    // Только папки верхнего уровня: "/имя" без вложенных "/"
    if (!isPathSafe(f) || f == "/" || f.indexOf('/', 1) != -1 || isSystemFolder(f)) {
      report += "<li>" + f + " — " + TR("недопустимый путь, пропущено", "invalid path, skipped") + "</li>";
      continue;
    }
    if (active.length() && f == active) {
      report += "<li>" + f + " — " + TR("сейчас идёт запись лога, не удаляю", "log is being written, not deleted") + "</li>";
      continue;
    }
    uint32_t n = 0;
    bool ok = removeTree(f, n);
    totalFiles += n;
    if (ok) foldersDone++;
    report += "<li>" + f + " — " + (ok ? TR("удалено", "deleted") : TR("удалено частично / ошибка", "partially deleted / error")) +
              " (" + String(n) + " " + TR("файлов", "files") + ")</li>";
    Serial.printf("Удаление %s: %s, файлов %lu\n", f.c_str(), ok ? "OK" : "ОШИБКА", (unsigned long)n);
  }
  if (report.length() == 0) report = "<li>" + TR("Ничего не выбрано", "Nothing selected") + "</li>";

  webServer.send(200, "text/html; charset=utf-8",
    htmlHead(TR("Удаление", "Delete")) + "<h2>" + TR("Удаление папок", "Deleting folders") + "</h2><ul>" +
    report + "</ul><p>" + TR("Удалено папок", "Folders deleted") + ": " + String(foldersDone) + ", " +
    TR("файлов", "files") + ": " + String(totalFiles) + "</p><p><a href='/logs'>&larr; " +
    TR("к списку логов", "back to logs") + "</a></p></body></html>");
}

// Размер для людей: 512 Б / 12.3 КБ / 4.0 МБ
String humanSize(uint64_t b) {
  char buf[24];
  if (b < 1024)                 snprintf(buf, sizeof(buf), "%u %s", (unsigned)b, uiEn ? "B" : "Б");
  else if (b < 1024ULL * 1024)  snprintf(buf, sizeof(buf), "%.1f %s", b / 1024.0, uiEn ? "KB" : "КБ");
  else if (b < (1024ULL << 20))  snprintf(buf, sizeof(buf), "%.1f %s", b / 1048576.0, uiEn ? "MB" : "МБ");
  else                          snprintf(buf, sizeof(buf), "%.2f %s", b / 1073741824.0, uiEn ? "GB" : "ГБ");
  return String(buf);
}

// HTML-экранирование для вывода текста журнала
String htmlEscape(const String& in) {
  String o;
  o.reserve(in.length() + 16);
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    if (c == '<') o += "&lt;";
    else if (c == '>') o += "&gt;";
    else if (c == '&') o += "&amp;";
    else if (c != '\r') o += c;
  }
  return o;
}

// Страница журнала ошибок: последние записи, свежие сверху.
// Читаем только хвост файла (до 32 КБ), чтобы большой журнал не съел память.
#define ERRLOG_VIEW_BYTES  (32 * 1024)
#define ERRLOG_VIEW_LINES  300
void handleErrors() {
  PortalSdGuard portalGuard;
  String html = htmlHead(TR("Журнал ошибок", "Error log")) +
                "<h2>" + TR("Журнал ошибок", "Error log") + "</h2>"
                "<p><a href='/'>&larr; " + TR("на главную", "home") + "</a> · <a href='/logs'>" +
                TR("логи", "logs") + "</a></p>";

  if (!sdMounted || !LOGFS.exists(ERRLOG_PATH)) {
    html += "<p class='meta'>" + TR("Журнал пуст — ошибок не было.", "The log is empty — no errors so far.") + "</p>";
  } else {
    String tail;
    size_t size = 0;
    if (errMutex) xSemaphoreTake(errMutex, pdMS_TO_TICKS(2000));
    File f = LOGFS.open(ERRLOG_PATH);
    if (f) {
      size = f.size();
      size_t from = size > ERRLOG_VIEW_BYTES ? size - ERRLOG_VIEW_BYTES : 0;
      f.seek(from);
      tail.reserve(size - from + 1);
      while (f.available()) tail += (char)f.read();
      f.close();
      if (from > 0) {                         // первая строка обрезана — пропускаем
        int nl = tail.indexOf('\n');
        tail = nl >= 0 ? tail.substring(nl + 1) : String("");
      }
    }
    if (errMutex) xSemaphoreGive(errMutex);

    // Разбиваем на строки и выводим в обратном порядке: свежие сверху
    std::vector<int> starts;
    starts.push_back(0);
    for (int i = 0; i < (int)tail.length(); i++)
      if (tail[i] == '\n' && i + 1 < (int)tail.length()) starts.push_back(i + 1);
    int total = starts.size();
    int shown = total < ERRLOG_VIEW_LINES ? total : ERRLOG_VIEW_LINES;

    html += "<p class='meta'>errors.log — " + humanSize(size) + ". " +
            TR("Показаны последние записи, свежие сверху", "Latest entries, newest first") +
            " (" + String(shown) + ").</p>"
            "<pre style='white-space:pre-wrap;word-break:break-word;font-size:12px;line-height:1.45;"
            "background:#161616;border:1px solid #333;padding:10px;border-radius:6px'>";
    for (int k = total - 1; k >= total - shown; k--) {
      int a = starts[k];
      int b = (k + 1 < total) ? starts[k + 1] : tail.length();
      String line = tail.substring(a, b);
      line.trim();
      if (line.length()) html += htmlEscape(line) + "\n";
    }
    html += "</pre>"
            "<p><a href='/download?file=" ERRLOG_PATH "'>" + TR("Скачать errors.log", "Download errors.log") + "</a>";
    if (LOGFS.exists(ERRLOG_OLD_PATH))
      html += " · <a href='/download?file=" ERRLOG_OLD_PATH "'>errors.old.log</a>";
    html += "</p><form method='POST' action='/errors-clear' onsubmit=\"return confirm('" +
            TR("Очистить журнал ошибок?", "Clear the error log?") + "');\">"
            "<button type='submit' style='border-color:#c62828;color:#ff6b6b'>" +
            TR("Очистить журнал", "Clear log") + "</button></form>";
  }
  html += "</body></html>";
  webServer.send(200, "text/html; charset=utf-8", html);
}

void handleErrorsClear() {
  PortalSdGuard portalGuard;
  if (sdMounted) {
    if (errMutex) xSemaphoreTake(errMutex, pdMS_TO_TICKS(2000));
    LOGFS.remove(ERRLOG_PATH);
    LOGFS.remove(ERRLOG_OLD_PATH);
    if (errMutex) xSemaphoreGive(errMutex);
    Serial.println("Портал: журнал ошибок очищен");
  }
  webServer.sendHeader("Location", "/errors");
  webServer.send(303);
}

void handleLogs() {
  PortalSdGuard portalGuard;
  String html = htmlHead(TR("Логи CAN-сниффера", "CAN sniffer logs")) +
                 "<h2>" + TR("Логи по датам", "Logs by date") + "</h2>"
                 "<p><a href='/'>&larr; " + TR("на главную", "home") + "</a></p>"
                 "<form action='/download-tar' method='GET'>"
                 "<p>" + TR("Отметьте папки галочкой, чтобы скачать их одним TAR-архивом или удалить. "
                            "Нажмите «+», чтобы увидеть файлы папки.",
                            "Tick folders to download them as one TAR archive or delete them. "
                            "Press \"+\" to see the files in a folder.") + "</p>";

  File root = LOGFS.open("/");
  if (!root || !root.isDirectory()) {
    html += "<p>" + TR("Не удалось открыть корень SD-карты", "Cannot open SD card root") + "</p></body></html>";
    webServer.send(500, "text/html; charset=utf-8", html);
    return;
  }

  // Журнал ошибок в корне карты — ссылкой вверху страницы
  if (LOGFS.exists(ERRLOG_PATH)) {
    File ef = LOGFS.open(ERRLOG_PATH);
    size_t es = ef ? ef.size() : 0;
    if (ef) ef.close();
    html += "<p>" + TR("Журнал ошибок", "Error log") + ": <a href='/errors'>" + TR("открыть", "view") +
            "</a> · <a href='/download?file=" ERRLOG_PATH "'>errors.log</a> <span class='meta'>(" + humanSize(es) + ")</span>";
    if (LOGFS.exists(ERRLOG_OLD_PATH))
      html += " · <a href='/download?file=" ERRLOG_OLD_PATH "'>errors.old.log</a>";
    html += "</p>";
  } else {
    html += "<p><span class='meta'>" + TR("Журнал ошибок пуст", "Error log is empty") + "</span></p>";
  }

  bool foundAny = false;
  bool foundFolder = false;
  String active = getActiveLogFolder();
  String activePath = getActiveLogPath();
  File dateEntry = root.openNextFile();
  while (dateEntry) {
    String entryName = String(dateEntry.name());
    if (dateEntry.isDirectory() && !isSystemFolder(entryName)) {
      String folderName = entryName;
      if (!folderName.startsWith("/")) folderName = "/" + folderName;
      foundFolder = true;

      // Сначала собираем список файлов (для счётчика и размера в заголовке),
      // потом выводим папку свёрнутой: содержимое раскрывается по "+"
      String items;
      uint32_t nFiles = 0;
      uint64_t total = 0;
      File folder = LOGFS.open(folderName);
      File fileEntry = folder.openNextFile();
      while (fileEntry) {
        if (!fileEntry.isDirectory()) {
          String fileName = String(fileEntry.name());
          if (!fileName.startsWith("/")) fileName = "/" + fileName;
          String fullPath = folderName + "/" + fileName.substring(fileName.lastIndexOf('/') + 1);
          size_t sizeBytes = fileEntry.size();
          bool writing = (fullPath == activePath);
          items += "<li><a href='/download?file=" + fullPath + "'>" +
                   fullPath.substring(fullPath.lastIndexOf('/') + 1) + "</a> <span class='meta'>(" +
                   humanSize(sizeBytes) + (writing ? ", " + TR("пишется — при скачивании будет закрыт",
                                                                    "being written — will be closed on download") : String("")) +
                   ")</span></li>";
          nFiles++;
          total += sizeBytes;
          foundAny = true;
        }
        fileEntry.close();
        fileEntry = folder.openNextFile();
      }
      folder.close();

      html += "<div class='fold'><input type='checkbox' name='folder' value='" + folderName +
              "' title='" + TR("выбрать папку", "select folder") + "'><details><summary>" + folderName +
              " <span class='meta'>— " + String(nFiles) + " " + TR("файл(ов)", "file(s)") + ", " +
              humanSize(total);
      if (active.length() && folderName == active)
        html += ", " + TR("идёт запись", "recording");
      html += "</span></summary><ul>" +
              (nFiles ? items : "<li><i>" + TR("пусто", "empty") + "</i></li>") +
              "</ul></details></div>";
    }
    dateEntry.close();
    dateEntry = root.openNextFile();
  }
  root.close();

  if (!foundFolder) {
    html += "<p>" + TR("Папок с логами пока нет", "No log folders yet") + "</p>";
  } else {
    if (foundAny)
      html += "<br><button type='submit'>" + TR("Скачать выбранное одним TAR-файлом",
                                                    "Download selected as one TAR file") + "</button>";
    // Та же форма, другой адрес: удаляются отмеченные папки целиком
    html += " <button type='submit' formaction='/delete' formmethod='POST' "
            "style='border-color:#c62828;color:#ff6b6b' "
            "onclick=\"var n=document.querySelectorAll('input[name=folder]:checked').length;"
            "if(!n){alert('" + TR("Отметьте папки для удаления", "Select folders to delete") + "');return false;}"
            "return confirm('" + TR("Удалить выбранные папки со всем содержимым (", "Delete selected folders with all contents (") +
            "'+n+')? " + TR("Отменить будет нельзя.", "This cannot be undone.") + "');\">" +
            TR("Удалить выбранные папки", "Delete selected folders") + "</button>";
  }

  html += "</form></body></html>";
  webServer.send(200, "text/html; charset=utf-8", html);
}

// ---------- Надёжная отдача больших файлов ----------
// WebServer::streamFile()/sendContent() не повторяют запись: если TCP-буфер
// занят дольше таймаута WiFiClient, write() возвращает 0, кусок данных
// молча теряется, а браузер ждёт обещанные Content-Length байт и "висит".
// Здесь дописываем каждый кусок до конца с повторами, а обрыв считаем
// только при реальном отключении клиента или 15 с без прогресса.
#define DL_CHUNK        4096
#define DL_STALL_MS     15000
static uint8_t dlBuf[DL_CHUNK];

bool dlSendAll(WiFiClient& client, const uint8_t* data, size_t len) {
  uint32_t lastProgress = millis();
  while (len > 0) {
    if (!client.connected()) return false;
    size_t n = client.write(data, len);
    if (n > 0) {
      data += n; len -= n;
      lastProgress = millis();
    } else {
      if (millis() - lastProgress > DL_STALL_MS) return false;
      delay(2);   // даём стеку WiFi разгрести буфер
    }
  }
  return true;
}

// Отправить из файла ровно want байт (если файл короче — добить нулями,
// чтобы не нарушить заранее объявленный размер). Возвращает false при обрыве.
bool dlSendFileBytes(WiFiClient& client, File& f, uint32_t want, uint32_t& sentTotal) {
  static uint32_t lastTouch = 0;
  while (want > 0) {
    size_t toRead = want < DL_CHUNK ? want : DL_CHUNK;
    size_t got = f ? f.read(dlBuf, toRead) : 0;
    if (got < toRead) memset(dlBuf + got, 0, toRead - got);
    if (!dlSendAll(client, dlBuf, toRead)) return false;
    want -= toRead;
    sentTotal += toRead;
    if (millis() - lastTouch > 2000) {   // долгая выгрузка — портал "в работе"
      touchWeb();
      lastTouch = millis();
    }
  }
  return true;
}

// На время выгрузки останавливаем BLE-рекламу: WiFi и BLE делят одно
// радио, частая реклама заметно режет скорость WiFi
bool dlPauseBle() {
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  bool was = adv->isAdvertising();
  if (was) adv->stop();
  return was;
}
void dlResumeBle(bool was) {
  if (was && NimBLEDevice::getServer()->getConnectedCount() == 0) {
    NimBLEDevice::startAdvertising();
  }
}

// Закрыть соединение после отдачи файла. Сервер шлёт "Connection: close",
// и часть браузеров (Samsung Internet, менеджеры загрузок) считают
// скачивание завершённым только по закрытию соединения, а не по
// Content-Length — без этого висит "100%". Данные из TCP-буфера при
// закрытии всё равно уходят (FIN отправляется после них).
void dlFinish(WiFiClient& client) {
  delay(20);
  client.stop();
}

// Закрыть открытый на запись файл, если он попадает в скачивание.
// true — файла на запись больше нет (или не было).
bool closeActiveLogForDownload() {
  if (getActiveLogPath().length() == 0) return true;
  CanLogEntry e;
  memset(&e, 0, sizeof(e));
  e.id = ROTATE_SENTINEL_ID;
  xSemaphoreTake(rotateDoneSemaphore, 0);                 // сбросить старый сигнал
  if (xQueueSend(canQueue, &e, pdMS_TO_TICKS(500)) != pdTRUE) return false;
  bool ok = xSemaphoreTake(rotateDoneSemaphore, pdMS_TO_TICKS(20000)) == pdTRUE;   // LZMA дописывает хвост
  if (!ok) errLog("Портал: файл лога не закрылся для скачивания за 20 с");
  return ok;
}

void handleDownload() {
  PortalSdGuard portalGuard;
  if (!webServer.hasArg("file")) {
    webServer.send(400, "text/plain; charset=utf-8", TR("Не указан параметр file", "Missing file parameter"));
    return;
  }

  String path = webServer.arg("file");
  if (!isPathSafe(path)) {
    webServer.send(400, "text/plain; charset=utf-8", TR("Недопустимый путь", "Invalid path"));
    return;
  }

  // Скачивают файл, который сейчас пишется, — сначала закрываем его штатно
  if (path == getActiveLogPath()) closeActiveLogForDownload();
  if (path == getActiveLogPath()) {
    webServer.send(409, "text/plain; charset=utf-8",
      TR("Файл сейчас пишется и не закрылся — попробуйте ещё раз", "File is being written and could not be closed — try again"));
    return;
  }
  File f = LOGFS.open(path, FILE_READ);
  if (!f || f.isDirectory()) {
    webServer.send(404, "text/plain; charset=utf-8", TR("Файл не найден: ", "File not found: ") + path);
    return;
  }

  // Размер фиксируем в момент открытия: сегодняшний лог продолжает расти,
  // отдаём ровно то, что было на этот момент
  uint32_t size = f.size();
  String downloadName = path.substring(path.lastIndexOf('/') + 1);
  webServer.sendHeader("Content-Disposition", "attachment; filename=\"" + downloadName + "\"");
  webServer.setContentLength(size);
  webServer.send(200, "application/octet-stream", "");

  bool bleWas = dlPauseBle();
  uint32_t t0 = millis(), sent = 0;
  WiFiClient client = webServer.client();
  bool ok = dlSendFileBytes(client, f, size, sent);
  f.close();
  dlFinish(client);
  dlResumeBle(bleWas);

  uint32_t dt = millis() - t0;
  Serial.printf("Скачивание %s: %s, %lu из %lu байт за %lu мс (%lu КБ/с)\n",
                path.c_str(), ok ? "OK" : "ОБРЫВ", (unsigned long)sent, (unsigned long)size,
                (unsigned long)dt, (unsigned long)(dt ? sent / dt : 0));
  touchWeb();
}

// =====================================================================
// Скачивание нескольких папок одним TAR-архивом (без сжатия — самый
// простой формат для генерации "на лету" на embedded-устройстве)
// =====================================================================

// Записывает одно октальное числовое поле фиксированной ширины
// в TAR-заголовок (формат ustar): (fieldLen-1) октальных цифр + NUL
void tarWriteOctalField(uint8_t* header, int offset, int fieldLen, unsigned long value) {
  char buf[16];
  snprintf(buf, sizeof(buf), "%0*lo", fieldLen - 1, value);
  memcpy(header + offset, buf, fieldLen - 1);
  header[offset + fieldLen - 1] = '\0';
}

// Собирает 512-байтный заголовок файла в формате ustar tar
void tarBuildHeader(uint8_t* header, const String& name, uint32_t size, uint32_t mtime) {
  memset(header, 0, 512);

  strncpy((char*)header, name.c_str(), 100); // имя файла внутри архива

  tarWriteOctalField(header, 100, 8, 0644);   // mode
  tarWriteOctalField(header, 108, 8, 0);      // uid
  tarWriteOctalField(header, 116, 8, 0);      // gid
  tarWriteOctalField(header, 124, 12, size);  // size
  tarWriteOctalField(header, 136, 12, mtime); // mtime

  memset(header + 148, ' ', 8);  // поле checksum временно заполняем пробелами
  header[156] = '0';             // typeflag: обычный файл

  memcpy(header + 257, "ustar", 5);
  header[262] = '\0';
  header[263] = '0';
  header[264] = '0';             // magic "ustar" + version "00"

  // Контрольная сумма — сумма всех байт заголовка (пока chksum = пробелы)
  unsigned int sum = 0;
  for (int i = 0; i < 512; i++) sum += header[i];

  char chkbuf[7];
  snprintf(chkbuf, sizeof(chkbuf), "%06o", sum);
  memcpy(header + 148, chkbuf, 6);
  header[154] = '\0';
  header[155] = ' ';
}

// Элемент архива: размер фиксируется на первом проходе и дальше не меняется
struct TarItem {
  String path;        // путь на SD
  String archiveName; // имя внутри архива
  uint32_t size;
};

void handleDownloadTar() {
  PortalSdGuard portalGuard;
  // Собираем список выбранных папок из повторяющихся параметров folder=...
  std::vector<String> folders;
  for (int i = 0; i < webServer.args(); i++) {
    if (webServer.argName(i) == "folder") {
      String f = webServer.arg(i);
      if (isPathSafe(f)) folders.push_back(f);
    }
  }

  if (folders.empty()) {
    webServer.send(400, "text/plain; charset=utf-8", TR("Не выбрано ни одной папки", "No folders selected"));
    return;
  }

  // Если среди выбранных папок есть та, куда идёт запись, — закрываем
  // текущий файл: он попадёт в архив целиком, а новый (открытый уже после
  // этого) в архив не включается
  String activeFolder = getActiveLogFolder();
  for (const String& fp : folders)
    if (activeFolder.length() && fp == activeFolder) { closeActiveLogForDownload(); break; }
  String activeNow = getActiveLogPath();

  // Проход 1: список файлов и их размеры -> точный размер архива.
  // С известным Content-Length браузер показывает прогресс, и не нужен
  // chunked-режим (в нём недописанный кусок ломает весь поток).
  std::vector<TarItem> items;
  uint64_t total = 1024;   // два пустых блока в конце
  for (const String& folderPath : folders) {
    File folder = LOGFS.open(folderPath);
    if (!folder || !folder.isDirectory()) continue;
    // Имя папки без ведущего "/" — внутри архива структура "2026-09-08/can_log_0001.txt"
    String folderNameInArchive = folderPath.startsWith("/") ? folderPath.substring(1) : folderPath;
    File e = folder.openNextFile();
    while (e) {
      if (!e.isDirectory()) {
        String fileName = String(e.name());
        if (fileName.lastIndexOf('/') != -1) fileName = fileName.substring(fileName.lastIndexOf('/') + 1);
        TarItem it;
        it.path = folderPath + "/" + fileName;
        if (it.path == activeNow) {                 // пишется прямо сейчас — пропускаем
          e.close();
          e = folder.openNextFile();
          continue;
        }
        it.archiveName = folderNameInArchive + "/" + fileName;
        it.size = e.size();
        total += 512 + ((uint64_t)(it.size + 511) / 512) * 512;
        items.push_back(it);
      }
      e.close();
      e = folder.openNextFile();
    }
    folder.close();
  }

  webServer.sendHeader("Content-Disposition", "attachment; filename=\"can_logs.tar\"");
  webServer.setContentLength((size_t)total);
  webServer.send(200, "application/x-tar", "");

  bool bleWas = dlPauseBle();
  uint32_t t0 = millis(), sent = 0;
  WiFiClient client = webServer.client();
  bool ok = true;
  uint32_t mtime = nowTime().unixtime();

  // Проход 2: заголовок + ровно объявленный размер + выравнивание до 512
  for (TarItem& it : items) {
    uint8_t header[512];
    tarBuildHeader(header, it.archiveName, it.size, mtime);
    if (!(ok = dlSendAll(client, header, 512))) break;
    sent += 512;
    File f = LOGFS.open(it.path, FILE_READ);   // пропал файл — dlSendFileBytes добьёт нулями
    uint32_t padded = ((it.size + 511) / 512) * 512;
    ok = dlSendFileBytes(client, f, padded, sent);   // хвост после size = нули
    if (f) f.close();
    if (!ok) break;
  }
  if (ok) {
    memset(dlBuf, 0, 1024);
    ok = dlSendAll(client, dlBuf, 1024);
    if (ok) sent += 1024;
  }
  dlFinish(client);
  dlResumeBle(bleWas);

  uint32_t dt = millis() - t0;
  Serial.printf("TAR (%u файлов): %s, %lu из %llu байт за %lu мс (%lu КБ/с)\n",
                (unsigned)items.size(), ok ? "OK" : "ОБРЫВ", (unsigned long)sent,
                (unsigned long long)total, (unsigned long)dt, (unsigned long)(dt ? sent / dt : 0));
  touchWeb();
}

// Пароль для прошивки — общий для ArduinoOTA (espota/IDE) и веб-страницы
// /update (там логин OTA_WEB_USER). СМЕНИТЕ перед использованием.
// OTA_PASSWORD и OTA_WEB_USER (пароль и логин прошивки по воздуху) — в secrets.h

// Отдельный флаг — пока идёт OTA-заливка, ни в коем случае нельзя уходить
// в deep sleep (даже если ACC внезапно пропало во время прошивки) — это
// гарантированно испортит прошивку на середине записи
volatile bool otaInProgress = false;

void setupOTA() {
  // Пароль ОБЯЗАТЕЛЕН — иначе кто угодно в радиусе действия вашей WiFi-сети
  // сможет залить произвольную прошивку на плату. Смените на свой перед
  // использованием.
  ArduinoOTA.setHostname("s3-can-sniffer");
  ArduinoOTA.setPassword(OTA_PASSWORD);

  ArduinoOTA.onStart([]() {
    otaInProgress = true;
    Serial.println("OTA: начало заливки прошивки");
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("OTA: заливка завершена, перезагрузка...");
    closeLogForRestart(CLOSE_REASON_OTA);
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("OTA: %u%%\r", (progress * 100) / total);
  });
  ArduinoOTA.onError([](ota_error_t error) {
    otaInProgress = false;
    Serial.printf("OTA: ошибка [%u]\n", error);
  });

  ArduinoOTA.begin();
  Serial.println("OTA готово — в Arduino IDE выберите Tools -> Port -> "
                  "s3-can-sniffer (сетевой порт), пароль: смотрите в коде");
}

// =====================================================================
// Прошивка через браузер: http://192.168.4.1/update
// Нужен раздел OTA (Partition Scheme с двумя app-слотами), см. шапку.
// =====================================================================

static bool   webUpdAuthOk  = false;
// Заголовок образа собираем отдельно: первый кусок от WebServer может
// оказаться короче заголовка (или пустым), проверять его "как есть" нельзя
static uint8_t webUpdHdr[sizeof(esp_image_header_t)];
static size_t  webUpdHdrHave = 0;
static bool    webUpdHdrOk   = false;
static bool   webUpdFailed  = false;
static String webUpdError;

void handleUpdatePage() {
  touchWeb();
  if (!webServer.authenticate(OTA_WEB_USER, OTA_PASSWORD)) {
    return webServer.requestAuthentication(BASIC_AUTH, "CAN Sniffer OTA");
  }
  String html = htmlHead(TR("CAN Sniffer — прошивка", "CAN Sniffer — firmware")) +
    "<h2>" + TR("Обновление прошивки", "Firmware update") + "</h2>"
    "<p>" + TR("Текущая версия", "Current version") + ": <b>" FW_VERSION "</b> (" +
    TR("сборка", "build") + " " FW_BUILD ")</p>"
    "<p>" + TR("Файл", "File") + ": <code>*.ino.bin</code> " +
    TR("из Sketch → Export Compiled Binary (не merged и не bootloader).",
       "from Sketch → Export Compiled Binary (not merged, not bootloader).") + "</p>"
    "<form method='POST' action='/update' enctype='multipart/form-data' "
    "onsubmit=\"document.getElementById('b').disabled=true;"
    "document.getElementById('s').innerText='" +
    TR("Заливаю, не закрывайте страницу и не выключайте зажигание…",
       "Uploading, do not close the page or switch off the ignition…") + "';\">"
    "<input type='file' name='fw' accept='.bin' required><br><br>"
    "<button id='b' type='submit'>" + TR("Залить", "Upload") + "</button></form>"
    "<p id='s'></p>"
    "<p>" + TR("Сейчас", "Current") + ": " + String(ESP.getSketchSize() / 1024) + " " + TR("КБ", "KB") + ", " +
    TR("свободно в слоте OTA", "free in OTA slot") + ": " +
    String(ESP.getFreeSketchSpace() / 1024) + " " + TR("КБ", "KB") + "</p>"
    "<hr><p><a href='/'>" + TR("На главную", "Home") + "</a></p></body></html>";
  webServer.send(200, "text/html; charset=utf-8", html);
}

// Вызывается кусками по мере приёма файла
void handleUpdateUpload() {
  touchWeb();
  HTTPUpload& up = webServer.upload();

  if (up.status == UPLOAD_FILE_START) {
    webUpdFailed = false;
    webUpdError  = "";
    webUpdHdrHave = 0;
    webUpdHdrOk   = false;
    webUpdAuthOk = webServer.authenticate(OTA_WEB_USER, OTA_PASSWORD);
    if (!webUpdAuthOk) {
      webUpdFailed = true; webUpdError = TR("Нет авторизации", "Not authorized");
      return;
    }
    otaInProgress = true;
    Serial.printf("WEB OTA: приём %s\n", up.filename.c_str());
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
      webUpdFailed = true;
      webUpdError  = String("Update.begin: ") + Update.errorString() +
                     TR(" (нет OTA-раздела? см. Partition Scheme)", " (no OTA partition? check Partition Scheme)");
    }
    return;
  }

  if (webUpdFailed) return;  // дочитываем запрос вхолостую

  if (up.status == UPLOAD_FILE_WRITE) {
    uint8_t* p = up.buf;
    size_t   n = up.currentSize;

    // Сначала набираем заголовок образа (24 байта), проверяем и только
    // потом начинаем писать во flash
    if (!webUpdHdrOk) {
      size_t take = sizeof(webUpdHdr) - webUpdHdrHave;
      if (take > n) take = n;
      memcpy(webUpdHdr + webUpdHdrHave, p, take);
      webUpdHdrHave += take; p += take; n -= take;
      if (webUpdHdrHave < sizeof(webUpdHdr)) return;   // ждём ещё данных

      const esp_image_header_t* hdr = (const esp_image_header_t*)webUpdHdr;
      Serial.printf("WEB OTA: заголовок %02X %02X %02X %02X ... chip_id=%d\n",
                    webUpdHdr[0], webUpdHdr[1], webUpdHdr[2], webUpdHdr[3],
                    (int)hdr->chip_id);
      if (webUpdHdr[0] != ESP_IMAGE_HEADER_MAGIC) {
        webUpdFailed = true;
        webUpdError  = TR("Это не образ приложения ESP32 (нет 0xE9 в начале)",
                          "Not an ESP32 application image (no 0xE9 at start)");
        Update.abort(); return;
      }
      if (hdr->chip_id != CONFIG_IDF_FIRMWARE_CHIP_ID) {
        webUpdFailed = true;
        webUpdError  = TR("Прошивка для другого чипа (chip_id=", "Firmware for another chip (chip_id=") +
                       String((int)hdr->chip_id) + TR(", нужен ", ", expected ") +
                       String((int)CONFIG_IDF_FIRMWARE_CHIP_ID) + " — ESP32-S3)";
        Update.abort(); return;
      }
      webUpdHdrOk = true;
      if (Update.write(webUpdHdr, sizeof(webUpdHdr)) != sizeof(webUpdHdr)) {
        webUpdFailed = true; webUpdError = TR("Запись: ", "Write: ") + Update.errorString();
        Update.abort(); return;
      }
    }

    if (n && Update.write(p, n) != n) {
      webUpdFailed = true; webUpdError = TR("Запись: ", "Write: ") + Update.errorString();
      Update.abort();
    }
    return;
  }

  if (up.status == UPLOAD_FILE_END) {
    if (!webUpdHdrOk) {
      webUpdFailed = true; webUpdError = TR("Файл слишком короткий или пустой", "File is too short or empty");
      Update.abort(); return;
    }
    if (!Update.end(true)) {  // true — размер берём по факту принятого
      webUpdFailed = true; webUpdError = TR("Проверка образа: ", "Image check: ") + Update.errorString();
    } else {
      Serial.printf("WEB OTA: принято %u байт, образ проверен\n", up.totalSize);
    }
    return;
  }

  if (up.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
    webUpdFailed = true; webUpdError = TR("Передача прервана", "Transfer aborted");
  }
}

// Вызывается после приёма всего запроса
void handleUpdateDone() {
  touchWeb();
  if (!webUpdAuthOk) {
    otaInProgress = false;
    return webServer.requestAuthentication(BASIC_AUTH, "CAN Sniffer OTA");
  }
  if (webUpdFailed) {
    otaInProgress = false;
    errLog("WEB OTA: ошибка — %s", webUpdError.c_str());
    webServer.send(500, "text/html; charset=utf-8",
      htmlHead(TR("Ошибка прошивки", "Update error")) + "<h2>" + TR("Ошибка", "Error") + "</h2><p>" +
      webUpdError + "</p><p>" + TR("Старая прошивка не тронута.", "The old firmware is untouched.") +
      "</p><p><a href='/update'>" + TR("Назад", "Back") + "</a></p></body></html>");
    return;
  }
  webServer.send(200, "text/html; charset=utf-8",
    htmlHead(TR("Готово", "Done")) + "<h2>" + TR("Прошивка залита", "Firmware uploaded") + "</h2><p>" +
    TR("Перезагрузка… Через ~10 с переподключитесь к точке доступа.",
       "Rebooting… Reconnect to the access point in ~10 s.") + "</p></body></html>");
  delay(300);                 // дать ответу уйти в браузер
  ledOff();
  closeLogForRestart(CLOSE_REASON_OTA);   // штатно закрыть лог на SD
  Serial.println("WEB OTA: перезагрузка в новую прошивку");
  delay(100);
  ESP.restart();
}

// ---------------------------------------------------------------------
// Автовыбор канала WiFi. Каждая найденная сеть добавляет "помеху" каналам
// 1 / 6 / 11 с весом:
//   - по мощности: линейно (мВт), т.е. сеть на −45 дБм весит как 1000 сетей
//     на −75 дБм — одна громкая соседка хуже пяти далёких;
//   - по перекрытию: полосы 2.4 ГГц шириной ~4 канала, сосед через 1–4
//     канала мешает частично (1.0 / 0.7 / 0.4 / 0.15 / 0.05).
// Выбирается канал с наименьшей суммой.
// ---------------------------------------------------------------------

int pickQuietChannel() {
  static const float overlap[5] = {1.0f, 0.7f, 0.4f, 0.15f, 0.05f};
  double noise[3] = {0, 0, 0};
  for (int i = 0; i < 3; i++) apChanNets[i] = 0;

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  uint32_t t0 = millis();
  int n = WiFi.scanNetworks(false, true);        // синхронно, со скрытыми сетями
  for (int k = 0; k < n; k++) {
    int ch = WiFi.channel(k);
    int rssi = WiFi.RSSI(k);
    double mw = pow(10.0, rssi / 10.0);          // дБм -> мВт
    for (int i = 0; i < 3; i++) {
      int d = abs(ch - AP_CANDIDATES[i]);
      if (d < 5) { noise[i] += mw * overlap[d]; apChanNets[i]++; }
    }
  }
  WiFi.scanDelete();

  int best = 0;
  for (int i = 0; i < 3; i++) {
    apChanNoiseDbm[i] = noise[i] > 0 ? 10.0 * log10(noise[i]) : -100.0f;
    if (noise[i] < noise[best]) best = i;
  }
  apChannelUsed = AP_CANDIDATES[best];
  Serial.printf("WiFi: найдено сетей %d за %lu мс; помеха: к1 %.0f дБм (%d), к6 %.0f дБм (%d), к11 %.0f дБм (%d) -> канал %d\n",
                n < 0 ? 0 : n, (unsigned long)(millis() - t0),
                apChanNoiseDbm[0], apChanNets[0], apChanNoiseDbm[1], apChanNets[1],
                apChanNoiseDbm[2], apChanNets[2], apChannelUsed);
  return apChannelUsed;
}

void setupWiFiAndWebServer() {
  int ch = AP_CHANNEL;
#if AP_CHANNEL == 0
  ch = pickQuietChannel();
#else
  apChannelUsed = AP_CHANNEL;
#endif
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD, ch, WIFI_AP_HIDDEN);
  WiFi.setTxPower(WIFI_TX_POWER);   // после softAP — до старта мощность не применяется

  Serial.print("Веб-страница установки времени: http://");
  Serial.println(WiFi.softAPIP());
  Serial.printf("Точка доступа: %s (%s), канал %d%s, мощность %.1f дБм\n", AP_SSID,
                WIFI_AP_HIDDEN ? "скрытая" : "видимая", ch, AP_CHANNEL == 0 ? " (авто)" : "",
                WiFi.getTxPower() / 4.0f);

  // mDNS нужен, чтобы Arduino IDE увидела плату по имени как сетевой порт
  MDNS.begin("s3-can-sniffer");

  webServer.on("/", handleRoot);
  webServer.on("/set", HTTP_POST, handleSet);
  webServer.on("/can", HTTP_POST, handleCanBitrate);
  webServer.on("/set-lang", HTTP_POST, handleSetLang);
  webServer.on("/logs", handleLogs);
  webServer.on("/download", HTTP_GET, handleDownload);
  webServer.on("/download-tar", HTTP_GET, handleDownloadTar);
  webServer.on("/delete", HTTP_POST, handleDelete);
  webServer.on("/sd-format", HTTP_POST, handleSdFormat);
  webServer.on("/errors", HTTP_GET, handleErrors);
  webServer.on("/errors-clear", HTTP_POST, handleErrorsClear);
  webServer.on("/update", HTTP_GET, handleUpdatePage);
  webServer.on("/update", HTTP_POST, handleUpdateDone, handleUpdateUpload);
  webServer.begin();

  setupOTA();
}

// =====================================================================
// BLE-сервер синхронизации
// =====================================================================
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

// !!! ВАЖНО: отправлять синхропакет нужно ИМЕННО отсюда, а не из
// ServerCallbacks::onConnect — иначе получается гонка состояний:
// сервер шлёт notify() в момент установления соединения, а клиент
// успевает вызвать subscribe() (включить приём уведомлений) только
// ПОСЛЕ того, как connect() уже вернул управление — то есть УЖЕ ПОСЛЕ
// того, как сервер отправил пакет. Уведомления, отправленные до того,
// как клиент включил их приём (записал дескриптор CCCD), просто
// теряются. onSubscribe() вызывается ровно в момент этой записи —
// надёжная точка для однократной отправки.
class SyncCharCallbacks : public NimBLECharacteristicCallbacks {
  void onSubscribe(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo, uint16_t subValue) override {
    if (subValue != 0 && !bleAuthorized(connInfo)) { Serial.println("BLE: подписка на время отклонена — нет сопряжения"); return; }
    if (subValue == 0) return; // клиент отписался — ничего не шлём

    SyncPacket packet;
    packet.millisValue = millis();
    // Без достоверного времени отдаём 0 — камера увидит, что даты нет
    packet.unixEpoch = timeValid ? (uint32_t)time(nullptr) : 0;

    pCharacteristic->setValue((uint8_t*)&packet, sizeof(packet));
    pCharacteristic->notify();

    Serial.printf("Камера подписалась, отправлена синхронизация: millis=%lu, epoch=%lu\n",
                  (unsigned long)packet.millisValue, (unsigned long)packet.unixEpoch);
  }
};

// Колбэк на запись в характеристику выбора ID — вызывается, когда
// LCD-экран присылает 4 байта нового наблюдаемого CAN ID (uint32,
// little-endian, как формирует наш же клиентский код на LCD)
class WatchSelectCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) override {
    if (!bleAuthorized(connInfo)) return;
    std::string val = pCharacteristic->getValue();
    if (val.length() >= sizeof(uint32_t)) {
      uint32_t newId;
      memcpy(&newId, val.data(), sizeof(newId));
      watchedCanId = newId;
      Serial.printf("Новый наблюдаемый ID от LCD: 0x%03lX\n", (unsigned long)newId);
    }
  }
};

// Запись списка ACL: [версия 0x01][правило 12 байт] × N. Пустой список
// (только байт версии) — остановить поток.
class AclCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) override {
    if (!bleAuthorized(connInfo)) { Serial.println("BLE ACL: клиент не прошёл сопряжение — список не принят"); return; }
    NimBLEAttValue v = pCharacteristic->getValue();
    size_t L = v.length();
    const uint8_t* d = v.data();
    if (L < 1 || d[0] != ACL_VERSION || (L - 1) % sizeof(AclRule) != 0) {
      Serial.printf("BLE ACL: неверный формат (%u байт) — список не принят\n", (unsigned)L);
      return;
    }
    size_t n = (L - 1) / sizeof(AclRule);
    if (n > ACL_MAX_RULES) n = ACL_MAX_RULES;
    AclSet* next = (aclCur == &aclSets[0]) ? &aclSets[1] : &aclSets[0];
    memcpy(next->r, d + 1, n * sizeof(AclRule));
    next->n = n;
    memset(aclState, 0, sizeof(aclState));      // новые правила — новая память изменений
    aclCur = next;                               // атомарное переключение
    aclOwnerConn = connInfo.getConnHandle();
    Serial.printf("BLE ACL: принято правил %u от %s\n", (unsigned)n, connInfo.getAddress().toString().c_str());
  }
};

// Подписка на поток кадров: запоминаем MTU соединения для размера пачки
class FramesCallbacks : public NimBLECharacteristicCallbacks {
  void onSubscribe(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo, uint16_t subValue) override {
    if (subValue != 0 && !bleAuthorized(connInfo)) { framesSubscribed = false; Serial.println("BLE: подписка отклонена — нет сопряжения (подписывайтесь после шифрования)"); return; }
    framesSubscribed = subValue != 0;
    framesMtu = connInfo.getMTU();
    Serial.printf("BLE: подписка на поток кадров %s, MTU %u\n", framesSubscribed ? "вкл" : "выкл", framesMtu);
  }
};

// Имя устройства — вынесено в константу, используется и при инициализации,
// и при явной настройке рекламируемого имени (см. пояснение ниже)
#define SNIFFER_BLE_NAME  "S3-CAN-Sniffer"

void setupBLESync() {
  NimBLEDevice::init(SNIFFER_BLE_NAME);
  bleSecuritySetup();                 // защита: код доступа, один клиент, сброс по BOOT
  NimBLEDevice::setMTU(247);          // крупные пачки кадров в одном уведомлении

  bleQueue = xQueueCreate(ACL_QUEUE_LEN, sizeof(AclFrame));
  xTaskCreatePinnedToCore(bleTxTask, "bleTx", 4096, NULL, 1, NULL, 1);

  NimBLEServer* pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());
  pServer->advertiseOnDisconnect(false);   // рекламу возобновляем сами (один клиент)

  NimBLEService* pService = pServer->createService(SYNC_SERVICE_UUID);
  syncCharacteristic = pService->createCharacteristic(
      SYNC_CHAR_UUID,
      NIMBLE_PROPERTY::NOTIFY
  );
  syncCharacteristic->setCallbacks(new SyncCharCallbacks());

  // Выбор наблюдаемого ID — LCD пишет сюда 4 байта (uint32) нужного ID
  watchSelectCharacteristic = pService->createCharacteristic(
      WATCH_SELECT_CHAR_UUID,
      NIMBLE_PROPERTY::WRITE | BLE_PROP_AUTH_W
  );
  watchSelectCharacteristic->setCallbacks(new WatchSelectCallbacks());

  // Сырые данные наблюдаемого кадра — шлём notify() при каждом кадре,
  // совпавшем с watchedCanId (см. canTask ниже)
  watchDataCharacteristic = pService->createCharacteristic(
      WATCH_DATA_CHAR_UUID,
      NIMBLE_PROPERTY::NOTIFY
  );

  // Поток кадров по ACL: список правил (WRITE/READ) и сами кадры (NOTIFY)
  aclCharacteristic = pService->createCharacteristic(
      ACL_CHAR_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::READ | BLE_PROP_AUTH_RW, 1 + ACL_MAX_RULES * sizeof(AclRule));
  aclCharacteristic->setCallbacks(new AclCallbacks());
  framesCharacteristic = pService->createCharacteristic(FRAMES_CHAR_UUID, NIMBLE_PROPERTY::NOTIFY, 512);
  framesCharacteristic->setCallbacks(new FramesCallbacks());

  pService->start();

  // Стандартный Device Information Service — HUD/камера/nRF Connect
  // могут прочитать версию прошивки сниффера
  NimBLEService* pDis = pServer->createService("180A");
  pDis->createCharacteristic("2A29", NIMBLE_PROPERTY::READ)->setValue("DIY");            // Manufacturer
  pDis->createCharacteristic("2A24", NIMBLE_PROPERTY::READ)->setValue("S3-CAN-Sniffer"); // Model
  pDis->createCharacteristic("2A26", NIMBLE_PROPERTY::READ)->setValue(FW_VERSION);       // Firmware Rev
  pDis->createCharacteristic("2A28", NIMBLE_PROPERTY::READ)->setValue(FW_BUILD);         // Software Rev
  pDis->start();

  // !!! NimBLE-Arduino 2.x: реклама имени устройства больше НЕ происходит
  // автоматически от NimBLEDevice::init() — это нужно явно включить,
  // иначе сканирующая сторона видит MAC-адрес, но пустое имя (ровно то,
  // что и происходило до этого исправления).
  NimBLEAdvertising* pAdvertising = NimBLEDevice::getAdvertising();
  pAdvertising->setName(SNIFFER_BLE_NAME);

  // Полный 128-битный UUID сервиса (16 байт) + имя (15 символов) вместе
  // не влезают в стандартный 31-байтный рекламный пакет — включаем
  // scan response (отдельный дополнительный пакет с собственным лимитом
  // в 31 байт), куда NimBLE разнесёт данные, если не помещаются в одном
  pAdvertising->enableScanResponse(true);

  pAdvertising->addServiceUUID(SYNC_SERVICE_UUID);
  pAdvertising->start();

  Serial.println("BLE синхронизация запущена (" SNIFFER_BLE_NAME ")");
}

// =====================================================================
// SD
// =====================================================================
// Следующий свободный номер can_log_NNNN.txt в папке
// ============================================================================
// GzLog — потоковое gzip-сжатие лога "на лету", без внешних библиотек.
// Минимальное сжатие: LZ77 с одним кандидатом из хеш-таблицы + фиксированные
// коды Хаффмана (deflate BTYPE=01). Быстро и мало памяти (~45 КБ), а текстовый
// CAN-лог всё равно сжимается в разы — строки очень похожи друг на друга.
// Выход — стандартный .gz: распаковывается gzip/7-Zip/WinRAR/Python.
// sync() делает "sync flush": всё записанное до этого момента можно
// распаковать, даже если файл не будет закрыт штатно (без контрольной суммы
// в конце распаковщики ругаются на обрыв, но данные отдают).
// ============================================================================

static void* gzAlloc(size_t n) {
  void* p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return p ? p : malloc(n);
}

template <class OUT>   // OUT: size_t write(const uint8_t*, size_t)
class GzLog {
public:
  static const uint32_t WIN   = 16384;   // окно поиска повторов
  static const uint32_t CHUNK = 16384;   // сколько копим перед сжатием
  static const uint32_t HBITS = 12;      // хеш-таблица 4096 позиций
  static const uint32_t OBUF  = 4096;
  static const int      DEPTH = LOG_GZ_DEPTH;  // сколько кандидатов проверять (1 — минимум)

  bool begin(OUT* out, uint32_t mtime = 0) {
    if (!buf) {
      // ~130 КБ: крупные буферы — в PSRAM, если она есть
      buf  = (uint8_t*)gzAlloc(WIN + CHUNK);
      head = (uint32_t*)gzAlloc(sizeof(uint32_t) << HBITS);
      prev = (uint32_t*)gzAlloc(sizeof(uint32_t) * WIN);
      obuf = (uint8_t*)malloc(OBUF);
      if (!buf || !head || !prev || !obuf) return false;
      for (uint32_t n = 0; n < 256; n++) {           // таблица CRC32
        uint32_t c = n;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crcTab[n] = c;
      }
    }
    o = out; crc = 0xFFFFFFFFu; inSize = 0; outSize = 0;
    base = 0; hist = 0; pend = 0; bitBuf = 0; bitCnt = 0; olen = 0;
    memset(head, 0xFF, sizeof(uint32_t) << HBITS);   // 0xFFFFFFFF = пусто
    const uint8_t hdr[10] = {0x1F, 0x8B, 8, 0, (uint8_t)mtime, (uint8_t)(mtime >> 8),
                             (uint8_t)(mtime >> 16), (uint8_t)(mtime >> 24), 4, 0xFF};
    putBytes(hdr, 10);
    blockStart();
    return true;
  }

  // Добавить данные (строку лога)
  void write(const uint8_t* d, size_t n) {
    for (size_t i = 0; i < n; i++) crc = crcTab[(crc ^ d[i]) & 0xFF] ^ (crc >> 8);
    inSize += n;
    while (n) {
      size_t room = CHUNK - pend;
      size_t k = n < room ? n : room;
      memcpy(buf + hist + pend, d, k);
      pend += k; d += k; n -= k;
      if (pend == CHUNK) compressPending();
    }
  }

  // Sync flush: всё записанное доступно для распаковки
  void sync() {
    compressPending();
    putHuff(256);                        // конец текущего блока
    putBits(0, 1); putBits(0, 2);        // пустой stored-блок
    alignByte();
    const uint8_t s[4] = {0, 0, 0xFF, 0xFF};
    putBytes(s, 4);
    flushOut();
    blockStart();
  }

  // Завершить поток: финальный блок + CRC32 + размер
  void finish() {
    compressPending();
    putHuff(256);
    putBits(1, 1); putBits(1, 2); putHuff(256);   // пустой финальный блок
    alignByte();
    uint32_t c = crc ^ 0xFFFFFFFFu;
    const uint8_t t[8] = {(uint8_t)c, (uint8_t)(c >> 8), (uint8_t)(c >> 16), (uint8_t)(c >> 24),
                          (uint8_t)inSize, (uint8_t)(inSize >> 8), (uint8_t)(inSize >> 16), (uint8_t)(inSize >> 24)};
    putBytes(t, 8);
    flushOut();
  }

  uint32_t compressedBytes() const { return outSize + olen + pend / 4; }  // оценка с учётом несжатого хвоста
  uint32_t rawBytes() const { return inSize; }

private:
  OUT* o = nullptr;
  uint8_t* buf = nullptr; uint32_t* head = nullptr; uint32_t* prev = nullptr; uint8_t* obuf = nullptr;
  uint32_t crcTab[256];
  uint32_t crc = 0, inSize = 0, outSize = 0;
  uint32_t base = 0;    // абсолютная позиция buf[0]
  uint32_t hist = 0;    // байт истории в начале buf
  uint32_t pend = 0;    // несжатых байт после истории
  uint32_t bitBuf = 0; int bitCnt = 0; uint32_t olen = 0;

  static inline uint32_t hash3(const uint8_t* p) {
    return ((p[0] << 16 | p[1] << 8 | p[2]) * 2654435761u) >> (32 - HBITS);
  }

  void compressPending() {
    if (!pend) return;
    uint32_t i = hist, end = hist + pend;
    while (i < end) {
      uint32_t best = 0, dist = 0;
      if (end - i >= 3) {
        uint32_t h = hash3(buf + i);
        uint32_t cand = head[h];
        uint32_t absPos = base + i;
        head[h] = absPos;
        prev[absPos % WIN] = cand;
        uint32_t maxL = end - i; if (maxL > 258) maxL = 258;
        for (int t = 0; t < DEPTH && cand != 0xFFFFFFFFu && cand >= base && absPos - cand <= WIN - 1; t++) {
          const uint8_t* a = buf + (cand - base);
          const uint8_t* b = buf + i;
          uint32_t L = 0;
          while (L < maxL && a[L] == b[L]) L++;
          if (L > best) { best = L; dist = absPos - cand; if (L == maxL) break; }
          cand = prev[cand % WIN];
        }
        if (best < 3) best = 0;
      }
      if (best) {
        putLength(best); putDist(dist);
        // занести в хеш позиции внутри совпадения (дёшево и улучшает сжатие)
        for (uint32_t k = 1; k < best && i + k + 2 < end; k++) {
          uint32_t hh = hash3(buf + i + k);
          prev[(base + i + k) % WIN] = head[hh];
          head[hh] = base + i + k;
        }
        i += best;
      } else {
        putHuff(buf[i]); i++;
      }
    }
    // Сдвиг: оставить последние WIN байт как историю
    uint32_t total = hist + pend;
    uint32_t keep = total < WIN ? total : WIN;
    memmove(buf, buf + total - keep, keep);
    base += total - keep;
    hist = keep; pend = 0;
  }

  // ---- битовый вывод (LSB-first) ----
  inline void putBits(uint32_t v, int n) {
    bitBuf |= v << bitCnt; bitCnt += n;
    while (bitCnt >= 8) { putByte(bitBuf & 0xFF); bitBuf >>= 8; bitCnt -= 8; }
  }
  inline void putRev(uint32_t code, int n) {       // код Хаффмана: старшим битом вперёд
    uint32_t r = 0;
    for (int k = 0; k < n; k++) { r = (r << 1) | (code & 1); code >>= 1; }
    putBits(r, n);
  }
  void putHuff(uint32_t sym) {                      // фиксированные коды литералов/длин
    if (sym < 144)      putRev(0x30 + sym, 8);
    else if (sym < 256) putRev(0x190 + sym - 144, 9);
    else if (sym < 280) putRev(sym - 256, 7);
    else                putRev(0xC0 + sym - 280, 8);
  }
  void putLength(uint32_t L) {
    static const uint16_t lb[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
    static const uint8_t  le[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
    int c = 28; while (lb[c] > L) c--;
    putHuff(257 + c);
    if (le[c]) putBits(L - lb[c], le[c]);
  }
  void putDist(uint32_t D) {
    static const uint16_t db[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
    static const uint8_t  de[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
    int c = 29; while (db[c] > D) c--;
    putRev(c, 5);
    if (de[c]) putBits(D - db[c], de[c]);
  }
  void blockStart() { putBits(0, 1); putBits(1, 2); }   // не финальный, фиксированный Хаффман
  void alignByte() { if (bitCnt) { putByte(bitBuf & 0xFF); bitBuf = 0; bitCnt = 0; } }
  inline void putByte(uint8_t b) { obuf[olen++] = b; if (olen == OBUF) flushOut(); }
  void putBytes(const uint8_t* d, size_t n) { for (size_t i = 0; i < n; i++) putByte(d[i]); }
  void flushOut() { if (olen) { o->write(obuf, olen); outSize += olen; olen = 0; } }
};

#if LOG_COMPRESS == 1
GzLog<File> gzLog;
uint32_t gzLastSync = 0;
bool     gzDirty = false;   // есть данные после последнего sync
#endif

#if LOG_COMPRESS == 2
// ---------------------------------------------------------------------
// LZMA: кодер работает в отдельной задаче lzTask. sdTask кладёт строки в
// потоковый буфер lzSb, кодер забирает их (LzmaEnc_Encode сам "тянет"
// вход) и пишет сжатые данные в canLogFile. Файл, пока он открыт, пишет
// ТОЛЬКО lzTask; sdTask открывает его до старта и закрывает после
// завершения кодера (lzDoneSem).
// ---------------------------------------------------------------------
static StreamBufferHandle_t lzSb = nullptr;
static SemaphoreHandle_t   lzStartSem = nullptr, lzDoneSem = nullptr;
static volatile bool       lzEof = false;       // больше данных не будет
static volatile bool       lzFailed = false;    // кодер не смог стартовать
static volatile uint32_t   lzOutBytes = 0;      // сжатых байт в файле
static volatile uint32_t   lzRetries = 0;       // повторов записи, спасших файл
static volatile uint32_t   lzShortWrites = 0;   // сколько раз SD приняла меньше, чем просили
static volatile int        lzWriteErr = 0;      // код ошибки файла на момент сбоя записи
static uint32_t            lzLastFlush = 0;

static void* lzAllocF(ISzAllocPtr, size_t n) {
  void* p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);   // крупное — в PSRAM
  return p ? p : malloc(n);
}
static void lzFreeF(ISzAllocPtr, void* p) { free(p); }
static const ISzAlloc lzAlloc = { lzAllocF, lzFreeF };

// Вход кодера: ждём данные; 0 байт = конец потока (только после lzEof)
static SRes lzRead(ISeqInStreamPtr, void* buf, size_t* size) {
  size_t got = 0;
  while (!got) {
    got = xStreamBufferReceive(lzSb, buf, *size, pdMS_TO_TICKS(200));
    if (!got && lzEof && xStreamBufferIsEmpty(lzSb)) break;
  }
  *size = got;
  return SZ_OK;
}

// Выход кодера: в файл. Раз в секунду — flush (обновить размер в FAT)
static size_t lzWrite(ISeqOutStreamPtr, const void* buf, size_t size) {
  // Разовый сбой шины SD (помеха, просадка) переживаем повтором остатка
  size_t w = canLogFile.write((const uint8_t*)buf, size);
  for (int t = 0; t < 3 && w < size; t++) {
    lzRetries++;
    vTaskDelay(pdMS_TO_TICKS(20));
    w += canLogFile.write((const uint8_t*)buf + w, size - w);
  }
  if (w != size) {                    // SD не приняла данные: кодер вернёт SZ_ERROR_WRITE (9)
    lzShortWrites++;
    lzWriteErr = canLogFile.getWriteError();
  }
  lzOutBytes += w;
  logOutTotal += w;
  if (millis() - lzLastFlush >= 1000) { canLogFile.flush(); lzLastFlush = millis(); }
  return w;
}

static const ISeqInStream  lzIn  = { lzRead };
static const ISeqOutStream lzOut = { lzWrite };

void lzTask(void*) {
  for (;;) {
    xSemaphoreTake(lzStartSem, portMAX_DELAY);
    SRes r = SZ_ERROR_MEM;
    CLzmaEncHandle enc = LzmaEnc_Create(&lzAlloc);
    if (enc) {
      CLzmaEncProps pr;
      LzmaEncProps_Init(&pr);
      pr.level        = 1;               // быстрый режим (hash chain)
      pr.dictSize     = LOG_LZMA_DICT;
      pr.numHashBytes = 4;               // 5 по умолчанию — вдвое больше памяти
      pr.writeEndMark = 1;               // размер заранее неизвестен
      pr.numThreads   = 1;
      LzmaEncProps_Normalize(&pr);
      r = LzmaEnc_SetProps(enc, &pr);
      if (r == SZ_OK) {
        // Заголовок .lzma: 5 байт свойств + 8 байт размера (FF.. = неизвестен)
        Byte hdr[13];
        SizeT hs = 5;
        LzmaEnc_WriteProperties(enc, hdr, &hs);
        memset(hdr + 5, 0xFF, 8);
        lzWrite(&lzOut, hdr, 13);
        r = LzmaEnc_Encode(enc, &lzOut, &lzIn, NULL, &lzAlloc, &lzAlloc);
      }
      LzmaEnc_Destroy(enc, &lzAlloc, &lzAlloc);
    }
    if (r != SZ_OK) {
      lzFailed = true;
      if (r == SZ_ERROR_WRITE)
        errLog("SD: сбой записи в %s (LZMA код 9, writeErr=%d) — карта не приняла данные; пробую перемонтировать и открыть новый файл",
               currentLogName.c_str(), lzWriteErr);
      else if (r == SZ_ERROR_MEM)
        errLog("LZMA: не хватило памяти под кодер (код 2) — файл %s не пишется, пробую новый", currentLogName.c_str());
      else
        errLog("LZMA: ошибка кодера %d — файл %s не пишется, пробую новый", (int)r, currentLogName.c_str());
      // Выгребаем вход до конца, чтобы sdTask не повис на полном буфере
      uint8_t junk[256];
      while (!(lzEof && xStreamBufferIsEmpty(lzSb)))
        xStreamBufferReceive(lzSb, junk, sizeof(junk), pdMS_TO_TICKS(200));
    }
    xSemaphoreGive(lzDoneSem);
  }
}

void lzSetup() {
  lzSb       = xStreamBufferCreate(32 * 1024, 1);
  lzStartSem = xSemaphoreCreateBinary();
  lzDoneSem  = xSemaphoreCreateBinary();
  // Ядро 0: там же приём CAN (приоритет выше) и WiFi; ядро 1 остаётся вебу и SD
  xTaskCreatePinnedToCore(lzTask, "lzTask", 8192, NULL, 1, NULL, 0);
}

// Запустить кодер для только что открытого canLogFile
void lzStartFile() {
  xStreamBufferReset(lzSb);
  lzEof = false;
  lzFailed = false;
  lzWriteErr = 0;
  lzOutBytes = 0;
  lzLastFlush = millis();
  xSemaphoreGive(lzStartSem);
}
#endif

// Единые функции записи в лог: сжатие или простой текст
void logWrite(const char* d, size_t n) {
#if LOG_COMPRESS == 2
  if (!lzFailed) {
    // Если кодер не успевает, ждём (кадры пока копятся в canQueue)
    size_t sent = xStreamBufferSend(lzSb, d, n, pdMS_TO_TICKS(3000));
    if (sent < n) {
      lzFailed = true;
      errLog("LZMA: кодер не принимает данные 3 с (завис?) — запись файла %s остановлена", currentLogName.c_str());
    }
    size_t fill = xStreamBufferBytesAvailable(lzSb);
    if (fill > lzSbMax) lzSbMax = fill;
  }
#elif LOG_COMPRESS == 1
  gzLog.write((const uint8_t*)d, n);
  gzDirty = true;
#else
  canLogFile.write((const uint8_t*)d, n);
  logOutTotal += n;
#endif
  currentLogBytes += n;
  logInTotal += n;
}

// Размер файла для ротации: сжатый (что реально лежит на карте)
uint32_t logFileBytes() {
#if LOG_COMPRESS == 2
  return lzOutBytes;
#elif LOG_COMPRESS == 1
  return gzLog.compressedBytes();
#else
  return currentLogBytes;
#endif
}

// Протолкнуть данные на карту. Для LZMA — ничего: файлом владеет кодер,
// он сам сбрасывает данные на карту
void logSync() {
#if LOG_COMPRESS == 2
  return;
#else
#if LOG_COMPRESS == 1
  if (gzDirty) { gzLog.sync(); gzDirty = false; }
  gzLastSync = millis();
#endif
  canLogFile.flush();
#endif
}

// Закрыть файл штатно (для .gz / .lzma — с корректным концом потока)
void logClose() {
#if LOG_COMPRESS == 2
  lzEof = true;
  if (xSemaphoreTake(lzDoneSem, pdMS_TO_TICKS(15000)) != pdTRUE)
    errLog("LZMA: кодер не завершился за 15 с — файл %s закрыт как есть", currentLogName.c_str());
#elif LOG_COMPRESS == 1
  gzLog.finish();
  gzDirty = false;
#endif
  canLogFile.flush();
  canLogFile.close();
}

uint32_t nextLogIndex(const String& folder) {
  uint32_t maxIdx = 0;
  File dir = LOGFS.open(folder);
  if (!dir || !dir.isDirectory()) return 1;
  File e = dir.openNextFile();
  while (e) {
    String n = String(e.name());
    if (n.lastIndexOf('/') != -1) n = n.substring(n.lastIndexOf('/') + 1);
    if (n.startsWith("can_log_") && (n.endsWith(".txt") || n.endsWith(".txt.gz") || n.endsWith(".txt.lzma"))) {
      uint32_t idx = (uint32_t)n.substring(8, n.indexOf('.')).toInt();
      if (idx > maxIdx) maxIdx = idx;
    }
    e.close();
    e = dir.openNextFile();
  }
  dir.close();
  return maxIdx + 1;
}

// Открыть новый файл лога (папка по текущей дате RTC). prevName — имя
// предыдущего файла при ротации (пусто при старте) для строки-маркера.
bool openNewLogFile(const String& prevName) {
  currentDateFolder = dateFolderName(nowTime());
  if (!LOGFS.exists(currentDateFolder)) LOGFS.mkdir(currentDateFolder);

  char name[28];
  snprintf(name, sizeof(name), LOG_COMPRESS == 2 ? "can_log_%04lu.txt.lzma" :
                               LOG_COMPRESS == 1 ? "can_log_%04lu.txt.gz" : "can_log_%04lu.txt",
           (unsigned long)nextLogIndex(currentDateFolder));
  currentLogName = name;
  String logPath = currentDateFolder + "/" + currentLogName;

  canLogFile = LOGFS.open(logPath, FILE_WRITE);
  currentLogBytes = 0;
  if (canLogFile) {
    setActiveLogFolder(currentDateFolder.c_str());
    setActiveLogPath(logPath.c_str());
  }
  if (!canLogFile) {
    errLog("SD: не удалось открыть %s", logPath.c_str());
    return false;
  }

#if LOG_COMPRESS == 2
  lzStartFile();
#elif LOG_COMPRESS == 1
  if (!gzLog.begin(&canLogFile, (uint32_t)time(nullptr))) {
    Serial.println("Нет памяти под буферы сжатия — лог не открыт");
    canLogFile.close();
    return false;
  }
  gzLastSync = millis();
#endif

  // Каждый файл самодостаточен: в первой строке версия и скорость шины
  char hdr[200];
  int n;
  if (prevName.length() == 0) {
    n = snprintf(hdr, sizeof(hdr), "# %lu ===== BOOT / SYNC MARK ===== fw=%s build=\"%s\" reset=%s can_bps=%lu time=\"%s\" rtc=%s\n",
                          (unsigned long)millis(), FW_VERSION, FW_BUILD,
                          resetReasonStr(esp_reset_reason()), (unsigned long)canBitrate,
                          timeValid ? timeSource : "none", rtcName());
  } else {
    n = snprintf(hdr, sizeof(hdr), "# %lu ===== CONTINUED from %s ===== fw=%s can_bps=%lu\n",
                          (unsigned long)millis(), prevName.c_str(), FW_VERSION,
                          (unsigned long)canBitrate);
  }
  if (n > 0) logWrite(hdr, n);
  logSync();
  Serial.println("Лог пишется в: " + logPath);
  return true;
}

// Форматирование карты, которая не монтируется (новые карты от 64 ГБ
// идут в exFAT — ядро Arduino-ESP32 его не поддерживает). Смонтированную
// карту НЕ форматируем: её содержимое удаляется кнопкой на странице логов.
bool sdFormatAndMount() {
  SD.end();
  SPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
  // format_if_empty = true: если на карте нет FAT (exFAT, пустая) —
  // FatFs размечает её сам, для больших карт получается FAT32
  bool ok = SD.begin(SD_CS_PIN, SPI, SD_SPI_FREQ_SAFE, "/sd", 5, true);
  if (ok) {
    sdMounted = true;
    Serial.printf("SD-карта отформатирована и смонтирована, %llu МБ\n",
                  LOGFS.cardSize() / (1024ULL * 1024ULL));
  }
  return ok;
}

void handleSdFormat() {
  PortalSdGuard portalGuard;
  // Разрушительная операция — под тем же паролем, что и прошивка
  if (!webServer.authenticate(OTA_WEB_USER, OTA_PASSWORD)) {
    return webServer.requestAuthentication(BASIC_AUTH, "CAN Sniffer OTA");
  }
  String body;
  if (sdMounted) {
    body = "<h2>" + TR("Форматирование не требуется", "No formatting needed") + "</h2><p>" +
           TR("Карта смонтирована и работает. Удалить содержимое можно на странице логов.",
              "The card is mounted and working. Delete its contents on the logs page.") + "</p>";
  } else {
    Serial.println("Портал: форматирование SD-карты...");
    sdBusy = true;
    uint32_t t0 = millis();
    bool ok = sdFormatAndMount();
    sdBusy = false;
    Serial.printf("Форматирование SD: %s за %lu мс\n", ok ? "OK" : "ОШИБКА", (unsigned long)(millis() - t0));
    body = ok ? "<h2>" + TR("Карта отформатирована", "Card formatted") + "</h2><p>" +
                TR("Файловая система FAT32 создана, запись логов начнётся с первыми кадрами CAN.",
                   "FAT32 created, logging will start with the first CAN frames.") + "</p>"
              : "<h2>" + TR("Не удалось", "Failed") + "</h2><p>" +
                TR("Карта не отвечает или неисправна. Проверьте, что она вставлена, или отформатируйте её в FAT32 на компьютере (guiformat, Rufus).",
                   "The card does not respond or is faulty. Check that it is inserted, or format it as FAT32 on a computer (guiformat, Rufus).") + "</p>";
  }
  touchWeb();
  webServer.send(200, "text/html; charset=utf-8",
    htmlHead(TR("Форматирование SD", "SD format")) + body +
    "<p><a href='/'>&larr; " + TR("на главную", "home") + "</a></p></body></html>");
}

void setupSD() {
  SPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
  // По умолчанию SD.begin() работает на 4 МГц — это ~400 КБ/с чтения,
  // скачивание больших логов упиралось в SD. Пробуем быструю частоту,
  // если карта/проводка не тянет — откатываемся на медленную.
  uint32_t sdFreq = SD_SPI_FREQ_FAST;
  if (!SD.begin(SD_CS_PIN, SPI, sdFreq)) {
    Serial.printf("SD на %lu МГц не завелась — пробую %lu МГц\n",
                  (unsigned long)(SD_SPI_FREQ_FAST / 1000000), (unsigned long)(SD_SPI_FREQ_SAFE / 1000000));
    SD.end();
    sdFreq = SD_SPI_FREQ_SAFE;
    if (!SD.begin(SD_CS_PIN, SPI, sdFreq)) {
      Serial.println("Ошибка инициализации SD-карты!");
      return;
    }
  }
  Serial.printf("SD-карта инициализирована (SPI %lu МГц)\n", (unsigned long)(sdFreq / 1000000));
  sdMounted = true;

  // Файл НЕ создаётся при старте: откроется при первом реальном кадре CAN.
  // Нет трафика на шине (стенд, машина без зажигания) — нет пустых файлов.
  Serial.println("Лог: жду первый кадр CAN — файл будет создан при появлении данных");
}

// Перемонтировать SD после сбоя записи (вибрация, просадка питания, контакт)
static bool sdRemount(uint32_t hz = SD_FREQ_LOG) {
  sdMounted = false;
  SD.end();
  delay(100);
  SPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
  bool ok = SD.begin(SD_CS_PIN, SPI, hz);
  sdMounted = ok;
  return ok;
}

// =====================================================================
// canTask — только приём
// =====================================================================
// Фильтр ACL для одного кадра (вызывается из canTask). Первое совпавшее
// правило решает; ни одно не совпало — кадр не отправляется.
void aclProcess(const twai_message_t& m) {
  AclSet* set = aclCur;
  bool ext = m.extd;
  const AclRule* hit = nullptr;
  for (uint8_t i = 0; i < set->n; i++) {
    const AclRule& r = set->r[i];
    if (!(r.flags & ACL_ANYFMT) && (bool)(r.flags & ACL_EXT) != ext) continue;
    if (((m.identifier ^ r.id) & r.mask) == 0) { hit = &r; break; }
  }
  if (!hit || !(hit->flags & ACL_PERMIT)) return;

  uint8_t dlc = m.data_length_code > 8 ? 8 : m.data_length_code;
  if ((hit->flags & ACL_ONCHANGE) || hit->minIntervalMs) {
    uint32_t key = m.identifier | (ext ? 0x80000000u : 0) | 0x40000000u;   // 0 = пустой слот
    uint32_t h = (key * 2654435761u) >> 24;                               // 0..255
    AclState* st = nullptr;
    for (int p = 0; p < 8; p++) {                                         // короткое пробирование
      AclState& c = aclState[(h + p) & (ACL_STATE_SLOTS - 1)];
      if (c.key == key || c.key == 0) { st = &c; break; }
    }
    if (st) {
      uint32_t now = millis();
      bool fresh = st->key == 0;
      if (!fresh) {
        if ((hit->flags & ACL_ONCHANGE) && st->dlc == dlc && memcmp(st->data, m.data, dlc) == 0) return;
        if (hit->minIntervalMs && now - st->lastMs < hit->minIntervalMs) return;
      }
      st->key = key; st->lastMs = now; st->dlc = dlc;
      memcpy(st->data, m.data, dlc);
    }
  }

  AclFrame f;
  f.ts  = (uint16_t)millis();
  f.idf = m.identifier | (ext ? 0x80000000u : 0) | (m.rtr ? 0x40000000u : 0);
  f.dlc = m.rtr ? 0 : dlc;
  memcpy(f.data, m.data, f.dlc);
  if (xQueueSend(bleQueue, &f, 0) != pdTRUE) { bleDroppedTotal++; bleDropFlag = true; }
}

// Отправка по BLE пачками: [кол-во][флаги] + кадры {ts16, id32, dlc, data}.
// Пачка уходит, когда заполнена до MTU или прошло ACL_BATCH_MS с первого кадра.
void bleTxTask(void*) {
  static uint8_t buf[512];
  size_t len = 2;
  uint8_t cnt = 0;
  uint32_t first = 0;
  auto flush = [&]() {
    if (!cnt) return;
    buf[0] = cnt;
    buf[1] = bleDropFlag ? 0x01 : 0x00;     // бит 0: были потери с прошлой пачки
    bleDropFlag = false;
    framesCharacteristic->setValue(buf, len);
    framesCharacteristic->notify();
    bleSentTotal += cnt;
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
      memcpy(buf + len, &f.ts, 2);
      memcpy(buf + len + 2, &f.idf, 4);
      buf[len + 6] = f.dlc;
      memcpy(buf + len + 7, f.data, f.dlc);
      len += need; cnt++;
    }
    if (cnt && (!framesSubscribed || millis() - first >= ACL_BATCH_MS)) {
      if (framesSubscribed) flush(); else { len = 2; cnt = 0; }
    }
  }
}

void canTask(void* param) {
#if CAN_LISTEN_ONLY
  twai_general_config_t g_config =
      TWAI_GENERAL_CONFIG_DEFAULT(TWAI_TX_PIN, TWAI_RX_PIN, TWAI_MODE_LISTEN_ONLY);
#else
  twai_general_config_t g_config =
      TWAI_GENERAL_CONFIG_DEFAULT(TWAI_TX_PIN, TWAI_RX_PIN, TWAI_MODE_NORMAL);
#endif

  twai_timing_config_t t_config = canTimingFor(canBitrate);
  twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

  esp_err_t err = twai_driver_install(&g_config, &t_config, &f_config);
  if (err == ESP_OK) err = twai_start();
  if (err != ESP_OK) {
    canRunning = false;
    errLog("CAN: ОШИБКА запуска TWAI (%s) на %s — приём CAN не работает",
                  esp_err_to_name(err), canBitrateLabelRu(canBitrate));
    vTaskDelete(NULL);
  }
  Serial.printf("[CAN task] TWAI запущен, режим: %s, скорость: %s\n",
                CAN_LISTEN_ONLY ? "LISTEN_ONLY (машина)" : "NORMAL (стенд)",
                canBitrateLabelRu(canBitrate));

  twai_message_t message;
  for (;;) {
    if (twai_receive(&message, pdMS_TO_TICKS(20)) == ESP_OK) {
      CanLogEntry entry;
      entry.timestamp = millis();
      entry.id = message.identifier;
      entry.dlc = message.data_length_code > 8 ? 8 : message.data_length_code;
      entry.flags = (message.extd ? LOGF_EXTD : 0) | (message.rtr ? LOGF_RTR : 0);
      memset(entry.data, 0, sizeof(entry.data));
      if (!message.rtr) memcpy(entry.data, message.data, entry.dlc);
      if (xQueueSend(canQueue, &entry, 0) != pdTRUE) canDroppedTotal++;
      canFramesTotal++;
      UBaseType_t qw = uxQueueMessagesWaiting(canQueue);
      if (qw > canQueueMax) canQueueMax = qw;
      lastCanFrameMs = millis();

      // Живая трансляция сырых данных наблюдаемого ID по BLE — отдельно
      // от лога, лёгкая операция (notify() асинхронно уходит в BLE-стек,
      // не блокирует приём следующих кадров). Если клиент не подписан —
      // notify() просто ничего не сделает, безопасно вызывать всегда.
      if (framesSubscribed && aclCur->n) aclProcess(message);

      if (watchedCanId != 0 && message.identifier == watchedCanId) {
        WatchedFramePacket packet;
        packet.id = message.identifier;
        packet.dlc = message.data_length_code;
        memset(packet.data, 0, sizeof(packet.data));
        memcpy(packet.data, message.data, packet.dlc);
        watchDataCharacteristic->setValue((uint8_t*)&packet, sizeof(packet));
        watchDataCharacteristic->notify();
      }
    }
  }
}

// =====================================================================
// sdTask — запись на SD
// =====================================================================
// ---------- Состояние записи (только для sdTask) ----------
// Ленивое открытие файла:
//   - первый файл сессии открывается на первом кадре (маркер BOOT);
//   - после ротации следующий открывается на следующем кадре (CONTINUED).
static bool     logSessionStarted = false;  // был ли уже файл в этом запуске
static String   logPendingPrev;             // имя предыдущего файла для CONTINUED
static uint32_t logLastOpenFail = 0;        // не долбить SD при ошибке открытия
static bool     logOpenFailed = false;
static uint32_t logSinceFlush = 0;
static bool     logNeedRemount = false;     // после сбоя записи: SD перемонтировать
static uint32_t logFailAt = 0;              // когда случился сбой (для отчёта о потере)
static uint8_t  logRecoverTries = 0;

// Записать один кадр: при необходимости открыть файл, записать строку,
// сделать ротацию. false — файл открыть не удалось (кадр потерян).
bool logWriteFrame(const CanLogEntry& entry) {
#if LOG_COMPRESS == 2
  // Кодер упал (обычно — SD не приняла запись): закрываем файл и уходим
  // на перемонтирование + новый файл. Без перезагрузки и без зажигания.
  if (lzFailed && canLogFile) {
    String prev = currentLogName;
    logClose();
    setActiveLogPath("");
    logPendingPrev = prev;
    logSinceFlush = 0;
    logNeedRemount = true;
    logFailAt = millis();
    logRecoverTries = 0;
    logLastOpenFail = 0;
  }
  if (logNeedRemount && !canLogFile) {
    if (sdBusy) return false;
    if (logLastOpenFail && millis() - logLastOpenFail < 5000) return false;   // не чаще раза в 5 с
    logLastOpenFail = millis() ? millis() : 1;
    bool ok = sdRemount();
    if (!ok) {
      if (logRecoverTries++ == 0) errLog("SD: перемонтирование не удалось, повторяю каждые 5 с");
      return false;
    }
    logNeedRemount = false;
    logOpenFailed = false;
    logLastOpenFail = 0;
    errLog("SD: перемонтирована, запись возобновляется (простой %lu мс, попыток %u)",
           (unsigned long)(millis() - logFailAt), (unsigned)(logRecoverTries + 1));
  }
#endif
  if (!canLogFile) {
    if (sdBusy || !sdMounted) return false;   // карта форматируется / не смонтирована
    if (logOpenFailed && millis() - logLastOpenFail < 5000) return false;   // SD недоступна
    if (openNewLogFile(logSessionStarted ? logPendingPrev : String(""))) {
      logSessionStarted = true;
      logOpenFailed = false;
      logPendingPrev = "";
      logSinceFlush = 0;
    } else {
      logOpenFailed = true;
      logLastOpenFail = millis();
      return false;
    }
  }

  // <millis> <S|X> <ID> [R]<DLC> <байты> — без String, чтобы не
  // фрагментировать кучу на плотном трафике
  char line[64];
  bool extd = entry.flags & LOGF_EXTD;
  bool rtr  = entry.flags & LOGF_RTR;
  int n = extd
    ? snprintf(line, sizeof(line), "%lu X %08lX %s%u",
               (unsigned long)entry.timestamp, (unsigned long)entry.id,
               rtr ? "R" : "", entry.dlc)
    : snprintf(line, sizeof(line), "%lu S %03lX %s%u",
               (unsigned long)entry.timestamp, (unsigned long)entry.id,
               rtr ? "R" : "", entry.dlc);
  if (!rtr) {
    for (int i = 0; i < entry.dlc && n < (int)sizeof(line) - 4; i++) {
      n += snprintf(line + n, sizeof(line) - n, " %02X", entry.data[i]);
    }
  }

  line[n++] = '\r'; line[n++] = '\n';   // как println (буфер с запасом)
  logWrite(line, n);
#if LOG_COMPRESS == 1
  if (millis() - gzLastSync >= LOG_GZ_SYNC_MS) logSync();
#elif LOG_COMPRESS == 0
  if (++logSinceFlush >= 10) {
    logSync();
    logSinceFlush = 0;
  }
#endif

  // Ротация: файл дорос до лимита — закрываем; следующий откроется на
  // следующем кадре. Занимает единицы мс, кадры ждут в canQueue.
  if (logFileBytes() >= LOG_MAX_BYTES) {
    String prev = currentLogName;
    char m[80];
    int k = snprintf(m, sizeof(m), "# %lu ===== ROTATE (%lu bytes) =====\n",
                     (unsigned long)millis(), (unsigned long)currentLogBytes);
    logWrite(m, k);
    logClose();
    logSinceFlush = 0;
    logPendingPrev = prev;
    setActiveLogPath("");
  }
  return true;
}

#if LOG_PAUSE_WHILE_PORTAL
// Портал в работе: закрыть файл, перевести SD на скорость портала, кадры не писать.
// Освободился: вернуть SD на скорость записи, дальше лог идёт новым файлом (CONTINUED).
// true — запись сейчас остановлена (текущий кадр не сохранять).
static bool portalPauseStep(bool isFrame) {
  static bool     paused = false;
  static uint32_t skipped = 0;
  if (logPortalPause() && !sdBusy) {
    if (!paused) {
      paused = true;
      skipped = 0;
      if (canLogFile) {
        String prev = currentLogName;
        char m[96];
        int k = snprintf(m, sizeof(m), "# %lu ===== PAUSE (portal) ===== dropped=%lu\n",
                         (unsigned long)millis(), (unsigned long)canDroppedTotal);
        logWrite(m, k);
        logClose();
        logSinceFlush = 0;
        logPendingPrev = prev;
      }
      setActiveLogPath("");
      setActiveLogFolder("");
      if (sdMounted && SD_FREQ_PORTAL != SD_FREQ_LOG) {
        if (!sdRemount(SD_FREQ_PORTAL)) {
          errLog("SD: на скорости портала не заработала, возвращаю скорость записи");
          sdRemount(SD_FREQ_LOG);
        }
      }
      sdPortalMode = true;
      Serial.println("[SD task] Портал в работе — запись лога приостановлена, SD на скорости портала");
    }
    if (isFrame) skipped++;
    return true;
  }
  if (paused) {
    paused = false;
    sdPortalMode = false;
    if (SD_FREQ_PORTAL != SD_FREQ_LOG) {
      if (!sdRemount(SD_FREQ_LOG)) logNeedRemount = true;   // дальше — обычное восстановление
    }
    Serial.printf("[SD task] Портал свободен — запись продолжается (пропущено кадров: %lu)\n",
                  (unsigned long)skipped);
  }
  return false;
}
#endif

void sdTask(void* param) {
  CanLogEntry entry;
#if CAN_TIME_SYNC
  // Пока время неизвестно, первые кадры держим в памяти (на WROOM — в PSRAM),
  // ждём кадр 0x6B2 и только потом открываем файл — сразу в папке по дате
  const size_t PENDING_MAX = 8192;
  std::vector<CanLogEntry> pending;
  bool     timeWaiting = false, timeWaitDone = false;
  uint32_t timeWaitStart = 0;
#endif

  for (;;) {
    if (xQueueReceive(canQueue, &entry, pdMS_TO_TICKS(500)) != pdTRUE) {
      // Шина замолчала — не держать данные в буфере сжатия
#if LOG_PAUSE_WHILE_PORTAL
      if (portalPauseStep(false)) continue;
#endif
      if (canLogFile) logSync();
      continue;
    }

    // Запрос портала: закрыть текущий файл перед скачиванием. Кадры до
    // запроса попадают в закрываемый файл (очередь упорядочена), после — в новый
    if (entry.id == ROTATE_SENTINEL_ID) {
      if (canLogFile) {
        String prev = currentLogName;
        char m[96];
        int k = snprintf(m, sizeof(m), "# %lu ===== ROTATE (download) ===== dropped=%lu\n",
                         (unsigned long)millis(), (unsigned long)canDroppedTotal);
        logWrite(m, k);
        logClose();
        logSinceFlush = 0;
        logPendingPrev = prev;          // следующий файл начнётся с CONTINUED
        setActiveLogPath("");
        Serial.printf("[SD task] %s закрыт для скачивания\n", prev.c_str());
      }
      xSemaphoreGive(rotateDoneSemaphore);
      continue;
    }

    // Сигнал на выключение — обрабатываем ОТДЕЛЬНО и ПЕРВЫМ, не пытаясь
    // собрать из него строку лога (dlc/data тут не valid CAN-данные)
    if (entry.id == SHUTDOWN_SENTINEL_ID) {
      const char* why = (entry.dlc == CLOSE_REASON_OTA) ? "OTA REBOOT"
                      : (entry.dlc == CLOSE_REASON_CFG) ? "CONFIG REBOOT"
                      : "ACC OFF / SHUTDOWN";
#if CAN_TIME_SYNC
      // Выключаемся, так и не дождавшись времени — сохраняем, что накопили
      for (const CanLogEntry& e : pending) logWriteFrame(e);
      pending.clear();
#endif
      if (canLogFile) {
        char m[80];
        int k = snprintf(m, sizeof(m), "# %lu ===== %s ===== dropped=%lu\n", (unsigned long)millis(), why,
                         (unsigned long)canDroppedTotal);
        logWrite(m, k);
        logClose();
      }
      setActiveLogFolder("");
      setActiveLogPath("");
      if (logSessionStarted) Serial.printf("[SD task] Лог закрыт штатно (%s)\n", why);
      else Serial.printf("[SD task] Данных CAN не было — файл не создавался (%s)\n", why);
      xSemaphoreGive(shutdownDoneSemaphore);
      continue;
    }

    canTimeFrame(entry);   // время из CAN: выставит часы, если нужно

#if LOG_PAUSE_WHILE_PORTAL
    if (portalPauseStep(true)) continue;
#endif

#if CAN_TIME_SYNC
    if (!timeWaitDone && !canLogFile) {
      if (!timeValid) {
        if (!timeWaiting) {
          timeWaiting = true;
          timeWaitStart = millis();
          Serial.println("[SD task] Пошли данные CAN, жду кадр времени 0x6B2 перед открытием лога");
        }
        pending.push_back(entry);
        if (millis() - timeWaitStart < CAN_TIME_WAIT_MS && pending.size() < PENDING_MAX) continue;
        errLog("Время: кадр 0x6B2 не пришёл за %d мс — лог пойдёт в /no-rtc", CAN_TIME_WAIT_MS);
      } else if (timeWaiting) {
        pending.push_back(entry);          // время только что пришло — текущий кадр тоже в пачку
      }
      timeWaitDone = true;
      if (!pending.empty()) {
        for (const CanLogEntry& e : pending) logWriteFrame(e);
        Serial.printf("[SD task] Записано %u кадров, накопленных до получения времени\n",
                      (unsigned)pending.size());
        pending.clear();
        pending.shrink_to_fit();
        continue;
      }
    }
#endif

    logWriteFrame(entry);
  }
}

// ---------- USB-хост / сон ----------

// Подключён ли USB-хост прямо сейчас (хост шлёт SOF каждую 1 мс).
// От зарядки без данных — false.
bool usbHostConnected() {
#if USB_HOST_KEEPS_AWAKE
  return Serial.isPlugged();
#else
  return false;
#endif
}

// Для ранней проверки после пробуждения: USB заново проходит
// энумерацию, SOF появляются не сразу — даём ему время
bool waitForUsbHost(uint32_t timeoutMs) {
#if USB_HOST_KEEPS_AWAKE
  uint32_t t0 = millis();
  while (millis() - t0 < timeoutMs) {
    if (Serial.isPlugged()) return true;
    delay(20);
  }
#endif
  return false;
}

// Настройка пробуждения по ACC. Подтяжка — в RTC-домене, т.к. обычный
// INPUT_PULLUP в deep sleep отключается вместе с цифровым доменом.
void armAccWakeupAndSleep() {
  rtc_gpio_init(ACC_PIN);
  rtc_gpio_set_direction(ACC_PIN, RTC_GPIO_MODE_INPUT_ONLY);
  if (ACC_OFF_LEVEL == HIGH) {
    rtc_gpio_pulldown_dis(ACC_PIN);
    rtc_gpio_pullup_en(ACC_PIN);
  } else {
    rtc_gpio_pullup_dis(ACC_PIN);
    rtc_gpio_pulldown_en(ACC_PIN);
  }
  // RTC-периферию держим включённой во сне — на ней живёт подтяжка
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
  esp_sleep_enable_ext0_wakeup(ACC_PIN, ACC_ON_LEVEL == LOW ? 0 : 1);
  esp_deep_sleep_start();
  // Дальше код не выполняется
}

const char* resetReasonStr(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:   return "POWERON";   // подача питания
    case ESP_RST_EXT:       return "EXT";       // внешний сброс
    case ESP_RST_SW:        return "SW";        // ESP.restart() (в т.ч. после OTA)
    case ESP_RST_PANIC:     return "PANIC";     // исключение / abort
    case ESP_RST_INT_WDT:   return "INT_WDT";
    case ESP_RST_TASK_WDT:  return "TASK_WDT";
    case ESP_RST_WDT:       return "WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP"; // пробуждение из deep sleep
    case ESP_RST_BROWNOUT:  return "BROWNOUT";  // просадка питания
    case ESP_RST_SDIO:      return "SDIO";
    case ESP_RST_USB:       return "USB";       // сброс через USB (прошивка/монитор)
    case ESP_RST_JTAG:      return "JTAG";
    default:                return "UNKNOWN";
  }
}

// ---------- Задача светодиода состояния ----------
#if RGB_LED_ENABLE
void ledWrite(uint8_t r, uint8_t g, uint8_t b) {
  rgbLedWrite(RGB_LED_PIN, r, g, b);
}

void ledOff() {
  ledStopped = true;
  delay(RGB_LED_FLASH_MS + 20);   // дать задаче закончить текущую вспышку
  ledWrite(0, 0, 0);
}

// Отдельная задача, а не loop(): loop бывает занят надолго (выгрузка
// логов, удаление папок), а индикация должна идти ровно
void ledTask(void* param) {
  const uint8_t B = RGB_LED_BRIGHTNESS;
  for (;;) {
    // Сброс сопряжений BLE: фиолетовое мигание — держите BOOT; белое — сброшено
    if (bleResetHolding || (bleResetDoneAt && millis() - bleResetDoneAt < 1500)) {
      bool done = !bleResetHolding;
      if (!ledStopped) ledWrite(B, done ? B : 0, B);
      vTaskDelay(pdMS_TO_TICKS(100));
      if (!ledStopped) ledWrite(0, 0, 0);
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }
    uint32_t used = 0;
    if (!ledStopped) {
#if LOG_COMPRESS == 2
      bool error   = !sdMounted || !canRunning || logOpenFailed || lzFailed;
#else
      bool error   = !sdMounted || !canRunning || logOpenFailed;
#endif
      bool partial = !timeValid || rtcOscStopped;
      bool portal  = WiFi.softAPgetStationNum() > 0;
      bool canData = lastCanFrameMs != 0 && (millis() - lastCanFrameMs) < 1000;

      // 1) Состояние
      if (error)        ledWrite(B, 0, 0);        // красный
      else if (partial) ledWrite(B, B * 3 / 4, 0);// жёлтый
      else if (portal)  ledWrite(0, 0, B);        // синий
      else              ledWrite(0, B, 0);        // зелёный
      vTaskDelay(pdMS_TO_TICKS(RGB_LED_FLASH_MS));
      if (!ledStopped) ledWrite(0, 0, 0);
      used = RGB_LED_FLASH_MS;

      // 2) Активность шины
      if (canData && !ledStopped) {
        vTaskDelay(pdMS_TO_TICKS(RGB_LED_GAP_MS));
        if (!ledStopped) ledWrite(B, B, B);       // белый
        vTaskDelay(pdMS_TO_TICKS(RGB_LED_FLASH_MS / 2));
        if (!ledStopped) ledWrite(0, 0, 0);
        used += RGB_LED_GAP_MS + RGB_LED_FLASH_MS / 2;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(RGB_LED_PERIOD_MS > used ? RGB_LED_PERIOD_MS - used : 100));
  }
}
#else
void ledOff() {}
#endif

void setup() {
  Serial.begin(115200);
  Serial.printf("\n=== S3 CAN Sniffer, прошивка %s (сборка %s), сброс: %s ===\n",
                FW_VERSION, FW_BUILD, resetReasonStr(esp_reset_reason()));

  // После пробуждения пин остаётся в RTC-режиме — возвращаем в обычный GPIO
  rtc_gpio_deinit(ACC_PIN);
  pinMode(ACC_PIN, INPUT_PULLUP);
  delay(5); // дать подтяжке установиться перед чтением

  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();

  // Ранняя проверка — если ACC выключено и комп не подключён,
  // не тратим время на полную инициализацию SD/WiFi/BLE, сразу в сон.
  // Ожидание USB здесь же служит и паузой для Serial-монитора.
  bool usbHost = waitForUsbHost(USB_ENUM_WAIT_MS);
  if (digitalRead(ACC_PIN) == ACC_OFF_LEVEL && !usbHost) {
    if (cause == ESP_SLEEP_WAKEUP_EXT0) {
      Serial.println("Проснулись, но ACC уже выключено — сразу обратно в сон");
    } else {
      Serial.println("Старт при выключенном ACC, USB-хоста нет — ухожу в сон");
    }
    delay(20);
    armAccWakeupAndSleep();
  }

  if (usbHost) {
    Serial.println("USB-хост подключён — сон заблокирован, пока он подключён");
  }

  // Очередь кадров: запас на задержки записи SD и на ожидание времени
  // из CAN перед открытием первого файла (~2 с трафика I-CAN)
  canQueue = xQueueCreate(CAN_QUEUE_LEN, sizeof(CanLogEntry));
  shutdownDoneSemaphore = xSemaphoreCreateBinary();
  rotateDoneSemaphore   = xSemaphoreCreateBinary();

  setupRTC();          // сначала RTC — дата нужна для имени папки на SD
  loadUiLanguage();
  loadCanBitrate();   // до setupSD — скорость попадает в маркер BOOT в логе
  setupSD();
  errFlushPending();
  {
    esp_reset_reason_t rr = esp_reset_reason();
    if (rr == ESP_RST_PANIC || rr == ESP_RST_INT_WDT || rr == ESP_RST_TASK_WDT ||
        rr == ESP_RST_WDT || rr == ESP_RST_BROWNOUT || rr == ESP_RST_UNKNOWN)
      errLog("Перезагрузка по сбою: %s", resetReasonStr(rr));
  }
  setupWiFiAndWebServer();
  setupBLESync();

  xTaskCreatePinnedToCore(canTask, "canTask", 4096, NULL, 2, NULL, 0);
#if LOG_COMPRESS == 2
  lzSetup();
#endif
  xTaskCreatePinnedToCore(sdTask,  "sdTask",  8192, NULL, 1, NULL, 1);
#if RGB_LED_ENABLE
  ledWrite(0, 0, 0);
  xTaskCreatePinnedToCore(ledTask, "ledTask", 3072, NULL, 1, NULL, 1);
#endif

  Serial.printf("Состояние ACC при старте: %s\n",
                digitalRead(ACC_PIN) == ACC_ON_LEVEL ? "включено" : "выключено");
}

// Штатно закрыть лог на SD (перед сном или перезагрузкой после прошивки).
// Просим sdTask закрыть файл, ждём подтверждения с таймаутом — на случай,
// если задача почему-то зависла, не хотим застрять тут навсегда
void closeLogForRestart(uint8_t reason) {
  CanLogEntry sentinel;
  memset(&sentinel, 0, sizeof(sentinel));
  sentinel.id  = SHUTDOWN_SENTINEL_ID;
  sentinel.dlc = reason;
  xQueueSend(canQueue, &sentinel, pdMS_TO_TICKS(200));
  xSemaphoreTake(shutdownDoneSemaphore, pdMS_TO_TICKS(2000));
  LOGFS.end();
}

// Выключение WiFi и всего, что поверх него (веб-сервер, mDNS, OTA).
// Порядок важен: сначала сервисы, потом сам WiFi — один раз. Двойная
// остановка давала гонку в обработчике событий и безвредное
// "wifi_init_default: netstack cb reg failed" в логе.
bool wifiOn = true;
void wifiShutdown() {
  if (!wifiOn) return;
  wifiOn = false;
  webServer.stop();
  ArduinoOTA.end();
  MDNS.end();
  WiFi.mode(WIFI_OFF);
  delay(50);
}

// Автовыключение WiFi через WIFI_ACTIVE_MINUTES после старта
void wifiAutoOffLoop() {
#if WIFI_ACTIVE_MINUTES > 0
  if (!wifiOn || otaInProgress) return;
  if (millis() < (uint32_t)WIFI_ACTIVE_MINUTES * 60000UL) return;
  if (webPortalActive()) return;                 // портал в работе — ждём
  Serial.printf("WiFi: прошло %d мин, портал не используется — точка доступа выключена "
                "до следующего включения зажигания\n", WIFI_ACTIVE_MINUTES);
  wifiShutdown();
#endif
}

void goToSleepUntilAccOn() {
  Serial.println("ACC выключено — завершаю запись и ухожу в сон...");
  ledOff();
  rtcTimeValidBeforeSleep = timeValid;   // системные часы продолжат идти во сне
  closeLogForRestart(CLOSE_REASON_SLEEP);

  // Глушим радио перед сном — незачем тратить остатки энергии на
  // рекламу BLE/WiFi, когда всё равно никто уже не подключится
  // Порядок важен: сначала сервисы поверх сети, потом сам WiFi —
  // один раз. Двойная остановка (softAPdisconnect(true) + mode(OFF))
  // давала гонку в обработчике событий и безвредное
  // "wifi_init_default: netstack cb reg failed" в логе.
  wifiShutdown();
  NimBLEDevice::deinit(true);

  Serial.println("Ухожу в deep sleep, разбужусь по фронту ACC ON");
  delay(50); // дать Serial дописать вывод перед сном

  armAccWakeupAndSleep();
  // Дальше код не выполняется — чип полностью перезагрузится при
  // пробуждении и начнёт заново с setup()
}

// Раз в минуту: строка статистики в Serial; появились потери кадров —
// запись в errors.log (из canTask в карту не пишем: он не должен ждать SD)
void statsLoop() {
  static uint32_t last = 0, lastFrames = 0, lastDropped = 0, lastIn = 0, lastOut = 0;
  uint32_t now = millis();
  if (now - last < STATS_PERIOD_MS) return;
  float sec = (now - last) / 1000.0f;
  uint32_t fr = canFramesTotal, dr = canDroppedTotal, in = logInTotal, out = logOutTotal;
  uint32_t qPct = canQueueMax * 100 / CAN_QUEUE_LEN;
#if LOG_COMPRESS == 2
  uint32_t sbPct = lzSbMax * 100 / (32 * 1024);
#else
  uint32_t sbPct = 0;
#endif
  if (last != 0 && fr != lastFrames) {   // шина молчит — строку не печатаем
    Serial.printf("[STAT] кадров %.0f/с, лог %.1f КБ/с, на карту %.1f КБ/с (сжатие %.1fx), "
                  "буфер кодера макс %u%%, очередь CAN макс %u%%, потеряно %lu (всего %lu)\n",
                  (fr - lastFrames) / sec, (in - lastIn) / 1024.0f / sec, (out - lastOut) / 1024.0f / sec,
                  (out - lastOut) ? (float)(in - lastIn) / (out - lastOut) : 0.0f,
                  (unsigned)sbPct, (unsigned)qPct, (unsigned long)(dr - lastDropped), (unsigned long)dr);
  }
  static uint32_t lastBleSent = 0, lastBleDrop = 0;
  uint32_t bs = bleSentTotal, bd = bleDroppedTotal;
  if (last != 0 && (bs != lastBleSent || bd != lastBleDrop))
    Serial.printf("[STAT] BLE ACL: отправлено %.0f кадров/с, потеряно %lu (всего %lu)\n",
                  (bs - lastBleSent) / sec, (unsigned long)(bd - lastBleDrop), (unsigned long)bd);
  if (last != 0 && bd != lastBleDrop)
    errLog("BLE ACL: потеряно %lu кадров за %.0f с — BLE не успевает, сузьте список или добавьте интервал/по изменению",
           (unsigned long)(bd - lastBleDrop), sec);
  lastBleSent = bs; lastBleDrop = bd;
  if (last != 0 && dr != lastDropped)
    errLog("CAN: потеряно %lu кадров за %.0f с (очередь заполнялась до %u%%, буфер кодера до %u%%)",
           (unsigned long)(dr - lastDropped), sec, (unsigned)qPct, (unsigned)sbPct);
  canQueueMax = 0;
#if LOG_COMPRESS == 2
  lzSbMax = 0;
#endif
  last = now; lastFrames = fr; lastDropped = dr; lastIn = in; lastOut = out;
}

void loop() {
  if (wifiOn) {
    webServer.handleClient();
    ArduinoOTA.handle();
  }
  wifiAutoOffLoop();
  rtcServiceLoop();   // отложенная запись времени во внешние часы
  statsLoop();        // строка [STAT] в Serial, потери кадров — в errors.log

  // Не уходим в сон, пока идёт заливка прошивки (иначе "кирпич"),
  // пока пользуются веб-порталом и пока подключён комп по USB
  static bool usbHoldReported = false;
  static bool usbLostTiming = false;
  static uint32_t usbLostSince = 0;
  static bool webHoldReported = false;

  bool webHold = webPortalActive();
  if (!webHold && webHoldReported) {
    Serial.println("Портал: активности нет / клиент отключился — сон разрешён");
    webHoldReported = false;
  }

  if (!otaInProgress && digitalRead(ACC_PIN) == ACC_OFF_LEVEL && webHold) {
    if (!webHoldReported) {
      Serial.printf("ACC выключено, но идёт работа с порталом — не сплю "
                    "(до %lu с без запросов)\n", WEB_HOLD_MS / 1000UL);
      webHoldReported = true;
    }
    usbLostTiming = false;   // после портала отсчёт grace начнётся заново
  } else if (!otaInProgress && digitalRead(ACC_PIN) == ACC_OFF_LEVEL) {
    if (usbHostConnected()) {
      if (usbLostTiming) {
        Serial.printf("USB-хост вернулся через %lu мс — продолжаю не спать\n",
                      (unsigned long)(millis() - usbLostSince));
        usbLostTiming = false;
      }
      if (!usbHoldReported) {
        Serial.println("ACC выключено, но подключён USB-хост — не сплю");
        usbHoldReported = true;
      }
    } else {
      if (!usbLostTiming) {
        usbLostTiming = true;
        usbLostSince = millis();
        if (usbHoldReported) {
          Serial.printf("USB-хост пропал — жду %d мс перед сном\n", USB_LOST_GRACE_MS);
        }
      }
      // Без USB вообще (машина) — задержка перед сном та же, это не страшно
      if (millis() - usbLostSince >= USB_LOST_GRACE_MS) {
        goToSleepUntilAccOn();
      }
    }
  } else {
    usbHoldReported = false;
    usbLostTiming = false;
  }

  vTaskDelay(pdMS_TO_TICKS(10));
}
