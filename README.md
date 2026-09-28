# ESP32 CAN Sniffer + Instrument Cluster Camera

[Русская версия](README_ru.md)

A pair of independent devices for reverse-engineering a car's CAN bus:

- **CAN sniffer** (ESP32-S3) — passively logs every CAN frame to an SD card with real time-of-day, sleeps when the ignition is off, and provides a Wi-Fi portal for downloading logs, setting the clock, choosing the CAN bitrate and updating firmware.
- **Cluster camera** (AI-Thinker ESP32-CAM) — photographs the instrument cluster every 500 ms. Its clock is synchronized with the sniffer over BLE, so every frame can be matched to the CAN traffic recorded at the same moment.

Originally built for an Audi A4 B9 (MLB-Evo, I-CAN at 500 kbit/s), but the sniffer works on any CAN bus from 20 kbit/s to 1 Mbit/s.

---

## Repository layout

Arduino IDE requires each sketch to live in a folder with the same name:

```
esp32s3_wroom_can_sd_ble_sync_server/
    esp32s3_wroom_can_sd_ble_sync_server.ino    — CAN sniffer, ESP32-S3-WROOM-1 CAM board
    src/lzma/                                   — LZMA encoder (LZMA SDK, public domain)
esp32s3_can_sd_ble_sync_server/
    esp32s3_can_sd_ble_sync_server.ino          — CAN sniffer, ESP32-S3 Super Mini
    src/lzma/                                   — LZMA encoder (LZMA SDK, public domain)
esp32cam_aithinker_video_ble_sync_client/
    esp32cam_aithinker_video_ble_sync_client.ino — camera, recording mode
esp32cam_aithinker_aiming_stream/
    esp32cam_aithinker_aiming_stream.ino         — camera, aiming mode (live stream)
esp32s3cam_ov5640_video_ble_sync_client/
    esp32s3cam_ov5640_video_ble_sync_client.ino  — ESP32-S3 + OV5640 camera, recording mode
esp32s3cam_ov5640_aiming_stream/
    esp32s3cam_ov5640_aiming_stream.ino          — ESP32-S3 + OV5640 camera, aiming mode
docs/
    sniffer_supermini.svg, sniffer_supermini_sd_ams1117.svg, sniffer_wroom.svg,
    camera_aithinker.svg, camera_s3_ov5640.svg   — wiring diagrams
tools/
    can_time_decode.py                           — date/time and odometer from CAN logs
    log_unpack.py                                — unpack .txt.lzma / .txt.gz logs (also truncated ones), join a folder
README.md
README_ru.md
```

---

## Wiring diagrams

| | |
|---|---|
| CAN sniffer, ESP32-S3-WROOM-1 CAM | [docs/sniffer_wroom.svg](docs/sniffer_wroom.svg) |
| CAN sniffer, ESP32-S3 Super Mini, plain 3.3 V SD module (default) | [docs/sniffer_supermini.svg](docs/sniffer_supermini.svg) |
| CAN sniffer, ESP32-S3 Super Mini, SD module with AMS1117 feeding the peripherals | [docs/sniffer_supermini_sd_ams1117.svg](docs/sniffer_supermini_sd_ams1117.svg) |
| Camera, AI-Thinker ESP32-CAM | [docs/camera_aithinker.svg](docs/camera_aithinker.svg) |
| Camera, ESP32-S3-WROOM-1 CAM + OV5640 | [docs/camera_s3_ov5640.svg](docs/camera_s3_ov5640.svg) |

![CAN sniffer, ESP32-S3 Super Mini](docs/sniffer_supermini.svg)

---

## 1. CAN sniffer

### Hardware

Two sketches with identical logic, portal, log format and BLE, differing only in board-specific parts:

| | **`esp32s3_wroom_can_sd_ble_sync_server`** — ESP32-S3-WROOM-1 CAM (recommended) | **`esp32s3_can_sd_ble_sync_server`** — ESP32-S3 Super Mini |
|---|---|---|
| Module | N16R8: 16 MB flash, 8 MB **OPI** PSRAM | FH4R2: 4 MB flash, 2 MB **QSPI** PSRAM |
| SD card | On-board slot, SD_MMC 1-bit | External SPI module |
| 3.3 V regulator | AMS1117 (needs ≥ 4.4 V input) | Small on-board LDO — marginal with SD + Wi-Fi |

Common parts:

| Part | Notes |
|---|---|
| CAN transceiver | SN65HVD230, or TJA1051T/3 (e.g. CJMCU-1051 board with a genuine chip) |
| RTC (optional) | DS3231 or PCF8563, detected automatically. Without it, time comes from the CAN bus (see *Time*). DS3231 MH board: **charging circuit must be removed** when using a CR2032 |
| Optocoupler (PC817 or similar) | ACC (ignition) detection, 1.2–1.5 kΩ series resistor on the 12 V side |
| 12 V → 5 V DC-DC | Low quiescent current matters: it runs from permanent 12 V. For the WROOM board set it to 5.2–5.3 V if there is a Schottky diode in series |

### Wiring

| Signal | WROOM CAM | Super Mini |
|---|---|---|
| TWAI TX → transceiver **CTX** (D / TXD) | GPIO41 | GPIO4 |
| TWAI RX ← transceiver **CRX** (R / RXD) | GPIO42 | GPIO5 |
| SD | on-board slot (CLK 39, CMD 38, D0 40) | GPIO10 CS / 11 MOSI / 12 SCK / 13 MISO |
| RTC SDA / SCL (optional) | GPIO21 / GPIO47 | GPIO8 / GPIO9 |
| ACC (optocoupler output, LOW = ignition on) | GPIO14 | GPIO7 |

On the WROOM CAM board GPIO4–18 are routed to the camera connector, so the sniffer uses free pins. The board's IO2 LED is kept off; GPIO2 is not suitable for I2C.

> **TX goes to TX, RX to RX.** Unlike UART, the lines are *not* crossed: the names on both the ESP32 and the transceiver refer to the same direction (towards / from the bus).

**TJA1051T/3:** VCC = 5 V, VIO = 3.3 V (from the ESP32), S = VIO for hardware silent mode in the car (the transceiver physically cannot transmit), S = GND on the bench. Before connecting to the ESP32, check that CRX idles at ~3.3 V, not 5 V.

**Termination:** the car's bus is already terminated. CANH–CANL on the sniffer must measure tens of kΩ (transceiver input). If you see ~120 Ω, remove the resistor on the transceiver board.

### Arduino IDE settings

Core **Arduino-ESP32 3.x**, board **ESP32S3 Dev Module**, USB Mode **Hardware CDC and JTAG**, USB CDC On Boot **Enabled**, plus:

| | WROOM CAM | Super Mini |
|---|---|---|
| Flash Size | 16MB | 4MB |
| PSRAM | **OPI PSRAM** | **QSPI PSRAM** |
| Partition Scheme | 16M Flash (3MB APP/9.9MB FATFS) | Minimal SPIFFS (1.9MB APP with OTA/190KB SPIFFS) |

Choosing the wrong PSRAM type gives `PSRAM chip is not connected`. Both partition schemes have two app slots for OTA. The partition scheme can only be changed by flashing over USB; do this once, then update over Wi-Fi. On the WROOM board, the "USB wakes/keeps awake" logic works through the native **USB** port, not the **COM** port.

Libraries: **RTClib** (Adafruit), **NimBLE-Arduino 2.x**. Everything else is part of the core. The LZMA encoder is shipped with the sketch in `src/lzma/` and compiled automatically — nothing to install.

### Build options (`#define` at the top of the sketch)

