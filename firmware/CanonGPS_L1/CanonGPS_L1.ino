/*
 * CanonGPS_L1 - Seeed Wio Tracker L1 / L1 Pro as a Bluetooth GPS source for Canon EOS cameras
 *
 * The camera thinks it is paired with a smartphone running Canon Camera Connect and
 * receives the position from the tracker's built-in GNSS receiver. Every photo gets
 * GPS coordinates, altitude and UTC time in its EXIF data.
 *
 * Project:   https://github.com/spaceduck518/wio-tracker-canon-gps  (see README.md)
 * Protocol:  based on the reverse engineering by the "furble" project
 *            (https://github.com/gkoh/furble, MIT license, (c) 2020 Guo-Rong Koh)
 * Tested:    Wio Tracker L1 Pro + Canon EOS R6 Mark II
 *
 * Board:     "Seeed Wio Tracker L1 / L1 Pro (eigene Definition)"
 *            FQBN WioTrackerL1:nrf52:wiotrackerl1
 *            custom board definition from board/ in this repository,
 *            based on "Seeed nRF52 Boards" 1.1.13 (SoftDevice S140 7.3.0)
 * Libraries: U8g2, TinyGPSPlus (Bluefruit, LittleFS, Wire, SPI come with the core)
 *
 * Operation (no computer needed):
 *   - Switch on: searches the camera, connects, sends GPS. That's it.
 *   - Joystick (L1 Pro): left/right = page (Status, GPS, Device, Settings),
 *     up/down = select setting, press = change value. First move only wakes the display.
 *   - Small button left of the joystick = key lock: short = lock, hold 2 s = unlock.
 *   - Settings (stored in flash): language DE/EN, send interval, display timeout,
 *     key lock auto/off, delete pairing (with confirmation).
 *   - LED: short blink every 1 s = searching, fast = confirm pairing on the camera,
 *     1 flash / 5 s = sending GPS, 2 flashes / 5 s = connected but no GPS fix.
 *
 * Serial commands (USB CDC, 115200 baud):
 *   s status   r reconnect   x delete pairing   l toggle language
 *   h listen-only diagnostic (count camera adverts, never connect)
 *   Z deliberately crash (tests the crash log)
 *   D demo mode   v next display page   k next menu entry   P framebuffer dump (screenshot)
 *
 * Code comments are mostly in German (the original author's language).
 */

#include <Arduino.h>
#include <bluefruit.h>
#include <Adafruit_LittleFS.h>
#include <InternalFileSystem.h>
#include <SPI.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <TinyGPSPlus.h>
#include <math.h>
#include <time.h>

using namespace Adafruit_LittleFS_Namespace;

// ============================ Einstellungen ============================

#define FW_VERSION "1.1.0"
static const char *DEVICE_NAME = "Wio GPS";          // erscheint an der Kamera

// Auswahl im Einstellungsmenue (Standard = Index in Klammern)
static const uint16_t SEND_INTERVALS_S[] = {2, 5, 10, 30};       // (1) = 5 s
static const uint16_t DISPLAY_TIMES_S[]  = {15, 30, 60, 120, 0}; // (2) = 60 s, 0 = immer an
static const uint8_t  DEFAULT_INTERVAL_IDX = 1;
static const uint8_t  DEFAULT_DISPLAY_IDX  = 2;
static const uint32_t PAIR_TIMEOUT_MS       = 60000;  // neue Kamera: Zeit zum Bestaetigen
static const uint32_t KNOWN_CAMERA_WAIT_MS  = 3000;   // bekannte Kamera: Antwort ~1,5 s oder gar nicht
static const uint32_t RETRY_DELAY_MS        = 300;
static const uint32_t GPS_MAX_AGE_MS        = 3000;   // aelter = kein gueltiger Fix
// furble schaltet die Kamera nach dem Verbinden in den Aufnahmemodus. Fuer GPS unnoetig,
// und eine ausgeschaltete Kamera reagiert darauf -> aus.
static const bool     SEND_MODE_SHOOT       = false;

// Sprache beim allerersten Start (danach gilt die gespeicherte Auswahl)
enum Lang : uint8_t { LANG_DE = 0, LANG_EN = 1 };
static const Lang DEFAULT_LANG = LANG_EN;   // LANG_DE fuer Deutsch

// Display
#define OLED_SH1106 1   // 1,3"-OLED des L1 ist ein SH1106 (132 Spalten, Bild ab Spalte 2).
                        // Mit SSD1306-Treiber: links 2 px abgeschnitten, rechts Pixelmuell.
#define OX 1            // zusaetzlicher Abstand links (Gehaeuse verdeckt den Rand)

// GPS
static const uint32_t GPS_BAUD = 9600;

// ======================= Canon BLE UUIDs (furble) ======================
#define CANON_UUID(p) p "-0000-1000-0000-d8492fffa821"

BLEClientService        priSvc   (CANON_UUID("00010000"));
BLEClientCharacteristic nameChr  (CANON_UUID("00010006"));   // Pairing, Indications
BLEClientCharacteristic idenChr  (CANON_UUID("0001000a"));   // Identifikation
BLEClientService        remoteSvc(CANON_UUID("00050000"));   // Fernbedienungs-Modus (kein GPS)
BLEClientService        modeSvc  (CANON_UUID("00030000"));
BLEClientCharacteristic modeChr  (CANON_UUID("00030010"));
BLEClientService        geoSvc   (CANON_UUID("00040000"));
BLEClientCharacteristic geoChr   (CANON_UUID("00040002"));   // Position schreiben
BLEClientCharacteristic geoInd   (CANON_UUID("00040003"));   // Indications

static const uint8_t PAIR_ACCEPT = 0x02;
static const uint8_t PAIR_REJECT = 0x03;
static const uint8_t GEO_SUCCESS = 0x02;
static const uint8_t GEO_REQUEST = 0x03;
static const uint8_t MODE_SHOOT  = 0x02;

// ============================== Zustand ================================

struct GpsFix {
  bool     valid;
  double   lat, lon, alt, speed, course;
  uint32_t epoch;
  uint8_t  sats;     // fuer den Fix genutzt
  uint8_t  view;     // sichtbar (aus GSV)
};

enum class State { SCAN, PAIRING, CONNECTED, RUNNING };
static State state = State::SCAN;

static volatile bool     connPending  = false;
static volatile bool     disconnected = false;
static volatile uint8_t  discReason   = 0;
static volatile uint16_t connHandle   = BLE_CONN_HANDLE_INVALID;
static volatile int      pairResult   = -1;
static volatile bool     geoRequested = false;
static volatile bool     geoEnabled   = false;
static volatile bool     remoteSeen   = false;
static volatile uint32_t connectingSince = 0;
static volatile int      authStatus   = -1;     // Ergebnis des BLE-Pairings (0 = ok)   // Verbindungsaufbau laeuft (0 = nein)
static bool needConfirm   = false;
static bool waitingForUser = false;
static uint32_t lastGeoSend = 0, geoPacketsSent = 0;
// Kamera nimmt keine Daten mehr an (meist: ausgeschaltet) -> Pausen zwischen Neuverbindungen
static uint8_t  staleStreak = 0;
static uint32_t scanHoldUntil = 0;
static const uint32_t STALE_HOLD_MIN_MS = 5000, STALE_HOLD_MAX_MS = 30000;
static const uint32_t SHORT_CONN_MS = 60000;    // kuerzer verbunden = Kamera vermutlich aus
static uint32_t connectedAt = 0;
// Test: bekannte Kamera ohne Pairing-Abfrage verbinden (weckt die ausgeschaltete Kamera evtl. nicht)
static volatile bool listenOnly = false;   // nur zuhoeren, nicht verbinden (Pausen)
static bool listenForced = false;           // Diagnose: Befehl 'h'
static volatile uint32_t advCount = 0;      // Werbepakete der gespeicherten Kamera (Diagnose)
// Kamera an/aus an der Werbe-Rate erkennen (gemessen an der R6 Mark II):
//   aus ~0,8 Pakete/s, an ~15 Pakete/s, kurz nach Trennen ~25 s lang auch aus schnell.
// Mit ausgeschalteter Kamera verbinden weckt sie auf (Verschluss klickt) -> vermeiden.
static const uint32_t ADV_WIN_MS     = 3000;   // Messfenster
static const uint32_t ADV_FAST_MIN   = 9;      // >= Pakete pro Fenster = Kamera an
static const uint32_t AFTER_DISC_MS  = 45000;  // nach Trennen erst so lange "an" bestaetigen
static volatile uint32_t advWin = 0;
static volatile bool connectAllowed = false;
static bool camOn = false, camSeen = false;
static uint32_t lastDisconnectAt = 0;
// Getestet an der R6 Mark II: ohne Pairing-Abfrage keine GPS-Freigabe -> aus
static int8_t minimalOk = 0;           // -1 unbekannt (testen), 0 klappt nicht, 1 klappt
static bool   lastWasMinimal = false;

static uint8_t  secureFails = 0;

static char seenName[24] = "";
// Demo-Modus fuer Screenshots: Beispielwerte statt echter Position/Kamera (Befehl 'D')
static bool demoMode = false;
static const uint32_t DEMO_EPOCH = 1781950530;   // 2026-06-20 10:15:30 UTC
static char lastResetInfo[24] = "";   // letzter Absturz/Watchdog, fuer die Seite "Geraet"

