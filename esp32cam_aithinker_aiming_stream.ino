/*
  AI-Thinker ESP32-CAM — ВРЕМЕННАЯ прошивка для наведения камеры
  ------------------------------------------------------------------------------
  Это НЕ рабочая прошивка для записи — отдельный, самостоятельный скетч
  только для физической юстировки камеры при монтаже: поднимает WiFi
  точку доступа и отдаёт живой MJPEG-поток с камеры прямо в браузер,
  чтобы можно было в реальном времени видеть картинку, пока крутите
  камеру руками, добиваясь центрирования на приборке.

  После того как найдёте правильное положение — залейте обратно рабочую
  прошивку (esp32cam_aithinker_video_ble_sync_client.ino), эта временная
  version больше не нужна.

  Использование:
    1. Залить этот скетч.
    2. На телефоне/ноутбуке подключиться к WiFi-сети "ESP32CAM-Aiming".
    3. Открыть в браузере http://192.168.4.1
    4. Крутить/двигать камеру, глядя на живую картинку, пока приборка
       не окажется по центру кадра как нужно.
    5. Зафиксировать камеру физически, залить обратно рабочую прошивку.

  Опции:
    WIFI_AP_HIDDEN 1 — скрытая сеть (SSID не транслируется, подключаться
                       вручную по имени). По умолчанию 0 — видимая.
    AP_PASSWORD      — пустая строка = открытая сеть (по умолчанию),
                       иначе WPA2, не короче 8 символов.
  Язык страницы: RU/EN, красная кнопка вверху. Хранится в NVS (ui/lang),
  по умолчанию русский. Serial-вывод всегда на русском.

  Библиотеки: esp32-camera, WiFi.h, WebServer.h, Preferences.h — из ядра Arduino-ESP32.
*/

#include "esp_camera.h"
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>

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

// ---------- Встроенная вспышка — гасим, как и в рабочей прошивке ----------
#define FLASH_LED_PIN  4

const char* AP_SSID     = "ESP32CAM-Aiming";
// По умолчанию без пароля — прошивка на пару минут юстировки, лишние
// сложности с подключением тут ни к чему. Если нужен пароль — впишите
// (WPA2, минимум 8 символов, иначе точка доступа не поднимется).
const char* AP_PASSWORD = "";
#define AP_CHANNEL      1
// 1 — скрытая сеть: SSID не транслируется, в списке сетей её не видно,
//     подключаться вручную ("Добавить сеть", имя вводится руками).
// 0 — обычная видимая сеть (по умолчанию)
#define WIFI_AP_HIDDEN  0

// ---------- Язык страницы (RU/EN), хранится в NVS ----------
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
  Serial.printf("Язык страницы: %s\n", en ? "English" : "русский");
}