| Define | Default | Meaning |
|---|---|---|
| `FW_VERSION` | `"2.2.2"` (WROOM) / `"2.2.2"` (Super Mini) | Firmware version (MAJOR — breaking formats, MINOR — features, PATCH — fixes) |
| `CAN_LISTEN_ONLY` | `1` | 1 = car (never transmits, not even ACK), 0 = bench (needed when the bench has only one other node) |
| `CAN_BITRATE_DEFAULT` | `500000` | Bitrate used when nothing is stored in NVS |
| `CAN_TIME_SYNC` | `2` | Time from CAN frame 0x6B2: 0 = off, 1 = only while time is unknown, 2 = also correct the clock if off by more than `CAN_TIME_MAX_DIFF_S` (5 s) |
| `CAN_TIME_TZ_HOURS` | `0` | Offset if the car sends UTC |
| `CAN_TIME_MAX_JUMP_S` | `0` | Protection against a reset car clock: 0 = trust the car fully; >0 = if the sniffer already has valid time and the car differs by more than this, the car time is ignored. Car time earlier than the firmware build date is always ignored |
| `CAN_TIME_WAIT_MS` | `4500` | How long to wait for the time frame before opening the first log file (frames are buffered meanwhile) |
| `CAN_TIME_CONFIRM_FRAMES` | `3` | Car time is accepted only after this many consecutive 0x6B2 frames agree with each other (±2 s); out-of-range values and impossible dates reset the series — protects against GPS glitches |
| `LOG_MAX_BYTES` | `4 MB` | Log rotation size — of the file on the card, i.e. compressed (a new file is also started on every boot) |
| `LOG_COMPRESS` | `2` | Compression on the fly: 2 = LZMA (`.txt.lzma`, ~8× smaller, ~1 MB PSRAM), 1 = gzip (`.txt.gz`, ~3.5×, ~130 KB), 0 = plain `.txt` |
| `LOG_LZMA_DICT` | `64 KB` | LZMA dictionary; 64 KB is the sweet spot for CAN logs (32 KB ≈ 7.7×) |
| `LOG_GZ_DEPTH` | `4` | gzip only — match search depth: 1 = least CPU, 4 = better ratio almost for free |
| `LOG_GZ_SYNC_MS` | `1000` | gzip only — compressed data is flushed to the card this often; after a crash the file unpacks up to that point |
| `WIFI_AP_HIDDEN` | `0` | 1 = hidden access point (SSID not broadcast) |
| `WIFI_TX_POWER` | `WIFI_POWER_13dBm` (WROOM) / `WIFI_POWER_8_5dBm` (Super Mini) | Wi-Fi transmit power: lower means smaller current spikes, at the cost of range and portal speed |
| `AP_SSID` / `AP_PASSWORD` | `S3-CAN-Sniffer-Setup` / `canlogger123` | Portal access point |
| `OTA_PASSWORD` / `OTA_WEB_USER` | `changeme123` / `admin` | **Change before use.** Shared by `/update` and espota |
| `USB_HOST_KEEPS_AWAKE` | `1` | Don't sleep while a USB host (PC) is connected. Set to 0 if the sniffer is powered from a car head unit's USB port |
| `USB_LOST_GRACE_MS` | `5000` | How long the USB host must be gone before sleeping |
| `WEB_HOLD_MS` | `300000` | Portal activity keeps the sniffer awake this long after the last request |

### Status LED