// gespeicherte Daten (InternalFS)
struct Persist {
  uint32_t magic;
  uint8_t  uuid[16];
  uint8_t  haveCam;
  uint8_t  addr[6];
  uint8_t  addrType;
  char     camName[24];
};
static const uint32_t PERSIST_MAGIC = 0xCA9E0001;
static const char *PERSIST_FILE = "/canongps.bin";
static Persist persist;

// Einstellungen (eigene Datei, damit gespeichertes Pairing unberuehrt bleibt)
struct Settings {
  uint32_t magic;
  uint8_t  lang;
  uint8_t  intervalIdx;
  uint8_t  displayIdx;
  uint8_t  keylock;       // 0 = aus, 1 = automatisch
};
static const uint32_t SETTINGS_MAGIC_V1 = 0x5E770001;   // ohne Tastensperre
static const uint32_t SETTINGS_MAGIC = 0x5E770002;
static const uint8_t  DEFAULT_KEYLOCK = 1;
static const uint32_t LOCK_AFTER_MS   = 30000;   // Sperre nach letzter Eingabe
static const uint32_t UNLOCK_HOLD_MS  = 2000;    // zum Entsperren halten
static const char *SETTINGS_FILE = "/settings.bin";
static Settings settings = {SETTINGS_MAGIC, DEFAULT_LANG, DEFAULT_INTERVAL_IDX, DEFAULT_DISPLAY_IDX, DEFAULT_KEYLOCK};
static uint8_t &lang = settings.lang;
#define TR(de, en) (lang == LANG_EN ? (en) : (de))
#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

static uint32_t sendIntervalMs() { return SEND_INTERVALS_S[settings.intervalIdx] * 1000UL; }
static uint32_t displayOnMs()    { return DISPLAY_TIMES_S[settings.displayIdx] * 1000UL; }   // 0 = immer

static void backoff(const char *why) {
  if (staleStreak < 8) staleStreak++;
  uint32_t hold = STALE_HOLD_MIN_MS << (staleStreak - 1);
  if (hold > STALE_HOLD_MAX_MS) hold = STALE_HOLD_MAX_MS;
  scanHoldUntil = millis() + hold;
  Serial.printf("%s -> %s %lu s\n", why, lang == LANG_EN ? "pause" : "Pause", (unsigned long)(hold / 1000));
}

// =========================== Hilfsfunktionen ===========================

static void printHex(const uint8_t *d, size_t n) {
  for (size_t i = 0; i < n; i++) Serial.printf("%02x", d[i]);
}

static void savePersist() {
  InternalFS.remove(PERSIST_FILE);
  File f(InternalFS);
  if (f.open(PERSIST_FILE, FILE_O_WRITE)) {
    f.write((const uint8_t *)&persist, sizeof(persist));
    f.close();
  }
}

static void loadPersist() {
  InternalFS.begin();
  File f(InternalFS);
  bool ok = false;
  if (f.open(PERSIST_FILE, FILE_O_READ)) {
    ok = f.read((uint8_t *)&persist, sizeof(persist)) == sizeof(persist) && persist.magic == PERSIST_MAGIC;
    f.close();
  }
  if (!ok) {
    memset(&persist, 0, sizeof(persist));
    persist.magic = PERSIST_MAGIC;
    uint8_t got = 0;
    while (got < 16) {   // Zufallszahlen aus dem SoftDevice
      uint8_t avail = 0;
      sd_rand_application_bytes_available_get(&avail);
      if (avail) {
        uint8_t n = min<uint8_t>(avail, 16 - got);
        if (sd_rand_application_vector_get(persist.uuid + got, n) == NRF_SUCCESS) got += n;
      }
      delay(1);
    }
    savePersist();
  }
}

static void loadSettings() {
  InternalFS.begin();
  File f(InternalFS);
  Settings tmp;
  if (f.open(SETTINGS_FILE, FILE_O_READ)) {
    bool rd = f.read((uint8_t *)&tmp, sizeof(tmp)) == sizeof(tmp);
    if (rd && tmp.magic == SETTINGS_MAGIC_V1) {   // alte Datei uebernehmen
      tmp.magic = SETTINGS_MAGIC;
      tmp.keylock = DEFAULT_KEYLOCK;
    }
    if (rd && tmp.magic == SETTINGS_MAGIC && tmp.lang <= LANG_EN &&
        tmp.intervalIdx < COUNT(SEND_INTERVALS_S) && tmp.displayIdx < COUNT(DISPLAY_TIMES_S) &&
        tmp.keylock <= 1)
      settings = tmp;
    f.close();
  }
}

static void saveSettings() {
  InternalFS.remove(SETTINGS_FILE);
  File f(InternalFS);
  if (f.open(SETTINGS_FILE, FILE_O_WRITE)) { f.write((const uint8_t *)&settings, sizeof(settings)); f.close(); }
}

static void showToast(const char *msg);

static void toggleLang() {
  lang = lang == LANG_EN ? LANG_DE : LANG_EN;
  saveSettings();
  const char *msg = TR("Sprache: Deutsch", "Language: English");
  Serial.println(msg);
  showToast(msg);
}

static void forgetCamera() {
  Bluefruit.Central.clearBonds();
  persist.haveCam = 0;
  persist.camName[0] = 0;
  savePersist();
  needConfirm = false;
  Serial.println(TR("Pairing geloescht. Kamera muss neu gepairt werden.", "Pairing deleted. Camera must be paired again."));
}

static void addrToStr(const uint8_t *a, char *out) {   // Bluefruit: Byte 0 = LSB
  sprintf(out, "%02x:%02x:%02x:%02x:%02x:%02x", a[5], a[4], a[3], a[2], a[1], a[0]);
}

// ============================ GPS-Quelle ===============================

static TinyGPSPlus gps;
// Sichtbare Satelliten je System (GSV-Feld 3), L76K-Talker: GP, BD/GB, GL, GA
static TinyGPSCustom gsvGP(gps, "GPGSV", 3), gsvBD(gps, "BDGSV", 3), gsvGB(gps, "GBGSV", 3),
                     gsvGL(gps, "GLGSV", 3), gsvGA(gps, "GAGSV", 3);

static uint8_t satsInView() {
  TinyGPSCustom *all[] = {&gsvGP, &gsvBD, &gsvGB, &gsvGL, &gsvGA};
  int n = 0;
  for (TinyGPSCustom *c : all)
    if (c->isValid() && c->age() < 5000) n += atoi(c->value());
  return n > 255 ? 255 : n;
}

static const char *gnssStartText();

// --- Zeit-Hilfen ---
static int32_t daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int32_t)doe - 719468;
}

// --- L76K: Pins/Wakeup automatisch finden ---
// Meshtastic-Quellen widersprechen sich bei RX/TX und beim Wakeup-Pegel.
struct GnssCfg { uint8_t rx, tx; uint8_t wake; };
static const GnssCfg GNSS_CFGS[] = {
  {PIN_SERIAL1_RX, PIN_SERIAL1_TX, HIGH},
  {PIN_SERIAL1_RX, PIN_SERIAL1_TX, LOW},
  {PIN_SERIAL1_TX, PIN_SERIAL1_RX, HIGH},
  {PIN_SERIAL1_TX, PIN_SERIAL1_RX, LOW},
};
static int gnssCfgIdx = 0;
static bool gnssFound = false;
static uint32_t gnssCfgSince = 0, gnssChars = 0;

static void sendPCAS(const char *body) {     // $<body>*CS\r\n
  uint8_t cs = 0;
  for (const char *p = body; *p; p++) cs ^= (uint8_t)*p;
  Serial1.printf("$%s*%02X\r\n", body, cs);
}

static void gnssApplyCfg() {
  const GnssCfg &c = GNSS_CFGS[gnssCfgIdx];
  digitalWrite(PIN_GNSS_WAKEUP, c.wake);
  // end() nur nach begin(): der Core wartet sonst endlos auf TXSTOPPED
  static bool uartStarted = false;
  if (uartStarted) Serial1.end();
  uartStarted = true;
  Serial1.setPins(c.rx, c.tx);
  Serial1.begin(GPS_BAUD);
  gnssCfgSince = millis();
  gnssChars = 0;
}

static void gnssConfigure() {
  sendPCAS("PCAS04,7");     // GPS + BeiDou + GLONASS
  delay(50);
  sendPCAS("PCAS03,1,0,0,1,1,0,0,0,0,0,,,0,0");   // GGA + GSV + RMC ausgeben
}

static void gpsBegin() {
  pinMode(PIN_GNSS_WAKEUP, OUTPUT);
  gnssApplyCfg();
}

static bool gpsTimeValid() {
  return gps.date.isValid() && gps.time.isValid() && gps.date.year() >= 2024 &&
         gps.time.age() < GPS_MAX_AGE_MS;
}

// Ortszeit Mitteleuropa: MEZ (UTC+1), MESZ (UTC+2) vom letzten Sonntag im Maerz
// bis zum letzten Sonntag im Oktober, jeweils 01:00 UTC
static const int32_t TZ_STD_OFFSET_S = 3600;
static const bool    TZ_EU_DST       = true;

static int32_t lastSundayDays(int y, int m) {          // Tage seit 1970 des letzten Sonntags
  int32_t d = daysFromCivil(y, m, 31);
  return d - (d + 4) % 7;                               // 1970-01-01 war ein Donnerstag
}