WebServer webServer(80);

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

  // Для юстировки достаточно скромного разрешения — так поток будет
  // отзывчивее (выше кадровая частота), детализация тут не важна,
  // важна только скорость обновления картинки на глаз
  if (psramFound()) {
    config.frame_size = FRAMESIZE_VGA; // 640x480
    config.jpeg_quality = 12;
    config.fb_count = 2;
    config.fb_location = CAMERA_FB_IN_PSRAM;
  } else {
    config.frame_size = FRAMESIZE_QVGA; // 320x240
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

void handleRoot() {
  String html =
    "<!DOCTYPE html><html lang='" + String(uiEn ? "en" : "ru") + "'><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width, initial-scale=1'>"
    "<title>" + TR("Наведение камеры", "Camera aiming") + "</title>"
    "<style>"
    "body{background:#0d0d0d;color:#d8d8d8;font-family:sans-serif;"
    "text-align:center;margin:0;padding:16px;}"
    "h3{margin-bottom:12px;}"
    ".frame-wrap{position:relative;display:inline-block;max-width:100%;}"
    ".frame-wrap img{display:block;width:100%;border:2px solid #4caf50;}"
    // Сетка "правило третей" — две вертикальные и две горизонтальные линии
    // на 33.3%/66.6%, плюс линия по краю кадра (0%/100%) — реализовано
    // одним CSS-градиентом, без лишней разметки
    ".grid{position:absolute;top:0;left:0;right:0;bottom:0;pointer-events:none;"
    "background-image:"
    "linear-gradient(rgba(76,175,80,0.8) 1px, transparent 1px),"
    "linear-gradient(90deg, rgba(76,175,80,0.8) 1px, transparent 1px);"
    "background-size:33.333% 33.333%;}"
    // Центральный крест — отдельным, более ярким цветом, для точного
    // центрирования (не просто "в трети", а ровно посередине кадра)
    ".crosshair-h{position:absolute;left:0;right:0;top:50%;height:2px;"
    "background:rgba(255,80,80,0.9);pointer-events:none;}"
    ".crosshair-v{position:absolute;top:0;bottom:0;left:50%;width:2px;"
    "background:rgba(255,80,80,0.9);pointer-events:none;}"
    ".langbar{text-align:right;margin:0 0 4px 0;}"
    "button.lang{background:#c62828;border:1px solid #c62828;color:#fff;font-weight:bold;"
    "padding:6px 10px;border-radius:4px;cursor:pointer;}"
    "button.lang:hover{background:#e53935;}"
    "</style></head><body>"
    // Кнопка языка. Пока идёт поток, сервер занят им (WebServer обслуживает
    // одного клиента за раз), поэтому сначала обрываем поток (src=''), а
    // форму отправляем с небольшой задержкой — иначе запрос повиснет.
    "<form class='langbar' id='lf' method='POST' action='/set-lang'>"
    "<input type='hidden' name='lang' value='" + String(uiEn ? "ru" : "en") + "'>"
    "<button class='lang' type='button' onclick=\"document.getElementById('cam').src='';"
    "setTimeout(function(){document.getElementById('lf').submit();},400);\">" +
    String(uiEn ? "Русский" : "English") + "</button></form>"
    "<h3>" + TR("Живой поток с камеры — наведите на центр приборки",
                "Live camera stream — aim at the centre of the instrument cluster") + "</h3>"
    "<div class='frame-wrap'>"
    "<img id='cam' src='/stream'>"
    "<div class='grid'></div>"
    "<div class='crosshair-h'></div>"
    "<div class='crosshair-v'></div>"
    "</div>"
    "<p style='color:#888;font-size:0.9em;margin-top:10px;'>"
    "" + TR("Зелёная сетка — правило третей, красный крест — точный центр кадра",
            "Green grid — rule of thirds, red cross — exact frame centre") + "</p>"
    "</body></html>";

  webServer.send(200, "text/html; charset=utf-8", html);
}

// POST /set-lang: сохранить язык и вернуться на главную
void handleSetLang() {
  saveUiLanguage(webServer.arg("lang") == "en");
  webServer.sendHeader("Location", "/");
  webServer.send(303);
}

void handleStream() {
  WiFiClient client = webServer.client();

  String response = "HTTP/1.1 200 OK\r\n";
  response += "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n\r\n";
  client.print(response);

  while (client.connected()) {
    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) {
      Serial.println("Ошибка захвата кадра для потока");
      break;
    }

    client.print("--frame\r\n");
    client.print("Content-Type: image/jpeg\r\n");
    client.print("Content-Length: " + String(fb->len) + "\r\n\r\n");
    client.write(fb->buf, fb->len);
    client.print("\r\n");

    esp_camera_fb_return(fb);

    if (!client.connected()) break;
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);

  pinMode(FLASH_LED_PIN, OUTPUT);
  digitalWrite(FLASH_LED_PIN, LOW);

  loadUiLanguage();
  setupCamera();

  WiFi.mode(WIFI_AP);
  // Пустой пароль → открытая сеть; hidden → SSID не транслируется
  WiFi.softAP(AP_SSID, (AP_PASSWORD[0] ? AP_PASSWORD : NULL), AP_CHANNEL, WIFI_AP_HIDDEN);

  Serial.printf("Точка доступа поднята: %s (%s, %s)\n", AP_SSID,
                WIFI_AP_HIDDEN ? "скрытая" : "видимая",
                AP_PASSWORD[0] ? "с паролем" : "без пароля");
  Serial.print("Откройте в браузере: http://");
  Serial.println(WiFi.softAPIP());

  webServer.on("/", handleRoot);
  webServer.on("/stream", handleStream);
  webServer.on("/set-lang", HTTP_POST, handleSetLang);
  webServer.begin();
}

void loop() {
  webServer.handleClient();
}