The on-board RGB LED (WS2812, GPIO48) flashes once per second while the sniffer is awake. First a status flash, by priority: **red** — an error that prevents logging (SD not mounted, log file can't be opened, CAN reception failed to start); **yellow** — partially working (time not set, logging to `/no-rtc`, or the RTC reports its oscillator stopped); **blue** — a client is connected to the access point (portal in use); **green** — everything is fine. Then a short **white** flash if CAN frames were received during the last second. Driving: green + white; silent bus: green only. It is switched off before deep sleep and restarts. Options: `RGB_LED_ENABLE`, `RGB_LED_BRIGHTNESS`, `RGB_LED_FLASH_MS`, `RGB_LED_PERIOD_MS`, `RGB_LED_GAP_MS`.

### Web portal

Connect to the access point and open `http://192.168.4.1`.

- `/` — date/time with its source and the detected RTC, "Phone time" button, CAN bitrate, firmware version
- `/logs` — log folders by date, collapsed; press **+** to see a folder's files (count and total size are shown right away); single-file download, several folders as one TAR archive, or recursive deletion of the selected folders (with confirmation; the folder currently being written is protected)
- If the SD card can't be mounted, the home page offers **Format card as FAT32** (password-protected like `/update`). Cards of 64 GB and larger usually come formatted as exFAT, which the Arduino-ESP32 core does not support. A mounted card is never formatted — use folder deletion on `/logs` instead. You can also format large cards as FAT32 on a PC with guiformat or Rufus (the standard Windows formatter refuses FAT32 above 32 GB).
- `/update` — firmware upload from the browser (Basic Auth: `OTA_WEB_USER` / `OTA_PASSWORD`)

**Language:** RU/EN. The red button at the top of every page shows the *other* language; the choice is stored in NVS (`ui/lang`), Russian by default, switching is instant. Serial output is always in Russian.

**CAN bitrate:** 1000, 800, 500, 250, 125, 100, 83.3, 50, 33.3, 25, 20 kbit/s. Stored in NVS (`sniffer/can_bps`), applied by an automatic restart. 83.3 and 33.3 kbit/s use hand-written timings; 33.3 kbit/s (GMLAN single-wire) needs a single-wire transceiver.

### Firmware update

**From a browser (phone works too):** in Arduino IDE use *Sketch → Export Compiled Binary*, take `<sketch>.ino.bin` (not `merged`, not `bootloader`), upload it at `/update`. The page checks that the file is an ESP32-S3 application image before writing anything; if the upload fails or the image doesn't pass verification, the old firmware stays active.

**With espota** (no Arduino IDE needed, `espota.exe` is a standalone file from the core's `tools` folder):

```
espota.exe -i 192.168.4.1 -p 3232 -a <OTA_PASSWORD> -f firmware.bin -P 3233 -d -r
```

The ESP32 connects *back* to the PC, so allow port 3233 in the firewall.

### Power and sleep

- Ignition off → the log is closed cleanly → deep sleep, woken by the ACC pin. The pull-up is kept in the RTC domain, otherwise the floating pin causes endless phantom wake-ups.
- Woken up but ignition already off → straight back to sleep without full initialization.
- The sniffer stays awake while: firmware is being uploaded, a USB host is connected, or the portal is being used (a phone that merely auto-joins the AP does not keep it awake).
- The portal only starts with the ignition on; switching the ignition off while using it is fine.

### Time

Time with a year earlier than `TIME_MIN_YEAR` (2026) is treated as an error and is not accepted from any source (RTC, CAN, portal): logs go to `/no-rtc` and the LED shows yellow.

The external RTC is optional. At boot the sniffer takes time from, in order:

1. **DS3231 or PCF8563** on I2C, if present (detected by address 0x68 / 0x51). If the chip reports that its oscillator stopped (DS3231 OSF / PCF8563 VL) but the time looks sane, it is used with a warning on the portal.
2. After waking from deep sleep — the **ESP32 system clock**, which keeps running while asleep.
3. **CAN frame 0x6B2** (Diagnose_01: date, time and odometer, once per second). Frames received before it arrives are buffered, so the first log file lands in the correct date folder.
4. The **portal**: manually or with the "Phone time" button.

With `CAN_TIME_SYNC 2` the car's clock is authoritative: after disconnecting the car battery its clock is wrong, so set it (or enable GPS time sync in MMI), or switch to `1` / `0`. A time set on the portal or taken from CAN is also written to the external RTC if there is one. The boot marker in the log records the time source and the RTC type.

`tools/can_time_decode.py` decodes 0x6B2 from existing logs and maps sniffer `millis` (and camera frame names) to wall-clock time.

### Diagnostics

**`[STAT]` line in Serial** once a minute while the bus is active:

```
[STAT] кадров 1650/с, лог 64.2 КБ/с, на карту 7.7 КБ/с (сжатие 8.3x), буфер кодера макс 9%, очередь CAN макс 2%, потеряно 0 (всего 0)
```

Frames per second, text written to the log, compressed data written to the card, peak fill of the LZMA input buffer and of the CAN frame queue during the minute, and lost frames. If the queue approaches 100 % or frames get lost, the encoder can't keep up — raise the CPU frequency to 240 MHz or switch to `LOG_COMPRESS 1`.

**`/errors.log` in the root of the SD card** — errors and important events with date/time, firmware version and uptime: abnormal resets (`PANIC`, `TASK_WDT`, `BROWNOUT`…), lost CAN frames (once a minute, aggregated), log file / encoder errors, TWAI start failure, RTC oscillator-stopped flag, rejected car time, web OTA errors. Messages from before the card is mounted are kept in RAM and written afterwards. Above 256 KB the file is moved to `errors.old.log`. The portal shows the received / dropped frame counters on the home page and links to the error log there and on `/logs`. The close marker of every log file also records `dropped=N`.

### Log format

Files: `/YYYY-MM-DD/can_log_NNNN.txt.lzma` (`.txt.gz` with `LOG_COMPRESS 1`, `.txt` with `0`). Files are created only when real CAN frames arrive: no traffic on the bus — no file. A new file is started on every boot (at the first frame) and whenever the current one reaches `LOG_MAX_BYTES` (with LZMA a 4 MB file holds ~33 MB of text — about 8 minutes of busy I-CAN traffic). After midnight the next file goes into the new date folder. If the DS3231 is missing or its time is invalid, logs go to `/no-rtc/` until the time is set on the portal (the camera then writes to `/no-rtc/frames/`). The DS3231 is read only once at boot; after that the ESP32 system clock is used. Files of one trip can simply be concatenated in name order.

**LZMA (default).** The encoder from the LZMA SDK runs in its own task: the SD task feeds it log lines through a stream buffer, the encoder writes the compressed stream to the file. Files are in the standard `.lzma` format and open in 7-Zip, FAR (ArcLite), `xz --format=lzma -d` or Python's `lzma`. Compressed data reaches the card every ~4 KB. A file that was not closed properly (crash, power loss) has no end marker — `tools/log_unpack.py` unpacks it up to the break without errors and can join a folder into one text file for other tools (e.g. `bap_nav_decode.py`).

**gzip (`LOG_COMPRESS 1`).** Standard gzip written by a small built-in compressor (LZ77 + fixed Huffman codes): less memory, ~3.5× instead of ~8×. Data is sync-flushed every `LOG_GZ_SYNC_MS`.

One frame per line (inside the archive):

```
<millis> <S|X> <ID hex> [R]<DLC> <data hex>

123456 S 30B 8 00 11 22 33 44 55 66 77
123470 X 17333210 8 80 0A 4C 92 00 00 01 F4
```

`S` — 11-bit ID (3 hex digits), `X` — 29-bit ID (8 hex digits), `R` before DLC — remote frame. Lines starting with `#` are markers and should be skipped by parsers:

```
# 812 ===== BOOT / SYNC MARK ===== fw=1.2.0 build="Sep 25 2026 22:53:51" reset=DEEPSLEEP can_bps=500000
# 931204 ===== ACC OFF / SHUTDOWN =====
```

Close reasons: `ACC OFF / SHUTDOWN`, `OTA REBOOT`, `CONFIG REBOOT` (with `dropped=N` — frames lost since boot), `ROTATE`; a rotated file starts with `CONTINUED from can_log_NNNN.txt.lzma` plus the firmware version and bitrate, so every file is self-contained. The boot marker records the firmware version, reset reason (`BROWNOUT`, `PANIC`, `TASK_WDT`… help diagnose spontaneous reboots) and bitrate.

### BLE

Device name `S3-CAN-Sniffer`, service `A1B2C3D4-0001-41A2-9E3B-000000000001`:

| UUID suffix | Properties | Purpose |
|---|---|---|
| `…0002` | NOTIFY | Time sync `{millis, unixEpoch}`, sent from `onSubscribe()` |
| `…0004` | WRITE | Select a CAN ID to watch (uint32) |
| `…0005` | NOTIFY | Raw bytes of the watched ID |

Also the standard Device Information Service (`0x180A`) with firmware version and build date.

---

## 2. Cluster camera (AI-Thinker ESP32-CAM)

### Recording mode — `esp32cam_aithinker_video_ble_sync_client`

- JPEG every 500 ms, SVGA (800×600) with PSRAM, VGA fallback without it
- Connects to `S3-CAN-Sniffer` over BLE once at startup, synchronizes `millis()` and the system clock
- Frames: `/YYYY-MM-DD/frames/f_<millis>.jpg`, where `<millis>` is on the **sniffer's** time scale. If sync fails within 10 s — `/no-sync/frames/`
- SD via `SD_MMC` in **1-bit** mode (CLK 14, CMD 15, D0 2). GPIO12 is deliberately unused (strapping pin)
- The white flash LED (GPIO4) is forced off
- Power: Schottky diode + supercapacitor. `GPIO13` senses supply loss through a divider taken **before** the diode; on loss the SD card is closed and the chip goes to deep sleep. `POWER_LOST_THRESHOLD` must be calibrated on the real hardware

Board: **AI Thinker ESP32-CAM**, PSRAM enabled. Library: **NimBLE-Arduino 2.x**.

### Aiming mode — `esp32cam_aithinker_aiming_stream`

A separate sketch for pointing the camera during installation: access point `ESP32CAM-Aiming`, MJPEG stream at `/stream`, page `/` with a rule-of-thirds grid and a red centre crosshair.

| Define | Default | Meaning |
|---|---|---|
| `WIFI_AP_HIDDEN` | `0` | 1 = hidden access point |
| `AP_PASSWORD` | `""` | Empty = open network, otherwise WPA2 (at least 8 characters) |

Page language RU/EN with the same red button as the sniffer (NVS `ui/lang`, Russian by default). Pressing it stops the stream first, because the web server serves one client at a time.

The two modes are separate sketches because camera + Wi-Fi web server + NimBLE together overflow IRAM on the classic ESP32. To switch, flash the other sketch; a toggle on GPIO0 puts the board into download mode without opening the housing.

### Alternative camera: ESP32-S3-WROOM (N16R8) + OV5640

Sketches `esp32s3cam_ov5640_video_ble_sync_client` and `esp32s3cam_ov5640_aiming_stream` do the same job on an ESP32-S3 camera board (Freenove ESP32-S3 WROOM CAM and clones, camera pinout as on ESP32-S3-EYE). Use one camera variant or the other; both work with the same sniffer.

| | AI-Thinker ESP32-CAM | ESP32-S3 CAM + OV5640 |
|---|---|---|
| Default resolution | SVGA 800×600 | HD 1280×720 (`FRAME_SIZE`, up to 2560×1920) |
| Camera pins | XCLK 0, SIOD 26, SIOC 27, … | XCLK 15, SIOD 4, SIOC 5, D0–D7 11/9/8/10/12/18/17/16, VSYNC 6, HREF 7, PCLK 13 |
| SD (1-bit SD_MMC) | CLK 14, CMD 15, D0 2 | CLK 39, CMD 38, D0 40 |
| Power sense | GPIO13 | GPIO1 (ADC1) |
| Flash LED | GPIO4, forced off | none; `STATUS_LED_PIN` for any board LED that reflects in the cluster glass |
| Aiming AP | `ESP32CAM-Aiming` | `ESP32S3CAM-Aiming` |

Pinout verified against the ESP32-S3-WROOM-1 CAM board (N16R8, two USB-C); other clones may differ. The board's IO2 LED is kept off (`STATUS_LED_PIN 2`); the red power LED can't be switched off in software — cover it if it reflects in the cluster glass.

Other differences:
- `CAMERA_GRAB_LATEST`: every frame is fresh rather than taken from the queue, so the timestamp in the file name matches the moment of capture.
- `WARMUP_FRAMES`: the first frames after start are discarded while OV5640 settles exposure and white balance.
- `CAM_VFLIP` / `CAM_HMIRROR`: OV5640 modules are often mounted upside down; set the same values in both sketches.

Arduino IDE: **ESP32S3 Dev Module**, Flash Size **16MB**, PSRAM **OPI PSRAM** (N16R8 has Octal PSRAM, unlike the sniffer's QSPI), Partition Scheme **16M Flash (3MB APP/9.9MB FATFS)** or **Huge APP**. USB CDC On Boot: *Enabled* to see Serial on the native "USB" port, *Disabled* for the "COM" (UART bridge) port.

The ESP32-S3 has no IRAM shortage, so recording and aiming could be merged into one firmware later; for now they stay separate to match the AI-Thinker variant.

---

## 3. Using it in the car

1. Start the **sniffer first** and wait for `BLE синхронизация запущена (S3-CAN-Sniffer)` in Serial.
2. Power up the **camera** — it finds the sniffer, synchronizes and starts recording.
3. Afterwards connect to `S3-CAN-Sniffer-Setup`, open `http://192.168.4.1/logs` and download the dates you need as one TAR archive.

**Second unit** (another car): rename the BLE name and `AP_SSID` (e.g. add `-2`), otherwise logs from different cars are easy to mix up.

---

## Safety

- **Use `CAN_LISTEN_ONLY 1` in any real car.** In NORMAL mode a wrong bitrate produces error frames and can disturb the car's control units.
- **No extra termination.** CANH–CANL on the sniffer must not be ~120 Ω.
- **DS3231 + CR2032:** the MH board charges the battery from VCC. Remove the charging resistor (marked `201`) or use a rechargeable LIR2032. A charged CR2032 in a hot car can swell or leak.
- **Power:** don't feed the sniffer from the head unit's USB port (it acts as a USB host and keeps the sniffer awake). Use a DC-DC with low quiescent current, a fuse and a TVS diode on the input.
- **Antenna:** the Super Mini has a chip antenna on the board; keep it away from metal, or the portal and BLE range drop sharply.
- **Change the passwords** (`AP_PASSWORD`, `OTA_PASSWORD`) before installing.

---

## Credits

Log compression uses the LZMA encoder from the [LZMA SDK](https://github.com/ip7z/7zip) by Igor Pavlov (public domain).


Developed with the assistance of **Claude** (Anthropic): firmware, tools, wiring diagrams and documentation were written in collaboration with the AI assistant; hardware, testing in the car and design decisions — by the author.