static uint32_t localEpoch(uint32_t utc) {
  int32_t off = TZ_STD_OFFSET_S;
  if (TZ_EU_DST) {
    time_t tt = utc;
    struct tm t;
    gmtime_r(&tt, &t);
    int y = t.tm_year + 1900;
    uint32_t start = (uint32_t)lastSundayDays(y, 3) * 86400UL + 3600;
    uint32_t end   = (uint32_t)lastSundayDays(y, 10) * 86400UL + 3600;
    if (utc >= start && utc < end) off += 3600;
  }
  return utc + off;
}

static uint32_t gpsEpoch() {
  return (uint32_t)daysFromCivil(gps.date.year(), gps.date.month(), gps.date.day()) * 86400UL +
         gps.time.hour() * 3600UL + gps.time.minute() * 60UL + gps.time.second();
}

static void gpsUpdate() {
  while (Serial1.available()) {
    char c = Serial1.read();
    gnssChars++;
    gps.encode(c);
  }
  // Konfiguration fixieren, sobald gueltige NMEA-Saetze kommen
  if (!gnssFound && gps.passedChecksum() > 2) {
    gnssFound = true;
    const GnssCfg &c = GNSS_CFGS[gnssCfgIdx];
    Serial.printf(TR("GNSS gefunden: RX=D%u TX=D%u Wakeup=%s\n", "GNSS found: RX=D%u TX=D%u wakeup=%s\n"), c.rx, c.tx, c.wake ? "HIGH" : "LOW");
    gnssConfigure();
  }
  if (!gnssFound && millis() - gnssCfgSince > 4000) {
    gnssCfgIdx = (gnssCfgIdx + 1) % (int)(sizeof(GNSS_CFGS) / sizeof(GNSS_CFGS[0]));
    Serial.printf(TR("GNSS: keine Daten (%lu Zeichen), probiere Konfiguration %d\n", "GNSS: no data (%lu chars), trying configuration %d\n"),
                  (unsigned long)gnssChars, gnssCfgIdx);
    gnssApplyCfg();
  }
}

static const char *gnssStartText() {
  // alle 4 Pin-/Wakeup-Varianten (je 4 s) durchprobiert und nie Daten? -> Modul fehlt
  if (millis() < 20000) return TR("GNSS startet...", "GNSS starting...");
  return TR("GNSS antwortet nicht", "GNSS not responding");
}

static GpsFix gpsRead() {
  GpsFix f = {};
  if (demoMode) {                       // Brandenburger Tor, Berlin
    f.valid = true; f.lat = 52.516275; f.lon = 13.377704; f.alt = 34.6;
    f.speed = 1.3; f.course = 82; f.sats = 11; f.view = 18; f.epoch = DEMO_EPOCH;
    return f;
  }
  f.sats = gps.satellites.isValid() ? gps.satellites.value() : 0;
  f.view = satsInView();
  if (!gps.location.isValid() || gps.location.age() > GPS_MAX_AGE_MS || !gpsTimeValid()) return f;
  f.valid  = true;
  f.lat    = gps.location.lat();
  f.lon    = gps.location.lng();
  f.alt    = gps.altitude.isValid() ? gps.altitude.meters() : 0;
  f.speed  = gps.speed.isValid() ? gps.speed.mps() : 0;
  f.course = gps.course.isValid() ? gps.course.deg() : 0;
  f.epoch  = gpsEpoch();
  return f;
}

static void printFix(const GpsFix &f) {
  if (!f.valid) { Serial.printf(TR("GPS: kein Fix (%u/%u Sat. genutzt/sichtbar)\n", "GPS: no fix (%u/%u sats used/in view)\n"), f.sats, f.view); return; }
  time_t tt = f.epoch;
  struct tm tmv;
  gmtime_r(&tt, &tmv);
  Serial.printf(TR("GPS: %.6f, %.6f, %.1f m, %04d-%02d-%02d %02d:%02d:%02d UTC | %.1f km/h, Kurs %.0f, %u Sat.\n",
                   "GPS: %.6f, %.6f, %.1f m, %04d-%02d-%02d %02d:%02d:%02d UTC | %.1f km/h, course %.0f, %u sats\n"),
                f.lat, f.lon, f.alt, tmv.tm_year + 1900, tmv.tm_mon + 1,
                tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec, f.speed * 3.6, f.course, f.sats);
}

// ============================ Batterie =================================

static uint16_t batMv = 0;
static int batPct = -1;

static void batteryUpdate() {
  static uint32_t last = 0;
  if (last && millis() - last < 10000) return;
  last = millis();
  digitalWrite(PIN_VBAT_ENABLE, HIGH);
  analogReadResolution(12);
  uint32_t sum = 0;
  for (int i = 0; i < 8; i++) sum += analogRead(PIN_VBAT);
  float mv = (sum / 8.0f) * 3600.0f / 4095.0f * VBAT_DIVIDER;
  batMv = (uint16_t)mv;
  // Ruhespannungs-Kurve (aus Meshtastic-Variant), 100 % ... 0 % in 10er-Schritten
  static const uint16_t OCV[] = {4200, 3876, 3826, 3763, 3713, 3660, 3573, 3485, 3422, 3359, 3300};
  if (mv >= OCV[0]) batPct = 100;
  else if (mv <= OCV[10]) batPct = 0;
  else {
    for (int i = 0; i < 10; i++) {
      if (mv <= OCV[i] && mv > OCV[i + 1]) {
        batPct = 100 - i * 10 - (int)(10.0f * (OCV[i] - mv) / (OCV[i] - OCV[i + 1]));
        break;
      }
    }
  }
}

// =========================== LoRa schlafen =============================

static void loraSleep() {
  pinMode(PIN_LORA_CS, OUTPUT);
  digitalWrite(PIN_LORA_CS, HIGH);
  pinMode(PIN_LORA_RXEN, OUTPUT);
  digitalWrite(PIN_LORA_RXEN, LOW);
  pinMode(PIN_LORA_BUSY, INPUT);
  pinMode(PIN_LORA_RESET, OUTPUT);
  digitalWrite(PIN_LORA_RESET, LOW);
  delay(2);
  digitalWrite(PIN_LORA_RESET, HIGH);
  uint32_t t0 = millis();
  while (digitalRead(PIN_LORA_BUSY) && millis() - t0 < 100) delay(1);
  SPI.begin();
  SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  digitalWrite(PIN_LORA_CS, LOW);
  SPI.transfer(0x84);   // SetSleep
  SPI.transfer(0x00);   // Cold start, kein RTC-Wakeup
  digitalWrite(PIN_LORA_CS, HIGH);
  SPI.endTransaction();
  SPI.end();
}

// ============================== Display ================================

#if OLED_SH1106
static U8G2_SH1106_128X64_NONAME_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE);
#else
static U8G2_SSD1306_128X64_NONAME_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE);
#endif
static bool oledPresent = false, oledOn = false;
static uint32_t oledOnUntil = 0;
static char toastMsg[24] = "";
static uint32_t toastUntil = 0;

static bool i2cProbe(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

static void displayBegin() {
  // Adresse und ggf. vertauschte SDA/SCL automatisch finden
  const uint8_t pins[2][2] = {{PIN_WIRE_SDA, PIN_WIRE_SCL}, {PIN_WIRE_SCL, PIN_WIRE_SDA}};
  for (int p = 0; p < 2 && !oledPresent; p++) {
    Wire.end();
    Wire.setPins(pins[p][0], pins[p][1]);
    Wire.begin();
    static const uint8_t addrs[] = {0x3C, 0x3D};
    for (uint8_t a : addrs) {
      if (i2cProbe(a)) {
        oled.setI2CAddress(a << 1);
        oledPresent = true;
        Serial.printf(TR("OLED gefunden: 0x%02X (SDA=D%u SCL=D%u)\n", "OLED found: 0x%02X (SDA=D%u SCL=D%u)\n"), a, pins[p][0], pins[p][1]);
        break;
      }
    }
  }
  if (!oledPresent) { Serial.println(TR("Kein OLED gefunden.", "No OLED found.")); return; }
  oled.begin();
  oled.setFont(u8g2_font_6x10_tf);
  oledOn = true;
  oledOnUntil = millis() + displayOnMs();
}

static void displayWake() {
  oledOnUntil = millis() + displayOnMs();
  if (oledPresent && !oledOn) { oled.setPowerSave(0); oledOn = true; }
}

static void showToast(const char *msg) {
  strncpy(toastMsg, msg, sizeof(toastMsg) - 1);
  toastUntil = millis() + 2500;
  displayWake();
}

static const char *stateText() {
  if (toastMsg[0] && (int32_t)(millis() - toastUntil) < 0) return toastMsg;
  if (demoMode) return TR("GPS -> Kamera", "GPS -> camera");
  if (remoteSeen && state == State::SCAN) return TR("Fernbed.-Modus!", "Remote mode!");
  if (state == State::SCAN && persist.haveCam && camSeen && !camOn) return TR("Kamera aus", "Camera off");
  if (state == State::SCAN && persist.haveCam && camOn && !connectAllowed) return TR("Kamera an, warte...", "Camera on, waiting...");
  switch (state) {
    case State::SCAN:      return TR("Suche Kamera...", "Searching camera...");
    case State::PAIRING:   return waitingForUser ? TR("An Kamera OK!", "Confirm on camera!") : TR("Verbinde...", "Connecting...");
    case State::CONNECTED: return TR("Verbunden", "Connected");
    case State::RUNNING:   return TR("GPS -> Kamera", "GPS -> camera");
  }
  return "";
}

// ---------------------------- Tastensperre ----------------------------

static bool locked = false;
static uint32_t lastInputMs = 0;

static void noteInput() { lastInputMs = millis(); }

static void keyLockUpdate() {
  if (settings.keylock && !locked && millis() - lastInputMs > LOCK_AFTER_MS) {
    locked = true;
    Serial.println(TR("Tastensperre aktiv.", "Key lock active."));
  }
}

static void unlockKeys() {
  locked = false;
  noteInput();
  Serial.println(TR("Tasten entsperrt.", "Keys unlocked."));
  showToast(TR("Entsperrt", "Unlocked"));
}

static void lockKeys() {
  locked = true;
  Serial.println(TR("Tastensperre aktiv.", "Key lock active."));
  showToast(TR("Gesperrt", "Locked"));
}

static void lockedHint() {
  showToast(TR("Gesperrt: Taste 2 s", "Locked: button 2 s"));
}

// ------------------------- Seiten & Menue -----------------------------

enum Page : uint8_t { PAGE_STATUS, PAGE_GPS, PAGE_DEVICE, PAGE_SETTINGS, PAGE_COUNT };
enum MenuItem : uint8_t { MENU_LANG, MENU_INTERVAL, MENU_DISPLAY, MENU_KEYLOCK, MENU_UNPAIR, MENU_COUNT };
static uint8_t page = PAGE_STATUS;
static uint8_t menuSel = 0;
static bool confirmUnpair = false;

static void drawHeader(const char *title) {
  char b[8];
  oled.drawStr(OX, 9, title);
  if (locked) {                                   // kleines Schloss hinter dem Titel
    int lx = OX + oled.getStrWidth(title) + 4;
    oled.drawFrame(lx + 1, 1, 5, 5);
    oled.drawBox(lx, 4, 7, 6);
  }
  int x = 128;
  if (batPct >= 0) {
    snprintf(b, sizeof(b), "%d%%", batPct);
    x -= oled.getStrWidth(b);
    oled.drawStr(x, 9, b);
  }
  // Seitenpunkte
  for (int i = 0; i < PAGE_COUNT; i++) {
    int px = x - 6 - (PAGE_COUNT - i) * 5;
    if (i == page) oled.drawBox(px, 4, 3, 3);
    else oled.drawPixel(px + 1, 5);
  }
  oled.drawHLine(OX, 11, 128 - OX);
}

// Grad/Minuten/Sekunden, z.B. N 48° 05' 07.2"  (0xB0 = Gradzeichen im u8g2-Font)
static void formatDms(char *out, size_t n, double v, bool isLat) {
  char hemi = isLat ? (v < 0 ? 'S' : 'N') : (v < 0 ? 'W' : 'E');
  uint32_t t = (uint32_t)lround(fabs(v) * 36000.0);   // Zehntelsekunden, gerundet
  uint32_t deg = t / 36000, rem = t % 36000;
  uint32_t min = rem / 600, ds = rem % 600;
  snprintf(out, n, "%c %lu\xb0 %02lu' %02lu.%lu\"", hemi, (unsigned long)deg, (unsigned long)min,
           (unsigned long)(ds / 10), (unsigned long)(ds % 10));
}

static void drawPageStatus(const GpsFix &f) {
  char line[32];
  drawHeader("Canon GPS");
  oled.drawStr(OX, 22, stateText());
  oled.drawStr(OX, 32, demoMode ? "EOSR6m2_123456" : persist.haveCam ? persist.camName : (seenName[0] ? seenName : "-"));
  if (!gnssFound) {
    snprintf(line, sizeof(line), "%s", gnssStartText());
  } else if (f.valid) {
    time_t tt = localEpoch(f.epoch);   // Ortszeit (MEZ/MESZ)
    struct tm tmv;
    gmtime_r(&tt, &tmv);
    snprintf(line, sizeof(line), "Sat %u/%u  %02d:%02d:%02d", f.sats, f.view, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
  } else {
    snprintf(line, sizeof(line), "Sat %u/%u  %s", f.sats, f.view, TR("kein Fix", "no fix"));
  }
  oled.drawStr(OX, 42, line);
  if (f.valid) {
    formatDms(line, sizeof(line), f.lat, true);
    oled.drawStr(OX, 52, line);
    formatDms(line, sizeof(line), f.lon, false);
    oled.drawStr(OX, 62, line);
  }
}

static void drawPageGps(const GpsFix &f) {
  char line[32];
  drawHeader("GPS");
  snprintf(line, sizeof(line), TR("Satelliten: %u/%u", "Satellites: %u/%u"), f.sats, f.view);
  oled.drawStr(OX, 22, line);
  if (demoMode) snprintf(line, sizeof(line), "HDOP: 0.8");
  else if (gps.hdop.isValid()) snprintf(line, sizeof(line), "HDOP: %.1f", gps.hdop.hdop());
  else snprintf(line, sizeof(line), "HDOP: -");
  oled.drawStr(OX, 32, line);
  if (f.valid) {
    snprintf(line, sizeof(line), TR("Hoehe: %.0f m", "Altitude: %.0f m"), f.alt);
    oled.drawStr(OX, 42, line);
    snprintf(line, sizeof(line), "%.1f km/h  %03.0f%c", f.speed * 3.6, f.course, 0xb0);
    oled.drawStr(OX, 52, line);
    snprintf(line, sizeof(line), TR("Fix-Alter: %.1f s", "Fix age: %.1f s"), demoMode ? 0.4 : gps.location.age() / 1000.0);
  } else {
    snprintf(line, sizeof(line), "%s", gnssFound ? TR("kein Fix", "no fix") : gnssStartText());
  }
  oled.drawStr(OX, 62, line);
}

static void drawPageDevice() {
  char line[32];
  drawHeader(TR("Geraet", "Device"));
  snprintf(line, sizeof(line), TR("Akku: %.2f V", "Battery: %.2f V"), batMv / 1000.0);
  oled.drawStr(OX, 22, line);
  snprintf(line, sizeof(line), TR("Pakete: %lu", "Packets: %lu"), (unsigned long)geoPacketsSent);
  oled.drawStr(OX, 32, line);
  uint32_t up = millis() / 1000;
  snprintf(line, sizeof(line), TR("Laufzeit: %lu:%02lu:%02lu", "Uptime: %lu:%02lu:%02lu"),
           (unsigned long)(up / 3600), (unsigned long)(up / 60 % 60), (unsigned long)(up % 60));
  oled.drawStr(OX, 42, line);
  if (demoMode) {
    oled.drawStr(OX, 52, "aa:bb:cc:11:22:33");
  } else if (persist.haveCam) {
    addrToStr(persist.addr, line);
    oled.drawStr(OX, 52, line);
  } else {
    oled.drawStr(OX, 52, TR("Keine Kamera gepairt", "No camera paired"));
  }
  if (lastResetInfo[0] && !demoMode) snprintf(line, sizeof(line), "%s", lastResetInfo);
  else snprintf(line, sizeof(line), "%s %s", TR("Name:", "Name:"), DEVICE_NAME);
  oled.drawStr(OX, 62, line);
}

static void menuValue(uint8_t item, char *out, size_t n) {
  switch (item) {
    case MENU_LANG:     snprintf(out, n, "%s", TR("Deutsch", "English")); break;
    case MENU_INTERVAL: snprintf(out, n, "%u s", SEND_INTERVALS_S[settings.intervalIdx]); break;
    case MENU_DISPLAY:
      if (DISPLAY_TIMES_S[settings.displayIdx]) snprintf(out, n, "%u s", DISPLAY_TIMES_S[settings.displayIdx]);
      else snprintf(out, n, "%s", TR("immer", "always"));
      break;
    case MENU_KEYLOCK:  snprintf(out, n, "%s", settings.keylock ? "Auto" : TR("Aus", "Off")); break;
    default: out[0] = 0; break;
  }
}

static const char *menuLabel(uint8_t item) {
  switch (item) {
    case MENU_LANG:     return TR("Sprache", "Language");
    case MENU_INTERVAL: return TR("Intervall", "Interval");
    case MENU_DISPLAY:  return "Display";
    case MENU_KEYLOCK:  return TR("Tastensperre", "Key lock");
    case MENU_UNPAIR:   return TR("Pairing loeschen", "Delete pairing");
  }
  return "";
}

static void drawPageSettings() {
  drawHeader(TR("Einstellungen", "Settings"));
  if (confirmUnpair) {
    oled.drawStr(OX, 26, TR("Pairing loeschen?", "Delete pairing?"));
    oled.drawStr(OX, 42, TR("Druecken = Ja", "Press = yes"));
    oled.drawStr(OX, 54, TR("Sonst = Nein", "Other = no"));
    return;
  }
  char val[16];
  for (uint8_t i = 0; i < MENU_COUNT; i++) {
    int y = 21 + i * 10;
    if (i == menuSel) {
      oled.drawBox(OX, y - 8, 128 - OX, 10);
      oled.setDrawColor(0);
    }
    oled.drawStr(OX + 2, y, menuLabel(i));
    menuValue(i, val, sizeof(val));
    if (val[0]) oled.drawStr(126 - oled.getStrWidth(val), y, val);
    oled.setDrawColor(1);
  }
}

static void displayDraw() {
  GpsFix f = gpsRead();
  oled.clearBuffer();
  switch (page) {
    case PAGE_STATUS:   drawPageStatus(f); break;
    case PAGE_GPS:      drawPageGps(f); break;
    case PAGE_DEVICE:   drawPageDevice(); break;
    case PAGE_SETTINGS: drawPageSettings(); break;
  }
}

// Bildspeicher (8 Seiten x 128 Byte, je Byte 8 Pixel senkrecht, Bit 0 oben) als Hex ausgeben.
// tools/fb2png.py macht daraus ein PNG.
static void displayDump() {
  if (!oledPresent) { Serial.println("FB-NONE"); return; }
  displayDraw();
  oled.sendBuffer();
  const uint8_t *buf = oled.getBufferPtr();
  Serial.printf("FB-BEGIN page=%u\n", page);
  for (int row = 0; row < 8; row++) {
    for (int x = 0; x < 128; x++) Serial.printf("%02x", buf[row * 128 + x]);
    Serial.println();
  }
  Serial.println("FB-END");
}

static void displayUpdate() {
  if (!oledPresent) return;
  uint32_t now = millis();
  if (oledOn && displayOnMs() && (int32_t)(now - oledOnUntil) > 0) {
    oled.setPowerSave(1);
    oledOn = false;
    page = PAGE_STATUS;          // beim naechsten Einschalten wieder Status
    confirmUnpair = false;
  }
  static uint32_t last = 0;
  if (!oledOn || now - last < 250) return;
  last = now;
  displayDraw();
  oled.sendBuffer();
}

static void menuActivate() {
  switch (menuSel) {
    case MENU_LANG:
      toggleLang();
      break;
    case MENU_INTERVAL:
      settings.intervalIdx = (settings.intervalIdx + 1) % COUNT(SEND_INTERVALS_S);
      saveSettings();
      Serial.printf(TR("Sendeintervall: %u s\n", "Send interval: %u s\n"), SEND_INTERVALS_S[settings.intervalIdx]);
      break;
    case MENU_DISPLAY:
      settings.displayIdx = (settings.displayIdx + 1) % COUNT(DISPLAY_TIMES_S);
      saveSettings();
      displayWake();
      Serial.printf(TR("Display-Zeit: %u s (0 = immer)\n", "Display time: %u s (0 = always)\n"),
                    DISPLAY_TIMES_S[settings.displayIdx]);
      break;
    case MENU_KEYLOCK:
      settings.keylock = !settings.keylock;
      saveSettings();
      if (!settings.keylock) locked = false;
      Serial.printf(TR("Tastensperre: %s\n", "Key lock: %s\n"), settings.keylock ? "Auto" : TR("aus", "off"));
      break;
    case MENU_UNPAIR:
      confirmUnpair = true;
      break;
  }
}

// ------------------------------ Joystick ------------------------------

enum JoyDir : uint8_t { JOY_UP, JOY_DOWN, JOY_LEFT, JOY_RIGHT, JOY_PRESS, JOY_COUNT };
static const uint8_t JOY_PINS[JOY_COUNT] = {PIN_JOY_UP, PIN_JOY_DOWN, PIN_JOY_LEFT, PIN_JOY_RIGHT, PIN_JOY_PRESS};

static void joystickBegin() {
  for (uint8_t i = 0; i < JOY_COUNT; i++) pinMode(JOY_PINS[i], INPUT_PULLUP);
}

static void handleJoy(uint8_t d) {
  static const char *namesDe[] = {"hoch", "runter", "links", "rechts", "druecken"};
  static const char *namesEn[] = {"up", "down", "left", "right", "press"};
  Serial.printf("Joystick: %s\n", lang == LANG_EN ? namesEn[d] : namesDe[d]);

  if (locked) { lockedHint(); return; }    // gesperrt: nur Hinweis anzeigen
  noteInput();
  bool wasOff = oledPresent && !oledOn;
  displayWake();
  if (wasOff) return;                      // erste Bewegung schaltet nur ein

  if (confirmUnpair) {
    confirmUnpair = false;
    if (d == JOY_PRESS) {
      if (Bluefruit.connected(connHandle)) Bluefruit.disconnect(connHandle);
      forgetCamera();
      showToast(TR("Pairing geloescht", "Pairing deleted"));
      page = PAGE_STATUS;
    }
    return;
  }
  switch (d) {
    case JOY_LEFT:  page = (page + PAGE_COUNT - 1) % PAGE_COUNT; break;
    case JOY_RIGHT: page = (page + 1) % PAGE_COUNT; break;
    case JOY_UP:    if (page == PAGE_SETTINGS) menuSel = (menuSel + MENU_COUNT - 1) % MENU_COUNT; break;
    case JOY_DOWN:  if (page == PAGE_SETTINGS) menuSel = (menuSel + 1) % MENU_COUNT; break;
    case JOY_PRESS: if (page == PAGE_SETTINGS) menuActivate(); break;
  }
}

static void joystickUpdate() {
  static uint8_t stable = 0, lastRaw = 0;
  static uint32_t changedAt = 0;
  uint8_t raw = 0;
  for (uint8_t i = 0; i < JOY_COUNT; i++)
    if (digitalRead(JOY_PINS[i]) == LOW) raw |= 1 << i;
  uint32_t now = millis();
  if (raw != lastRaw) { lastRaw = raw; changedAt = now; }
  if (now - changedAt < 30 || raw == stable) return;      // entprellen
  uint8_t pressed = raw & ~stable;
  stable = raw;
  // nur eindeutige Einzelrichtung auswerten (Druecken hat Vorrang)
  if (pressed & (1 << JOY_PRESS)) { handleJoy(JOY_PRESS); return; }
  for (uint8_t i = 0; i < JOY_PRESS; i++)
    if (pressed == (1 << i)) { handleJoy(i); return; }
}

// ======================= Kamera an/aus erkennen ========================

static void camPowerUpdate() {
  static uint32_t t0 = 0;
  uint32_t now = millis();
  if (now - t0 >= ADV_WIN_MS) {
    uint32_t n = advWin;
    advWin = 0;
    t0 = now;
    bool seen = n > 0, on = n >= ADV_FAST_MIN;
    if (state == State::SCAN && persist.haveCam && (on != camOn || seen != camSeen)) {
      Serial.printf(TR("Kamera %s (%lu Werbepakete/3 s)\n", "Camera %s (%lu adverts/3 s)\n"),
                    on ? TR("an", "on") : (seen ? TR("aus", "off") : TR("nicht in Reichweite", "out of range")),
                    (unsigned long)n);
    }
    camOn = on;
    camSeen = seen;
  }
  bool settled = lastDisconnectAt == 0 || now - lastDisconnectAt > AFTER_DISC_MS;
  connectAllowed = camOn && settled && !listenForced && (int32_t)(now - scanHoldUntil) >= 0;
}

// ================================ LED ==================================

static void ledUpdate() {
  uint32_t t = millis();
  bool on = false;
  switch (state) {
    case State::SCAN:      on = (t % 1000) < 60; break;
    case State::PAIRING:   on = waitingForUser ? (t % 200) < 100 : (t % 500) < 60; break;
    case State::CONNECTED:
    case State::RUNNING: {
      uint32_t p = t % 5000;
      bool fix = gpsRead().valid;
      on = p < 40 || (!fix && p >= 200 && p < 240);
      break;
    }
  }
  digitalWrite(LED_BUILTIN, on ? LED_STATE_ON : !LED_STATE_ON);
}

// =============================== Taste =================================

// Kleine Taste = Tastensperre: kurz = sperren, 2 s halten = entsperren
static void buttonUpdate() {
  static bool wasDown = false, longDone = false, wokeDisplay = false;
  static uint32_t downSince = 0;
  bool down = digitalRead(PIN_BUTTON1) == LOW;
  uint32_t now = millis();
  if (down && !wasDown) {
    downSince = now;
    longDone = false;
    Serial.println(TR("Taste gedrueckt", "Button pressed"));
    wokeDisplay = oledPresent && !oledOn;          // dunkles Display: Druck schaltet nur ein
    if (locked) lockedHint(); else { noteInput(); displayWake(); }
  }
  if (down && !longDone && locked && now - downSince >= UNLOCK_HOLD_MS) {
    longDone = true;
    unlockKeys();
  }
  if (!down && wasDown && !longDone && !locked) {
    uint32_t held = now - downSince;
    if (held > 30 && held < 2000 && !wokeDisplay) lockKeys();   // kurzer Druck -> sperren
  }
  wasDown = down;
}

// =========================== BLE-Callbacks =============================

static void onNameIndication(BLEClientCharacteristic *, uint8_t *data, uint16_t len) {
  if (!len) return;
  Serial.print("<- Pairing-Indication: ");
  printHex(data, len);
  Serial.println();
  pairResult = data[0];
}

static void onGeoIndication(BLEClientCharacteristic *, uint8_t *data, uint16_t len) {
  if (!len) return;
  Serial.print("<- GPS-Indication: ");
  printHex(data, len);
  Serial.println();
  if (data[0] == GEO_REQUEST) geoRequested = true;
  else if (data[0] == GEO_SUCCESS) geoEnabled = true;
}

static void onPairComplete(uint16_t, uint8_t status) {
  authStatus = status;
}

static void onConnect(uint16_t h) {
  connHandle = h;
  disconnected = false;
  connPending = true;
  connectingSince = 0;
  Serial.println(TR("BLE verbunden.", "BLE connected."));
}

static void onDisconnect(uint16_t h, uint8_t reason) {
  // Nur wenn die Kamera bei bestehender Verbindung trennt (z.B. beim Ausschalten):
  // dann sendet sie noch ~25 s schnell, obwohl sie aus ist.
  if (reason == BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION &&
      (state == State::CONNECTED || state == State::RUNNING))
    lastDisconnectAt = millis() | 1;
  (void)h;
  discReason = reason;
  disconnected = true;
  connHandle = BLE_CONN_HANDLE_INVALID;
  connectingSince = 0;
  Serial.printf(TR("BLE getrennt (Grund 0x%02x).\n", "BLE disconnected (reason 0x%02x).\n"), reason);
}

static void onScan(ble_gap_evt_adv_report_t *r) {
  bool smart = Bluefruit.Scanner.checkReportForUuid(r, priSvc.uuid);
  bool remote = Bluefruit.Scanner.checkReportForUuid(r, remoteSvc.uuid);
  uint8_t buf[32];
  uint8_t n = Bluefruit.Scanner.parseReportByType(r, BLE_GAP_AD_TYPE_MANUFACTURER_SPECIFIC_DATA, buf, sizeof(buf));
  bool canon = smart || remote || (n >= 2 && buf[0] == 0xA9 && buf[1] == 0x01);
  if (canon) {
    // Diagnose: Werbedaten bei Aenderung ausgeben (an/aus erkennbar?)
    static uint8_t lastRaw[31];
    static uint8_t lastRawLen = 0;
    uint8_t rl = r->data.len > 31 ? 31 : r->data.len;
    if (rl != lastRawLen || memcmp(lastRaw, r->data.p_data, rl) != 0) {
      memcpy(lastRaw, r->data.p_data, rl);
      lastRawLen = rl;
      Serial.printf("ADV %s typ=%u rssi=%d: ", smart ? "smart" : (remote ? "remote" : "canon"),
                    (unsigned)(r->type.connectable | (r->type.scannable << 1) | (r->type.directed << 2)), r->rssi);
      printHex(r->data.p_data, rl);
      Serial.println();
    }
    char name[24] = "";
    if (Bluefruit.Scanner.parseReportByType(r, BLE_GAP_AD_TYPE_COMPLETE_LOCAL_NAME, (uint8_t *)name, sizeof(name) - 1))
      strncpy(seenName, name, sizeof(seenName) - 1);
    bool stored = persist.haveCam && memcmp(r->peer_addr.addr, persist.addr, 6) == 0;
    if (stored) { advCount++; advWin++; }
    if (listenOnly || (stored && !connectAllowed)) { Bluefruit.Scanner.resume(); return; }
    if (smart && (stored || !persist.haveCam)) {
      remoteSeen = false;
      if (Bluefruit.Central.connect(r)) {
        connectingSince = millis() | 1;
        return;
      }
    }
    if (remote && !smart) remoteSeen = true;
  }
  Bluefruit.Scanner.resume();
}

// ===================== Pairing & GPS-Handshake =========================

static bool writeChr(BLEClientCharacteristic &c, const char *name, const uint8_t *d, uint16_t len) {
  bool ok = c.write_resp(d, len) == len;
  Serial.printf("  -> %s: ", name);
  printHex(d, len);
  Serial.println(ok ? "  ok" : TR("  FEHLER", "  ERROR"));
  return ok;
}

static bool writePrefixed(BLEClientCharacteristic &c, const char *name, uint8_t prefix,
                          const uint8_t *d, uint16_t len) {
  uint8_t buf[40];
  if (len + 1 > sizeof(buf)) return false;
  buf[0] = prefix;
  memcpy(buf + 1, d, len);
  return writeChr(c, name, buf, len + 1);
}

static bool fail(const char *msg) {
  Serial.println(msg);
  if (Bluefruit.connected(connHandle)) Bluefruit.disconnect(connHandle);
  return false;
}

static bool connectAndPair() {
  uint16_t h = connHandle;
  pairResult = -1;
  geoRequested = geoEnabled = false;
  waitingForUser = false;

  BLEConnection *conn = Bluefruit.Connection(h);
  if (!conn) return fail(TR("Keine Verbindung.", "No connection."));
  ble_gap_addr_t peer = conn->getPeerAddr();
  bool stored = persist.haveCam && memcmp(peer.addr, persist.addr, 6) == 0;
  bool known = stored && !needConfirm;

  Serial.println(TR("Starte Verschluesselung/Bonding ...", "Starting encryption/bonding ..."));
  // Bluefruit verschluesselt mit vorhandenem Bond automatisch beim Verbinden.
  // requestPairing() stoesst ein neues Pairing nur an und wartet NICHT -> selbst warten.
  uint32_t ts = millis();
  while (!conn->secured() && !disconnected && millis() - ts < 1500) delay(10);
  bool secOk = conn->secured();
  if (!secOk && !disconnected) {
    authStatus = -1;
    if (conn->requestPairing()) {
      ts = millis();
      while (authStatus == -1 && !disconnected && millis() - ts < 15000) delay(10);
      secOk = authStatus == BLE_GAP_SEC_STATUS_SUCCESS && conn->secured();
      if (authStatus > 0) Serial.printf(TR("Pairing-Status: 0x%02x\n", "Pairing status: 0x%02x\n"), authStatus);
    }
  }
  if (!secOk || disconnected) {
    secureFails++;
    Serial.printf(TR("Verschluesselung fehlgeschlagen (%u x).\n", "Encryption failed (%u x).\n"), secureFails);
    if (secureFails == 5) Serial.println(TR(">>> Dauerhaft? Im Menue Pairing loeschen und neu pairen.",
                                         ">>> Persistent? Delete pairing in the menu and pair again."));
    fail("");
    delay(1000);
    return false;
  }
  secureFails = 0;
  delay(200);   // Kamera nach dem Verschluesseln kurz in Ruhe lassen
  Serial.printf(TR("Verschluesselung: ja, gebondet: %s\n", "Encrypted: yes, bonded: %s\n"),
                conn->bonded() ? TR("ja", "yes") : TR("nein", "no"));

  // Bekannte Kamera: nur GPS-Meldungen abonnieren, keine Pairing-Abfrage senden
  lastWasMinimal = false;
  if (known && minimalOk != 0) {
    if (geoSvc.discover(h) && geoChr.discover() && geoInd.discover() && geoInd.enableIndicate()) {
      lastWasMinimal = true;
      Serial.println(TR("Schnellverbindung ohne Pairing-Abfrage, warte auf GPS-Anforderung ...",
                        "Quick connect without pairing request, waiting for GPS request ..."));
      return true;
    }
    Serial.println(TR("Schnellverbindung nicht moeglich -> volle Sequenz.", "Quick connect not possible -> full sequence."));
  }

  if (!priSvc.discover(h)) {
    if (remoteSvc.discover(h)) {
      remoteSeen = true;
      Serial.println(TR("!!! Kamera im FERNBEDIENUNGS-Modus, kein GPS. Als Smartphone pairen!",
                         "!!! Camera in REMOTE mode, no GPS. Pair as smartphone!"));
    }
    return fail(TR("Smart-Service 00010000 fehlt.", "Smart service 00010000 missing."));
  }
  if (!nameChr.discover() || !idenChr.discover()) return fail(TR("Pairing-Characteristics fehlen.", "Pairing characteristics missing."));
  if (!nameChr.enableIndicate()) return fail(TR("Kann Pairing-Indications nicht abonnieren.", "Cannot subscribe to pairing indications."));

  const uint8_t *name = (const uint8_t *)DEVICE_NAME;
  uint16_t nameLen = strlen(DEVICE_NAME);
  uint8_t two = 0x02;
  bool ok = writePrefixed(nameChr, "00010006", 0x01, name, nameLen) &&
            writePrefixed(idenChr, "0001000a", 0x03, persist.uuid, 16) &&
            writePrefixed(idenChr, "0001000a", 0x04, name, nameLen) &&
            writePrefixed(idenChr, "0001000a", 0x05, &two, 1);
  if (!ok) return fail(TR("Pairing-Sequenz fehlgeschlagen.", "Pairing sequence failed."));

  if (!known) {
    Serial.println(TR(">>> Bitte Pairing an der Kamera bestaetigen <<<", ">>> Please confirm pairing on the camera <<<"));
    waitingForUser = true;
    displayWake();
  }
  uint32_t timeout = known ? KNOWN_CAMERA_WAIT_MS : PAIR_TIMEOUT_MS;
  uint32_t t0 = millis();
  while (pairResult == -1 && millis() - t0 < timeout && !disconnected) {
    gpsUpdate(); buttonUpdate(); joystickUpdate(); ledUpdate(); displayUpdate();
    delay(20);
  }
  waitingForUser = false;
  if (pairResult == -1 && known && !disconnected) {
    Serial.println(TR("Keine Pairing-Antwort, Kamera ist aber bekannt -> weiter.", "No pairing reply, but camera is known -> continuing."));
    pairResult = PAIR_ACCEPT;
  }
  if (pairResult != PAIR_ACCEPT || disconnected) {
    Serial.printf("Pairing %s.\n", disconnected ? TR("abgebrochen (Kamera hat getrennt)", "aborted (camera disconnected)")
                                   : pairResult == PAIR_REJECT ? TR("abgelehnt", "rejected")
                                                               : TR("nicht bestaetigt", "not confirmed"));
    if (pairResult == PAIR_REJECT || !stored) conn->removeBondKey();
    if (stored && !known) needConfirm = false;
    return fail("");
  }
  Serial.printf(TR("Pairing akzeptiert (nach %lu ms).\n", "Pairing accepted (after %lu ms).\n"), (unsigned long)(millis() - t0));
  {
    char pn[24] = "";
    if (conn->getPeerName(pn, sizeof(pn)) && pn[0]) strncpy(seenName, pn, sizeof(seenName) - 1);
  }
  if (!known) needConfirm = false;
  if (!stored || strcmp(persist.camName, seenName) != 0) {
    memcpy(persist.addr, peer.addr, 6);
    persist.addrType = peer.addr_type;
    persist.haveCam = 1;
    strncpy(persist.camName, seenName[0] ? seenName : "Canon", sizeof(persist.camName) - 1);
    savePersist();
  }
  delay(300);

  // GPS-Service
  if (geoSvc.discover(h) && geoChr.discover() && geoInd.discover() && geoInd.enableIndicate()) {
    // ok
  } else {
    Serial.println(TR("GPS-Service/Characteristics nicht nutzbar.", "GPS service/characteristics not usable."));
  }

  uint8_t one = 0x01;
  if (!writeChr(idenChr, "0001000a", &one, 1) && known) {
    needConfirm = true;
    Serial.println(TR("Kamera verlangt offenbar eine neue Bestaetigung.", "Camera apparently requires a new confirmation."));
    return fail(TR(">>> Beim naechsten Versuch ggf. an der Kamera bestaetigen <<<", ">>> Confirm on the camera at the next attempt if asked <<<"));
  }
  if (SEND_MODE_SHOOT && modeSvc.discover(h) && modeChr.discover()) {
    uint8_t mode = MODE_SHOOT;
    writeChr(modeChr, "00030010", &mode, 1);
  }
  delay(200);
  if (disconnected) { Serial.println(TR("Kamera hat nach dem Pairing getrennt.", "Camera disconnected after pairing.")); return false; }
  Serial.println(TR("Warte auf GPS-Anforderung der Kamera ...", "Waiting for GPS request from camera ..."));
  return true;
}

// ============================ GPS senden ===============================

static bool sendGeo(const GpsFix &f) {
  uint8_t pkt[20];
  float v;
  pkt[0] = 0x04;
  pkt[1] = f.lat < 0 ? 'S' : 'N';
  v = fabs(f.lat); memcpy(pkt + 2, &v, 4);    // nRF52 ist little-endian
  pkt[6] = f.lon < 0 ? 'W' : 'E';
  v = fabs(f.lon); memcpy(pkt + 7, &v, 4);
  pkt[11] = f.alt < 0 ? '-' : '+';
  v = fabs(f.alt); memcpy(pkt + 12, &v, 4);
  memcpy(pkt + 16, &f.epoch, 4);
  bool ok = geoChr.write_resp(pkt, sizeof(pkt)) == sizeof(pkt);
  Serial.print(ok ? TR("-> Position gesendet: ", "-> Position sent: ") : TR("-> Senden FEHLGESCHLAGEN: ", "-> Sending FAILED: "));
  printHex(pkt, sizeof(pkt));
  Serial.println();
  // Verbindung "halbtot" (Schreiben scheitert, aber kein Disconnect)? -> neu verbinden
  static uint8_t failsInRow = 0, okInRow = 0;
  if (ok) {
    geoPacketsSent++;
    failsInRow = 0;
    if (++okInRow >= 10) { okInRow = 10; staleStreak = 0; }   // laeuft wieder stabil
  } else if (++failsInRow >= 3) {
    failsInRow = 0;
    okInRow = 0;
    backoff(TR("3x Senden fehlgeschlagen (Kamera aus?)", "Sending failed 3x (camera off?)"));
    if (Bluefruit.connected(connHandle)) Bluefruit.disconnect(connHandle);
  }
  return ok;
}

// ============================== Serial =================================

static void printStatus() {
  Serial.println("CanonGPS L1 v" FW_VERSION);
  const char *st[] = {"SCAN", "PAIRING", TR("VERBUNDEN", "CONNECTED"), TR("GPS AKTIV", "GPS ACTIVE")};
  char a[20] = "-";
  if (persist.haveCam) addrToStr(persist.addr, a);
  Serial.printf(TR("Status: %s, GPS freigegeben: %s, Pakete: %lu, Kamera: %s (%s)\n",
                   "Status: %s, GPS enabled: %s, packets: %lu, camera: %s (%s)\n"), st[(int)state],
                geoEnabled ? TR("ja", "yes") : TR("nein", "no"), (unsigned long)geoPacketsSent, a, persist.camName);
  Serial.printf(TR("Akku: %u mV (%d %%), GNSS: %s, %lu NMEA ok / %lu Fehler\n",
                   "Battery: %u mV (%d %%), GNSS: %s, %lu NMEA ok / %lu errors\n"), batMv, batPct,
                gnssFound ? "ok" : TR("nicht gefunden", "not found"), (unsigned long)gps.passedChecksum(),
                (unsigned long)gps.failedChecksum());
  printFix(gpsRead());
}

static void handleSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    switch (c) {
      case 's': printStatus(); break;
      case 'r': if (Bluefruit.connected(connHandle)) Bluefruit.disconnect(connHandle); break;
      case 'l': toggleLang(); break;
      case 'D': demoMode = !demoMode; Serial.printf("Demo mode: %s\n", demoMode ? "on" : "off"); displayWake(); break;
      case 'v': page = (page + 1) % PAGE_COUNT; displayWake(); break;      // naechste Display-Seite
      case 'k': menuSel = (menuSel + 1) % MENU_COUNT; displayWake(); break; // Menue-Auswahl weiter
      case 'P': displayDump(); break;                                       // Screenshot
      case 'Z': {                                    // Test: absichtlicher Absturz
        Serial.println("Test-Absturz ...");
        Serial.flush();
        delay(100);
        *(volatile uint32_t *)0xFFFFFFF0 = 1;
        break;
      }
      case 'h':
        listenForced = !listenForced;
        Serial.printf(TR("Nur-Zuhoeren-Modus: %s\n", "Listen-only mode: %s\n"), listenForced ? TR("an", "on") : TR("aus", "off"));
        if (listenForced && Bluefruit.connected(connHandle)) Bluefruit.disconnect(connHandle);
        break;
      case 'x':
        if (Bluefruit.connected(connHandle)) Bluefruit.disconnect(connHandle);
        forgetCamera();
        break;
      default: break;
    }
  }
}

// ============================ setup / loop =============================

// Diagnose: Startschritte ausgeben, auf USB warten, Absturz per LED anzeigen
#define BOOT_DEBUG 0     // 1 = Startschritte ausgeben und bis 10 s auf USB warten
#if BOOT_DEBUG
#define STEP(x) do { Serial.printf("[boot %lu ms] %s\n", (unsigned long)millis(), x); Serial.flush(); delay(20); } while (0)
#else
#define STEP(x) do {} while (0)
#endif

// ---------------------- Absturzprotokoll & Watchdog --------------------
// Bei einem HardFault Absturzstelle merken und neu starten (statt stehen zu bleiben).
// Der Bereich liegt ausserhalb von .data/.bss und uebersteht daher einen Software-Reset.
struct CrashInfo {
  uint32_t magic, type, pc, lr, cfsr, hfsr, count, reported, check;   // type: 1 HardFault, 2 Watchdog
};
static const uint32_t CRASH_MAGIC = 0xC0A5F417;
// Feste Adresse weit oben im RAM: der Bootloader (laeuft bei jedem Reset) nutzt den unteren
// RAM und loescht ihn; App-Stack liegt bei 0x2003F800..0x20040000, der Heap waechst von unten.
static volatile CrashInfo &crash = *reinterpret_cast<volatile CrashInfo *>(0x2003E000);
static uint32_t crashSum() {
  return crash.type ^ crash.pc ^ crash.lr ^ crash.cfsr ^ crash.hfsr ^ crash.count ^ crash.reported ^ CRASH_MAGIC;
}
static bool crashValid() { return crash.magic == CRASH_MAGIC && crash.check == crashSum(); }

static void crashRecord(uint32_t type, uint32_t *sp) {
  uint32_t cnt = crashValid() ? crash.count + 1 : 1;
  crash.type = type;
  crash.pc = sp[6];
  crash.lr = sp[5];
  crash.cfsr = SCB->CFSR;
  crash.hfsr = SCB->HFSR;
  crash.count = cnt;
  crash.reported = 0;
  crash.magic = CRASH_MAGIC;
  crash.check = crashSum();
}

extern "C" __attribute__((used)) void hardFaultC(uint32_t *sp) {
  crashRecord(1, sp);
  NVIC_SystemReset();
}
extern "C" __attribute__((used)) void wdtC(uint32_t *sp) {
  crashRecord(2, sp);          // Reset folgt automatisch durch den Watchdog
  while (true) {}
}

extern "C" __attribute__((naked)) void HardFault_Handler(void) {
  __asm volatile(
      "tst lr, #4      \n"
      "ite eq          \n"
      "mrseq r0, msp   \n"
      "mrsne r0, psp   \n"
      "b hardFaultC    \n");
}
// Watchdog-Interrupt kommt kurz vor dem Reset: Stelle merken, an der das Programm hing
extern "C" __attribute__((naked)) void WDT_IRQHandler(void) {
  __asm volatile(
      "tst lr, #4      \n"
      "ite eq          \n"
      "mrseq r0, msp   \n"
      "mrsne r0, psp   \n"
      "b wdtC          \n");
}

// Watchdog: haengt das Programm laenger als 2 min, startet der Chip neu
static const uint32_t WDT_TIMEOUT_S = 120;
static void wdtStart() {
  NRF_WDT->CONFIG = (WDT_CONFIG_HALT_Pause << WDT_CONFIG_HALT_Pos) | (WDT_CONFIG_SLEEP_Run << WDT_CONFIG_SLEEP_Pos);
  NRF_WDT->CRV = WDT_TIMEOUT_S * 32768;
  NRF_WDT->RREN = WDT_RREN_RR0_Msk;
  NRF_WDT->INTENSET = WDT_INTENSET_TIMEOUT_Msk;
  NVIC_SetPriority(WDT_IRQn, 2);
  NVIC_EnableIRQ(WDT_IRQn);
  NRF_WDT->TASKS_START = 1;
}
static inline void wdtFeed() { NRF_WDT->RR[0] = WDT_RR_RR_Reload; }

void setup() {
  uint32_t resetReason = NRF_POWER->RESETREAS;
  NRF_POWER->RESETREAS = 0xFFFFFFFF;
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < (BOOT_DEBUG ? 10000 : 1500)) delay(10);   // USB abwarten
  Serial.printf("Reset-Grund: 0x%08lx\n", (unsigned long)resetReason);
  // Absturzprotokoll (der Bootloader loescht RESETREAS, daher eigener Merker im RAM)
  if (crashValid()) {
    const char *what = crash.type == 2 ? TR("Watchdog (Programm hing)", "watchdog (program hung)")
                                       : "HardFault";
    Serial.printf(TR("!!! Letzter Neustart #%lu durch %s: PC=0x%08lx LR=0x%08lx CFSR=0x%08lx HFSR=0x%08lx\n",
                     "!!! Last reset #%lu by %s: PC=0x%08lx LR=0x%08lx CFSR=0x%08lx HFSR=0x%08lx\n"),
                  (unsigned long)crash.count, what, (unsigned long)crash.pc, (unsigned long)crash.lr,
                  (unsigned long)crash.cfsr, (unsigned long)crash.hfsr);
    snprintf(lastResetInfo, sizeof(lastResetInfo), "%s %08lX", crash.type == 2 ? "WDT" : TR("Absturz", "Crash"),
             (unsigned long)crash.pc);
    if (!crash.reported) { crash.reported = 1; crash.check = crashSum(); }
  }
  wdtStart();
  STEP("loadSettings");
  loadSettings();
  Serial.println("\n=== CanonGPS L1 v" FW_VERSION " (Wio Tracker L1 -> Canon EOS via BLE) ===");

  STEP("Taste/Joystick/LED");
  pinMode(PIN_BUTTON1, INPUT_PULLUP);
  joystickBegin();
  pinMode(LED_BUILTIN, OUTPUT);
  STEP("loraSleep");
  loraSleep();

  STEP("Bluefruit.begin");
  Bluefruit.begin(0, 1);               // 0 Peripheral, 1 Central
  Bluefruit.setName(DEVICE_NAME);
  Bluefruit.setTxPower(4);
  Bluefruit.autoConnLed(false);
  Bluefruit.Security.setIOCaps(false, false, false);   // "Just Works"
  Bluefruit.Security.setMITM(false);
  Bluefruit.Security.setPairCompleteCallback(onPairComplete);

  STEP("Services");
  priSvc.begin();
  nameChr.begin();
  idenChr.begin();
  nameChr.setIndicateCallback(onNameIndication);
  remoteSvc.begin();
  modeSvc.begin();
  modeChr.begin();
  geoSvc.begin();
  geoChr.begin();
  geoInd.begin();
  geoInd.setIndicateCallback(onGeoIndication);

  Bluefruit.Central.setConnectCallback(onConnect);
  Bluefruit.Central.setDisconnectCallback(onDisconnect);
  Bluefruit.Scanner.setRxCallback(onScan);
  Bluefruit.Scanner.restartOnDisconnect(false);
  Bluefruit.Scanner.setInterval(160, 160);   // durchgehend lauschen (100 ms / 100 ms)
  Bluefruit.Scanner.useActiveScan(false);

  STEP("loadPersist");
  loadPersist();
  STEP("gpsBegin");
  gpsBegin();
  STEP("displayBegin");
  displayBegin();
  STEP("batteryUpdate");
  batteryUpdate();
  noteInput();   // Sperre startet erst LOCK_AFTER_MS nach dem Einschalten
  STEP("fertig");

  char a[20];
  strcpy(a, TR("keine", "none"));
  if (persist.haveCam) addrToStr(persist.addr, a);
  Serial.print(TR("Geraete-UUID: ", "Device UUID: "));
  printHex(persist.uuid, 16);
  Serial.printf(TR("\nGespeicherte Kamera: %s %s\n", "\nStored camera: %s %s\n"), a, persist.camName);
  Serial.println(TR("Befehle: s=Status r=Reconnect x=Pairing loeschen l=Sprache",
                    "Commands: s=status r=reconnect x=delete pairing l=language"));
}

void loop() {
  wdtFeed();
  handleSerial();
  buttonUpdate();
  joystickUpdate();
  keyLockUpdate();
  camPowerUpdate();
  gpsUpdate();
  batteryUpdate();
  ledUpdate();
  displayUpdate();

  static State shown = State::SCAN;
  if (state != shown) { shown = state; displayWake(); }

  switch (state) {
    case State::SCAN: {
      static uint32_t searchStart = 0;
      static uint16_t attempt = 0;
      if (connPending) {
        connPending = false;
        if (attempt == 0) searchStart = millis();
        attempt++;
        Serial.printf(TR("Verbindungsversuch %u\n", "Connection attempt %u\n"), attempt);
        state = State::PAIRING;
        if (connectAndPair()) {
          state = State::CONNECTED;
          connectedAt = millis();
          Serial.printf(TR("Verbunden nach %u Versuch(en), %.1f s\n", "Connected after %u attempt(s), %.1f s\n"), attempt, (millis() - searchStart) / 1000.0);
          attempt = 0;
        } else {
          state = State::SCAN;
          if (attempt % 3 == 0)
            backoff(TR("3 Verbindungsversuche gescheitert", "3 connection attempts failed"));
          delay(RETRY_DELAY_MS);
        }
        break;
      }
      // laufenden Verbindungsaufbau nicht durch neuen Scan stoeren (max. 5 s)
      if (connectingSince && millis() - connectingSince < 5000) break;
      connectingSince = 0;
      // Pause nach "Kamera aus?" bzw. Diagnose: nur zuhoeren und Werbepakete zaehlen
      listenOnly = listenForced || (int32_t)(millis() - scanHoldUntil) < 0;
      {
        static uint32_t rateT0 = 0;
        if (millis() - rateT0 >= 10000) {
          if (listenForced) Serial.printf(TR("Kamera-Werbung: %lu Pakete/10 s%s\n", "Camera adverts: %lu packets/10 s%s\n"),
                        (unsigned long)advCount, listenOnly ? TR(" (nur zuhoeren)", " (listen only)") : "");
          advCount = 0;
          rateT0 = millis();
        }
      }
      if (listenOnly) {
        if (!Bluefruit.Scanner.isRunning()) Bluefruit.Scanner.start(0);
        break;
      }
      if (!Bluefruit.Scanner.isRunning() && !Bluefruit.Central.connected()) {
        if (attempt == 0) searchStart = millis();
        Bluefruit.Scanner.start(0);
      }
      break;
    }

    case State::PAIRING:
      break;

    case State::CONNECTED:
    case State::RUNNING:
      if (disconnected || !Bluefruit.connected(connHandle)) {
        Serial.println(TR("Verbindung verloren, suche erneut ...", "Connection lost, searching again ..."));
        if (millis() - connectedAt >= SHORT_CONN_MS) staleStreak = 0;
        state = State::SCAN;
        geoEnabled = false;
        break;
      }
      if (lastWasMinimal && !geoEnabled && millis() - connectedAt > 15000) {
        minimalOk = 0;
        lastWasMinimal = false;
        Serial.println(TR("Keine GPS-Freigabe ohne Pairing-Abfrage -> naechstes Mal volle Sequenz.",
                          "No GPS enable without pairing request -> full sequence next time."));
        connectedAt = 0;                        // nicht als "Kamera aus" werten
        if (Bluefruit.connected(connHandle)) Bluefruit.disconnect(connHandle);
        break;
      }
      if (lastWasMinimal && geoEnabled && minimalOk != 1) {
        minimalOk = 1;
        Serial.println(TR("Schnellverbindung funktioniert.", "Quick connect works."));
      }
      if (geoRequested) {
        geoRequested = false;
        uint8_t en = 0x01;
        writeChr(geoChr, TR("GPS-Freigabe", "GPS enable"), &en, 1);
      }
      if (geoEnabled) {
        if (state != State::RUNNING) {
          Serial.println(TR("Kamera akzeptiert GPS-Daten.", "Camera accepts GPS data."));
          state = State::RUNNING;
          lastGeoSend = 0;
        }
        if (lastGeoSend == 0 || millis() - lastGeoSend >= sendIntervalMs()) {
          lastGeoSend = millis() | 1;
          GpsFix f = gpsRead();
          printFix(f);
          if (f.valid) sendGeo(f);   // ohne Fix nichts senden
        }
      }
      break;
  }
  delay(10);
}
