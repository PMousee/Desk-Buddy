#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <math.h>
#include <time.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>

// Wiring layar: SCL=4, SDA=6, DC=1, RES=0
Adafruit_ST7789 tft = Adafruit_ST7789(&SPI, -1, 1, 0);

// ======================== PENGATURAN ========================
// --- WiFi (dipakai sebentar saja untuk sinkron jam dan ambil cuaca) ---
const char *WIFI_SSID = "WIFI";
const char *WIFI_PASS = "WIFI-PASS";

// --- Lokasi cuaca (default: Bandung). Ganti kalau lokasimu beda ---
const char *LATITUDE  = "-6.9175";
const char *LONGITUDE = "107.6191";
const char *TZ_STRING = "WIB-7";     // WIB = UTC+7 (WITA: "WITA-8", WIT: "WIT-9")

// --- Layar ---
const int  SCREEN_ROTATION = 3;      // 0-3
const bool INVERT_COLORS   = true;
const bool AUTO_CHANGE     = true;   // wajah ganti sendiri di halaman wajah
const bool SHOW_NAME       = true;   // tampilkan nama wajah di bawah
const bool AUTO_ROTATE     = true;   // halaman ganti otomatis (wajah -> jam -> cuaca)

// --- Pin ---
const int TOUCH_PIN     = 3;         // sensor sentuh (OUT)
const int BOOT_BTN_PIN  = 9;         // tombol BOOT bawaan board
const int BUZZER_PIN    = 10;
const int BACKLIGHT_PIN = -1;        // pin BLK layar (-1 = BLK masih ke 3V3). Kalau dipindah, pakai GPIO21
const int BATTERY_PIN   = 2;         // titik tengah pembagi tegangan baterai

const bool BUZZER_PASSIVE = true;    // false kalau buzzer-mu active

// --- Indikator baterai ---
// 0 = mati
// 1 = PERKIRAAN dari lama menyala (tanpa resistor, tanpa kabel tambahan)
// 2 = ukur asli (butuh 2 resistor 100k di GPIO2)
const int   BATTERY_MODE = 1;
const bool  BATTERY_ENABLED = (BATTERY_MODE != 0);
const float RUNTIME_HOURS = 5.0;     // (mode 1) kira-kira baterai penuh habis berapa jam. Sesuaikan hasil tes
const float SLEEP_DRAIN   = 0.6;     // (mode 1) saat mode malam, pemakaian dihitung segini dari normal
const float BAT_DIVIDER = 2.0;       // (mode 2) 2.0 kalau dua resistor sama besar
const float BAT_CAL     = 1.0;       // (mode 2) kalibrasi: tegangan multimeter / tegangan di Serial Monitor

// --- Waktu (milidetik) ---
const unsigned long DWELL_FACE    = 25000;   // lama di halaman wajah
const unsigned long DWELL_CLOCK   = 12000;   // lama di halaman jam
const unsigned long DWELL_WEATHER = 8000;    // lama di halaman cuaca
const unsigned long SLEEP_DIM_MS  = 120000;  // tidak disentuh 2 menit -> mode malam (jam redup)
const unsigned long SCREEN_OFF_MS = 300000;  // 5 menit -> layar mati (hanya kalau BACKLIGHT_PIN dipakai)
const unsigned long SYNC_INTERVAL_MS = 30UL * 60UL * 1000UL;  // sinkron jam + cuaca tiap 30 menit
const unsigned long SYNC_RETRY_MS    = 5UL * 60UL * 1000UL;   // kalau gagal, coba lagi 5 menit
const unsigned long HOLD_MS = 800;           // tahan sentuhan segini lama (lalu lepas) = ganti halaman
const unsigned long FULL_HOLD_MS = 4000;     // tahan segini lama (lalu lepas) = tandai baterai penuh (mode 1)
// ============================================================

// ---------- tipe data (harus di atas semua fungsi) ----------
struct Note { uint16_t f; uint16_t ms; };  // f = frekuensi (Hz), 0 = hening

enum Face {
  F_NORMAL, F_HAPPY, F_SLEEPY, F_ANGRY, F_SAD, F_SURPRISED, F_LOVE,
  F_WINK, F_DIZZY, F_BORED, F_CRY, F_EXCITED, F_COOL, FACE_COUNT
};

enum Page { P_FACE, P_CLOCK, P_WEATHER, PAGE_COUNT };
enum WKind { W_SUN, W_PARTLY, W_CLOUD, W_FOG, W_RAIN, W_STORM };

const int CANVAS_W = 240;
const int CANVAS_H = 180;
const int CANVAS_Y = 30;
GFXcanvas16 canvas(CANVAS_W, CANVAS_H);

const uint16_t BG    = ST77XX_BLACK;
const uint16_t CYAN  = ST77XX_CYAN;
const uint16_t RED   = ST77XX_RED;
const uint16_t WHITE = ST77XX_WHITE;
const uint16_t PINK  = 0xFC18;
const uint16_t TEAR  = 0x9EFF;
const uint16_t GRAY  = 0x7BEF;
const uint16_t LGRAY = 0xC618;
const uint16_t NIGHT = 0x3186;

const char *faceNames[FACE_COUNT] = {
  "NORMAL", "HAPPY", "SLEEPY", "ANGRY", "SAD", "SURPRISED", "LOVE",
  "WINK", "DIZZY", "BORED", "CRY", "EXCITED", "COOL"
};

const char *dayNames[7]    = {"MINGGU", "SENIN", "SELASA", "RABU", "KAMIS", "JUMAT", "SABTU"};
const char *monthNames[12] = {"Jan", "Feb", "Mar", "Apr", "Mei", "Jun", "Jul", "Agu", "Sep", "Okt", "Nov", "Des"};

// ---------- status ----------
Face face = F_NORMAL;
int shownFace = -1;
Page page = P_FACE;
unsigned long pageUntil = 0;
bool pageDirty = true;

float lookX = 0, lookY = 0, tgtX = 0, tgtY = 0;
float blinkAmt = 1.0;
int blinkDir = 0;
int bounceY = 0;

unsigned long nextBlink, nextLook, nextFace, lastFrame;
unsigned long lastActivity = 0, reactUntil = 0;
bool sleeping = false, screenOff = false;
bool soundOn = true;

char toastText[16] = "";
unsigned long toastUntil = 0;

// jam & cuaca
bool timeSynced = false;
unsigned long nextSync = 0;
bool weatherOk = false;
float weatherTemp = 0;
int weatherCode = 0;
char weatherAt[8] = "--:--";

// baterai
float batV = 0;
int batPct = -1;
bool batUsb = false;
unsigned long nextBat = 0, nextLowWarn = 0;
Preferences prefs;                 // penyimpanan permanen (untuk perkiraan baterai)
float usedMin = 0;                 // menit pemakaian sejak baterai penuh
unsigned long lastBatTick = 0, nextBatSave = 0;

// ==================== BACKLIGHT ====================
void setBacklight(int v) {
  if (BACKLIGHT_PIN < 0) return;
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(BACKLIGHT_PIN, v);
#else
  ledcWrite(1, v);
#endif
}

void backlightInit() {
  if (BACKLIGHT_PIN < 0) return;
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(BACKLIGHT_PIN, 5000, 8);
#else
  ledcSetup(1, 5000, 8);
  ledcAttachPin(BACKLIGHT_PIN, 1);
#endif
  setBacklight(255);
}

// ==================== BUZZER ====================
void buzzInit() {
  if (BUZZER_PASSIVE) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcAttach(BUZZER_PIN, 2000, 8);
#else
    ledcSetup(0, 2000, 8);
    ledcAttachPin(BUZZER_PIN, 0);
#endif
  } else {
    pinMode(BUZZER_PIN, OUTPUT);
    digitalWrite(BUZZER_PIN, LOW);
  }
}

void buzzOut(int f) {
  if (BUZZER_PASSIVE) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcWriteTone(BUZZER_PIN, f);
#else
    ledcWriteTone(0, f);
#endif
  } else {
    digitalWrite(BUZZER_PIN, f > 0 ? HIGH : LOW);
  }
}

const Note SND_START[]    = {{1047, 80}, {1319, 80}, {1568, 140}};
const Note SND_HAPPY[]    = {{880, 70}, {1175, 70}, {1568, 110}};
const Note SND_ANGRY[]    = {{220, 120}, {0, 40}, {220, 120}, {0, 40}, {196, 200}};
const Note SND_SAD[]      = {{660, 150}, {523, 150}, {392, 260}};
const Note SND_SURPRISE[] = {{1500, 60}, {0, 30}, {1900, 100}};
const Note SND_SLEEPY[]   = {{400, 200}, {300, 320}};
const Note SND_WINK[]     = {{1200, 50}, {1600, 90}};
const Note SND_DIZZY[]    = {{500, 60}, {700, 60}, {500, 60}, {700, 60}, {500, 60}};
const Note SND_BORED[]    = {{300, 220}};
const Note SND_COOL[]     = {{440, 80}, {554, 80}, {659, 170}};
const Note SND_NORMAL[]   = {{1000, 40}};

const Note *curMelody = nullptr;
int melodyLen = 0, melodyIdx = 0;
unsigned long noteEnd = 0;

void playMelody(const Note *m, int n) {
  if (!soundOn) return;
  curMelody = m;
  melodyLen = n;
  melodyIdx = 0;
  buzzOut(m[0].f);
  noteEnd = millis() + m[0].ms;
}
#define PLAY(arr) playMelody(arr, sizeof(arr) / sizeof(arr[0]))

void updateSound() {
  if (!curMelody || millis() < noteEnd) return;
  melodyIdx++;
  if (melodyIdx >= melodyLen) {
    buzzOut(0);
    curMelody = nullptr;
    return;
  }
  buzzOut(curMelody[melodyIdx].f);
  noteEnd = millis() + curMelody[melodyIdx].ms;
}

void soundForFace(Face f) {
  switch (f) {
    case F_HAPPY: case F_LOVE: case F_EXCITED: PLAY(SND_HAPPY); break;
    case F_ANGRY:     PLAY(SND_ANGRY);    break;
    case F_SAD: case F_CRY: PLAY(SND_SAD); break;
    case F_SURPRISED: PLAY(SND_SURPRISE); break;
    case F_SLEEPY:    PLAY(SND_SLEEPY);   break;
    case F_WINK:      PLAY(SND_WINK);     break;
    case F_DIZZY:     PLAY(SND_DIZZY);    break;
    case F_BORED:     PLAY(SND_BORED);    break;
    case F_COOL:      PLAY(SND_COOL);     break;
    default:          PLAY(SND_NORMAL);   break;
  }
}

// ==================== FUNGSI BANTU GAMBAR WAJAH ====================
int eyeX(int i) { return (i == 0 ? 72 : 168) + (int)lookX; }
int eyeY()      { return 62 + (int)lookY + bounceY; }
int mouthX()    { return 120 + (int)(lookX / 2); }
int mouthY()    { return 130 + (int)(lookY / 2); }
int bh(int base) { return max(4, (int)(base * blinkAmt)); }

void eyeRound(int i, int w, int h, int r, uint16_t c) {
  canvas.fillRoundRect(eyeX(i) - w / 2, eyeY() - h / 2, w, h, r, c);
}

void mouthCurve(int w, int depth, int thick, uint16_t c) {
  int mx = mouthX(), my = mouthY();
  for (int dx = -w; dx <= w; dx += 2) {
    int y = my + depth - (int)((long)depth * dx * dx / ((long)w * w));
    canvas.fillCircle(mx + dx, y, thick / 2, c);
  }
}

void mouthOpenSmile(int w, uint16_t c) {
  int mx = mouthX(), my = mouthY() - 4;
  canvas.fillCircle(mx, my, w / 2, c);
  canvas.fillRect(mx - w / 2 - 1, my - w / 2 - 1, w + 2, w / 2 + 1, BG);
}

void mouthFlat(int w, uint16_t c) {
  canvas.fillRoundRect(mouthX() - w / 2, mouthY() + 4, w, 6, 3, c);
}

void mouthO(int r, uint16_t c) {
  canvas.fillCircle(mouthX(), mouthY() + 6, r, c);
}

void lidSlant(int i, int h, bool inner) {
  int cx = eyeX(i), x0 = cx - 31, x1 = cx + 31;
  int y0 = eyeY() - h / 2 - 1, drop = h * 45 / 100;
  bool left = (i == 0);
  bool lowerRight = (left == inner);
  if (lowerRight) canvas.fillTriangle(x0, y0, x1, y0, x1, y0 + drop, BG);
  else            canvas.fillTriangle(x0, y0, x1, y0, x0, y0 + drop, BG);
}

void eyeArch(int i, uint16_t c) {
  eyeRound(i, 60, 72, 20, c);
  canvas.fillCircle(eyeX(i), eyeY() + 46, 36, BG);
}

void heart(int cx, int cy, int r, uint16_t c) {
  canvas.fillCircle(cx - r, cy - r / 2, r, c);
  canvas.fillCircle(cx + r, cy - r / 2, r, c);
  canvas.fillTriangle(cx - 2 * r + 1, cy - r / 2 + r / 3,
                      cx + 2 * r - 1, cy - r / 2 + r / 3,
                      cx, cy + 2 * r, c);
}

void spiral(int cx, int cy, uint16_t c) {
  float phase = millis() / 120.0;
  for (float t = 0; t < 18.0; t += 0.3) {
    float r = 2 + t * 1.35;
    float a = t + phase;
    canvas.fillCircle(cx + (int)(r * cos(a)), cy + (int)(r * sin(a)), 2, c);
  }
}

// ==================== SATU FUNGSI PER WAJAH ====================
void faceNormal() {
  for (int i = 0; i < 2; i++) eyeRound(i, 60, bh(72), 20, CYAN);
  mouthCurve(26, 10, 6, CYAN);
}

void faceHappy() {
  for (int i = 0; i < 2; i++) {
    eyeArch(i, CYAN);
    canvas.fillRoundRect(eyeX(i) - 18, eyeY() + 48, 36, 10, 5, PINK);
  }
  mouthOpenSmile(48, CYAN);
}

void faceSleepy() {
  int h = bh(56);
  for (int i = 0; i < 2; i++) {
    eyeRound(i, 60, h, 20, CYAN);
    canvas.fillRect(eyeX(i) - 31, eyeY() - h / 2 - 1, 62, h / 2 + 1, BG);
  }
  mouthO(5 + (int)(2 * sin(millis() / 400.0)), CYAN);
  canvas.setTextColor(CYAN);
  canvas.setTextSize(2);
  canvas.setCursor(168, 12);
  canvas.print("z");
  canvas.setTextSize(3);
  canvas.setCursor(194, 0);
  canvas.print("Z");
}

void faceAngry() {
  int h = bh(72);
  for (int i = 0; i < 2; i++) {
    eyeRound(i, 60, h, 20, RED);
    lidSlant(i, h, true);
  }
  mouthCurve(24, -10, 6, RED);
}

void faceSad() {
  int h = bh(72);
  for (int i = 0; i < 2; i++) {
    eyeRound(i, 60, h, 20, CYAN);
    lidSlant(i, h, false);
  }
  mouthCurve(22, -12, 6, CYAN);
}

void faceSurprised() {
  int h = bh(92);
  for (int i = 0; i < 2; i++) {
    eyeRound(i, 52, h, 26, CYAN);
    if (blinkAmt > 0.5) canvas.fillCircle(eyeX(i), eyeY(), 8, BG);
  }
  canvas.fillCircle(mouthX(), mouthY() + 8, 14, CYAN);
  canvas.fillCircle(mouthX(), mouthY() + 8, 8, BG);
}

void faceLove() {
  int r = 14 + (int)(2 * sin(millis() / 150.0));
  for (int i = 0; i < 2; i++) heart(eyeX(i), eyeY(), r, PINK);
  mouthOpenSmile(44, PINK);
}

void faceWink() {
  eyeRound(0, 60, 72, 20, CYAN);
  eyeArch(1, CYAN);
  canvas.fillRoundRect(mouthX() + 2, mouthY() + 8, 16, 20, 8, PINK);
  mouthCurve(28, 12, 6, CYAN);
}

void faceDizzy() {
  for (int i = 0; i < 2; i++) spiral(eyeX(i), eyeY(), CYAN);
  int mx = mouthX(), my = mouthY() + 6;
  for (int k = -3; k < 3; k++) {
    int x0 = mx + k * 10, x1 = x0 + 10;
    int y0 = my + ((k & 1) ? 5 : -5), y1 = my + ((k & 1) ? -5 : 5);
    canvas.drawLine(x0, y0, x1, y1, CYAN);
    canvas.drawLine(x0, y0 + 1, x1, y1 + 1, CYAN);
    canvas.drawLine(x0, y0 + 2, x1, y1 + 2, CYAN);
  }
}

void faceBored() {
  int h = bh(44);
  for (int i = 0; i < 2; i++) {
    eyeRound(i, 60, h, 14, CYAN);
    canvas.fillRect(eyeX(i) - 31, eyeY() - h / 2 - 1, 62, h / 2 + 1, BG);
  }
  mouthFlat(36, CYAN);
}

void faceCry() {
  int h = bh(72);
  for (int i = 0; i < 2; i++) {
    eyeRound(i, 60, h, 20, CYAN);
    lidSlant(i, h, false);
    int ty = eyeY() + 30 + ((millis() / 6 + i * 40) % 60);
    canvas.fillCircle(eyeX(i) + (i == 0 ? -12 : 12), ty, 5, TEAR);
  }
  mouthCurve(20, -14 + (int)(2 * sin(millis() / 120.0)), 6, CYAN);
}

void faceExcited() {
  bounceY = (int)(4 * sin(millis() / 110.0));
  for (int i = 0; i < 2; i++) {
    int cx = eyeX(i), cy = eyeY();
    if (blinkAmt > 0.5) {
      canvas.fillCircle(cx, cy, 30, CYAN);
      canvas.fillCircle(cx - 10, cy - 10, 9, WHITE);
      canvas.fillCircle(cx + 10, cy + 10, 4, WHITE);
    } else {
      canvas.fillRoundRect(cx - 30, cy - 3, 60, 7, 3, CYAN);
    }
  }
  mouthOpenSmile(52, CYAN);
}

void faceCool() {
  int cx1 = eyeX(0), cx2 = eyeX(1), cy = eyeY();
  for (int i = 0; i < 2; i++) {
    int cx = eyeX(i);
    canvas.fillRoundRect(cx - 34, cy - 24, 68, 48, 14, WHITE);
    canvas.fillRoundRect(cx - 30, cy - 20, 60, 40, 11, BG);
    for (int k = 0; k < 3; k++)
      canvas.drawLine(cx - 20 + k, cy - 12, cx - 8 + k, cy + 2, WHITE);
  }
  canvas.fillRect(cx1 + 34, cy - 14, (cx2 - 34) - (cx1 + 34), 8, WHITE);
  canvas.fillRect(cx1 - 46, cy - 14, 12, 6, WHITE);
  canvas.fillRect(cx2 + 34, cy - 14, 12, 6, WHITE);
  mouthCurve(24, 8, 6, CYAN);
}

void drawFace() {
  canvas.fillScreen(BG);
  bounceY = 0;
  switch (face) {
    case F_NORMAL:    faceNormal();    break;
    case F_HAPPY:     faceHappy();     break;
    case F_SLEEPY:    faceSleepy();    break;
    case F_ANGRY:     faceAngry();     break;
    case F_SAD:       faceSad();       break;
    case F_SURPRISED: faceSurprised(); break;
    case F_LOVE:      faceLove();      break;
    case F_WINK:      faceWink();      break;
    case F_DIZZY:     faceDizzy();     break;
    case F_BORED:     faceBored();     break;
    case F_CRY:       faceCry();       break;
    case F_EXCITED:   faceExcited();   break;
    case F_COOL:      faceCool();      break;
    default: break;
  }
  tft.drawRGBBitmap(0, CANVAS_Y, canvas.getBuffer(), CANVAS_W, CANVAS_H);
}

// ==================== LABEL & TOAST (teks di bawah layar) ====================
void drawLabel() {
  tft.fillRect(0, 214, 240, 26, BG);
  const char *n = nullptr;
  if (toastUntil) n = toastText;
  else if (SHOW_NAME && page == P_FACE) n = faceNames[face];
  if (!n) return;
  tft.setTextSize(2);
  tft.setTextColor(toastUntil ? WHITE : GRAY, BG);
  tft.setCursor(120 - strlen(n) * 6, 218);
  tft.print(n);
}

void toast(const char *t) {
  strncpy(toastText, t, sizeof(toastText) - 1);
  toastText[sizeof(toastText) - 1] = 0;
  toastUntil = millis() + 1500;
  drawLabel();
}

// ==================== JAM ====================
bool getTm(struct tm &t) {
  if (!timeSynced) return false;
  time_t n = time(nullptr);
  localtime_r(&n, &t);
  return true;
}

void centerText(const char *s, int y, int size, uint16_t c) {
  int w = strlen(s) * 6 * size;
  canvas.setTextSize(size);
  canvas.setTextColor(c);
  canvas.setCursor((CANVAS_W - w) / 2, y);
  canvas.print(s);
}

// ==================== CUACA ====================
WKind weatherKind(int code) {
  if (code == 0) return W_SUN;
  if (code == 1 || code == 2) return W_PARTLY;
  if (code == 3) return W_CLOUD;
  if (code == 45 || code == 48) return W_FOG;
  if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) return W_RAIN;
  if (code >= 95) return W_STORM;
  return W_CLOUD;
}

const char *weatherText(WKind k) {
  switch (k) {
    case W_SUN:    return "Cerah";
    case W_PARTLY: return "Berawan";
    case W_CLOUD:  return "Mendung";
    case W_FOG:    return "Kabut";
    case W_RAIN:   return "Hujan";
    default:       return "Badai";
  }
}

void drawSun(int cx, int cy, int r, uint16_t c) {
  canvas.fillCircle(cx, cy, r, c);
  for (int i = 0; i < 8; i++) {
    float a = i * PI / 4;
    int x0 = cx + (int)((r + 5) * cos(a)), y0 = cy + (int)((r + 5) * sin(a));
    int x1 = cx + (int)((r + 14) * cos(a)), y1 = cy + (int)((r + 14) * sin(a));
    canvas.drawLine(x0, y0, x1, y1, c);
    canvas.drawLine(x0 + 1, y0, x1 + 1, y1, c);
  }
}

void drawCloud(int cx, int cy, uint16_t c) {
  canvas.fillCircle(cx - 16, cy + 4, 13, c);
  canvas.fillCircle(cx + 2, cy - 6, 17, c);
  canvas.fillCircle(cx + 20, cy + 5, 12, c);
  canvas.fillRoundRect(cx - 29, cy + 4, 60, 14, 7, c);
}

void drawWeatherIcon(WKind k, int cx, int cy) {
  switch (k) {
    case W_SUN:
      drawSun(cx, cy, 18, ST77XX_YELLOW);
      break;
    case W_PARTLY:
      drawSun(cx - 14, cy - 12, 12, ST77XX_YELLOW);
      drawCloud(cx + 4, cy + 8, LGRAY);
      break;
    case W_CLOUD:
      drawCloud(cx, cy, LGRAY);
      break;
    case W_FOG:
      drawCloud(cx, cy - 12, GRAY);
      for (int i = 0; i < 3; i++)
        canvas.fillRoundRect(cx - 28 + (i % 2) * 8, cy + 14 + i * 10, 52, 5, 2, GRAY);
      break;
    case W_RAIN:
      drawCloud(cx, cy - 10, GRAY);
      for (int i = 0; i < 3; i++) {
        canvas.drawLine(cx - 18 + i * 16, cy + 16, cx - 22 + i * 16, cy + 30, TEAR);
        canvas.drawLine(cx - 17 + i * 16, cy + 16, cx - 21 + i * 16, cy + 30, TEAR);
      }
      break;
    default:  // W_STORM
      drawCloud(cx, cy - 12, GRAY);
      canvas.fillTriangle(cx + 6, cy + 14, cx - 8, cy + 32, cx + 2, cy + 32, ST77XX_YELLOW);
      canvas.fillTriangle(cx + 2, cy + 28, cx + 10, cy + 28, cx - 6, cy + 48, ST77XX_YELLOW);
      break;
  }
}

// ==================== HALAMAN JAM ====================
int lastClockKey = -1;

void drawClockPage() {
  struct tm t;
  bool ok = getTm(t);
  int key = ok ? (t.tm_sec + 60 * t.tm_min + 3600 * t.tm_hour) : -2;
  if (!pageDirty && key == lastClockKey) return;  // gambar ulang hanya kalau ada yang berubah
  lastClockKey = key;
  pageDirty = false;

  uint16_t cMain = sleeping ? NIGHT : CYAN;
  uint16_t cSub  = sleeping ? NIGHT : WHITE;
  uint16_t cDay  = sleeping ? NIGHT : PINK;

  canvas.fillScreen(BG);
  char buf[32];
  if (ok) {
    snprintf(buf, sizeof(buf), "%02d%c%02d", t.tm_hour, (t.tm_sec & 1) ? ' ' : ':', t.tm_min);
    centerText(buf, 10, 7, cMain);
    centerText(dayNames[t.tm_wday], 78, 3, cDay);
    snprintf(buf, sizeof(buf), "%d %s %d", t.tm_mday, monthNames[t.tm_mon], t.tm_year + 1900);
    centerText(buf, 112, 3, cSub);
    if (weatherOk) {
      snprintf(buf, sizeof(buf), "%dC %s", (int)roundf(weatherTemp), weatherText(weatherKind(weatherCode)));
      centerText(buf, 150, 2, sleeping ? NIGHT : GRAY);
    }
  } else {
    centerText("--:--", 10, 7, cMain);
    centerText("MENUNGGU WIFI", 100, 2, cSub);
  }
  tft.drawRGBBitmap(0, CANVAS_Y, canvas.getBuffer(), CANVAS_W, CANVAS_H);
}

// ==================== HALAMAN CUACA ====================
void drawWeatherPage() {
  if (!pageDirty) return;
  pageDirty = false;

  canvas.fillScreen(BG);
  if (!weatherOk) {
    centerText("CUACA", 50, 4, GRAY);
    centerText("BELUM ADA DATA", 110, 2, WHITE);
    tft.drawRGBBitmap(0, CANVAS_Y, canvas.getBuffer(), CANVAS_W, CANVAS_H);
    return;
  }

  WKind k = weatherKind(weatherCode);
  drawWeatherIcon(k, 55, 80);

  char num[8];
  snprintf(num, sizeof(num), "%d", (int)roundf(weatherTemp));
  int len = strlen(num);
  int gw = len * 36 + 38;
  int sx = 95 + (145 - gw) / 2;
  canvas.setTextSize(6);
  canvas.setTextColor(CYAN);
  canvas.setCursor(sx, 40);
  canvas.print(num);
  int dx = sx + len * 36 + 8;
  canvas.drawCircle(dx, 46, 4, CYAN);
  canvas.drawCircle(dx, 46, 3, CYAN);
  canvas.setTextSize(4);
  canvas.setCursor(dx + 8, 40);
  canvas.print("C");

  const char *txt = weatherText(k);
  canvas.setTextSize(2);
  canvas.setTextColor(WHITE);
  canvas.setCursor(95 + (145 - (int)strlen(txt) * 12) / 2, 104);
  canvas.print(txt);

  char buf[24];
  snprintf(buf, sizeof(buf), "update %s", weatherAt);
  centerText(buf, 160, 1, GRAY);
  tft.drawRGBBitmap(0, CANVAS_Y, canvas.getBuffer(), CANVAS_W, CANVAS_H);
}

// ==================== BATERAI ====================
int pctFromV(float v) {
  const float tv[] = {3.30, 3.50, 3.60, 3.70, 3.80, 3.90, 4.00, 4.10, 4.20};
  const int   tp[] = {0, 5, 10, 25, 45, 65, 80, 90, 100};
  if (v <= tv[0]) return 0;
  if (v >= tv[8]) return 100;
  for (int i = 0; i < 8; i++) {
    if (v < tv[i + 1]) return tp[i] + (int)((v - tv[i]) * (tp[i + 1] - tp[i]) / (tv[i + 1] - tv[i]));
  }
  return 100;
}

void checkLowBattery(unsigned long now) {
  if (!batUsb && batPct >= 0 && batPct <= 15 && now > nextLowWarn) {
    nextLowWarn = now + 90000;
    toast("BATERAI LEMAH");
    PLAY(SND_SAD);
  }
}

// Mode 1: tandai baterai penuh (dipanggil lewat tahan sentuhan 4 detik)
void markBatteryFull() {
  usedMin = 0;
  batPct = 100;
  prefs.putFloat("used", usedMin);
  toast("BATERAI PENUH");
  PLAY(SND_HAPPY);
}

// Mode 1: hitung perkiraan sisa baterai dari lama menyala
void updateBatteryEstimate(unsigned long now) {
  if (now < nextBat) return;
  nextBat = now + 10000;

  float dt = (now - lastBatTick) / 60000.0;  // menit sejak hitungan terakhir
  lastBatTick = now;
  float w = screenOff ? 0.3 : (sleeping ? SLEEP_DRAIN : 1.0);
  usedMin += dt * w;

  float pct = 100.0 - usedMin / (RUNTIME_HOURS * 60.0) * 100.0;
  batPct = constrain((int)pct, 0, 100);
  batUsb = false;

  if (now > nextBatSave) {  // simpan tiap 2 menit supaya tidak hilang saat dimatikan
    nextBatSave = now + 120000;
    prefs.putFloat("used", usedMin);
  }
  checkLowBattery(now);
}

// Mode 2: ukur tegangan asli lewat pembagi tegangan (butuh 2 resistor)
void updateBatteryAdc(unsigned long now) {
  if (now < nextBat) return;
  nextBat = now + 5000;

  long sum = 0;
  for (int i = 0; i < 16; i++) sum += analogReadMilliVolts(BATTERY_PIN);
  float v = (sum / 16.0) / 1000.0 * BAT_DIVIDER * BAT_CAL;

  batV = (batPct < 0) ? v : (batV * 0.7 + v * 0.3);  // haluskan angka
  batUsb = (batV > 4.35);                             // lebih dari 4.35 V = tersambung USB
  batPct = batUsb ? 100 : pctFromV(batV);

  Serial.print("baterai: ");
  Serial.print(batV, 2);
  Serial.println(" V");

  checkLowBattery(now);
}

void updateBattery(unsigned long now) {
  if (BATTERY_MODE == 1) updateBatteryEstimate(now);
  else if (BATTERY_MODE == 2) updateBatteryAdc(now);
}

// ==================== BAR STATUS (atas layar) ====================
int lastStatusKey = -1;

void drawStatusBar() {
  struct tm t;
  bool ok = getTm(t);
  int key = ((ok ? t.tm_hour * 60 + t.tm_min : -1) + 1) * 1000
            + (batPct + 1) * 2 + (batUsb ? 1 : 0)
            + (int)page * 10000000 + (sleeping ? 100000000 : 0);
  if (key == lastStatusKey) return;
  lastStatusKey = key;

  tft.fillRect(0, 0, 240, 28, BG);
  uint16_t c = sleeping ? NIGHT : WHITE;

  if (page != P_CLOCK && ok) {  // halaman jam sudah punya jam besar
    tft.setTextSize(2);
    tft.setTextColor(c, BG);
    tft.setCursor(8, 7);
    tft.printf("%02d:%02d", t.tm_hour, t.tm_min);
  }

  if (BATTERY_ENABLED && batPct >= 0) {
    int x0 = 140, y0 = 7;
    uint16_t lc = (batPct > 40) ? ST77XX_GREEN : ((batPct > 15) ? ST77XX_YELLOW : RED);
    tft.drawRect(x0, y0, 26, 14, c);
    tft.fillRect(x0 + 26, y0 + 4, 3, 6, c);
    int fw = 22 * batPct / 100;
    if (fw > 0) tft.fillRect(x0 + 2, y0 + 2, fw, 10, lc);
    tft.setTextSize(2);
    tft.setTextColor(c, BG);
    tft.setCursor(x0 + 34, y0);
    if (batUsb) tft.print("USB");
    else tft.printf("%s%d%%", (BATTERY_MODE == 1) ? "~" : "", batPct);
  }
}

// ==================== WIFI: SINKRON JAM + CUACA ====================
void fetchWeather() {
  WiFiClientSecure client;
  client.setInsecure();  // tanpa cek sertifikat (cukup untuk data cuaca publik)
  HTTPClient http;
  String url = String("https://api.open-meteo.com/v1/forecast?latitude=") + LATITUDE +
               "&longitude=" + LONGITUDE + "&current=temperature_2m,weather_code&timezone=auto";
  if (!http.begin(client, url)) return;
  http.setTimeout(8000);
  int code = http.GET();
  if (code == 200) {
    String body = http.getString();
    int c = body.indexOf("\"current\":{");
    if (c >= 0) {
      String cur = body.substring(c);
      int ti = cur.indexOf("\"temperature_2m\":");
      int wi = cur.indexOf("\"weather_code\":");
      if (ti >= 0 && wi >= 0) {
        weatherTemp = cur.substring(ti + 17).toFloat();
        weatherCode = cur.substring(wi + 15).toInt();
        weatherOk = true;
        struct tm t;
        if (getTm(t)) snprintf(weatherAt, sizeof(weatherAt), "%02d:%02d", t.tm_hour, t.tm_min);
      }
    }
  }
  http.end();
}

void syncNow() {
  if (strcmp(WIFI_SSID, "ISI_NAMA_WIFI") == 0) {  // WiFi belum diisi
    toast("ISI WIFI DULU");
    nextSync = millis() + SYNC_INTERVAL_MS;
    return;
  }

  toast("SYNC...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 12000) {
    delay(100);
    updateSound();
  }

  bool ok = false;
  if (WiFi.status() == WL_CONNECTED) {
    configTzTime(TZ_STRING, "pool.ntp.org", "time.google.com");
    struct tm tmv;
    if (getLocalTime(&tmv, 8000)) timeSynced = true;
    fetchWeather();
    ok = timeSynced;
  }

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);  // matikan WiFi supaya hemat baterai

  toast(ok ? "SYNC OK" : "SYNC GAGAL");
  nextSync = millis() + (ok ? SYNC_INTERVAL_MS : SYNC_RETRY_MS);
  pageDirty = true;
}

// ==================== LOGIKA HALAMAN & WAJAH ====================
unsigned long dwell(Page p) {
  if (p == P_FACE) return DWELL_FACE;
  if (p == P_CLOCK) return DWELL_CLOCK;
  return DWELL_WEATHER;
}

void setPage(Page p) {
  page = p;
  pageUntil = millis() + dwell(p);
  pageDirty = true;
  drawLabel();
}

void nextPage(bool manual) {
  Page n = (Page)((page + 1) % PAGE_COUNT);
  if (n == P_WEATHER && !weatherOk && !manual) n = (Page)((n + 1) % PAGE_COUNT);
  setPage(n);
  if (manual) PLAY(SND_WINK);
}

void setFace(Face f, bool withSound) {
  face = f;
  nextFace = millis() + random(6000, 12000);
  if (withSound) soundForFace(f);
}

void wake() {
  if (!sleeping && !screenOff) return;
  sleeping = false;
  screenOff = false;
  setBacklight(255);
  pageDirty = true;
  nextFace = millis() + 8000;
}

void goSleep() {
  sleeping = true;
  setBacklight(25);
  setPage(P_CLOCK);  // mode malam: jam redup
}

void toggleMute() {
  soundOn = !soundOn;
  if (soundOn) {
    toast("SOUND ON");
    PLAY(SND_HAPPY);
  } else {
    toast("MUTE");
  }
}

// ==================== INPUT ====================
// --- sensor sentuh: ketuk = elus wajah, tahan = ganti halaman ---
bool lastTouch = false, wokeByTouch = false;
int holdStage = 0;  // 0 = ketukan, 1 = tahan (ganti halaman), 2 = tahan lama (baterai penuh)
unsigned long touchStart = 0;
int tapCount = 0;
unsigned long tapWindow = 0;

void handleTouch(unsigned long now) {
  bool t = (digitalRead(TOUCH_PIN) == HIGH);

  if (t && !lastTouch) {  // baru mulai disentuh
    Serial.println("touch");
    touchStart = now;
    holdStage = 0;
    lastActivity = now;
    wokeByTouch = (sleeping || screenOff);
    wake();
  }

  if (t) {
    lastActivity = now;
    unsigned long held = now - touchStart;
    if (holdStage == 0 && held > HOLD_MS) {  // bunyi pendek = "lepas sekarang untuk ganti halaman"
      holdStage = 1;
      if (!wokeByTouch) PLAY(SND_NORMAL);
    }
    if (holdStage == 1 && held > FULL_HOLD_MS) {  // masih ditahan -> mode tandai baterai penuh
      holdStage = 2;
      if (!wokeByTouch && BATTERY_MODE == 1) {
        toast("LEPAS=PENUH");
        PLAY(SND_HAPPY);
      }
    }
  }

  if (!t && lastTouch) {  // sentuhan dilepas
    if (wokeByTouch) {    // dibangunkan -> kaget
      setPage(P_FACE);
      setFace(F_SURPRISED, true);
      reactUntil = now + 3000;
    } else if (holdStage == 0) {  // ketukan singkat
      if (page == P_FACE) {
        if (now - tapWindow > 4000) { tapCount = 0; tapWindow = now; }
        tapCount++;
        if (tapCount >= 5) {             // diketuk terus -> kesal
          setFace(F_ANGRY, true);
          reactUntil = now + 4000;
          tapCount = 0;
        } else {                         // dielus -> senang
          Face r[] = {F_HAPPY, F_LOVE, F_WINK, F_EXCITED};
          setFace(r[random(4)], true);
          reactUntil = now + 4000;
        }
      } else {
        PLAY(SND_NORMAL);
      }
    } else if (holdStage == 2 && BATTERY_MODE == 1) {
      markBatteryFull();
    } else {
      nextPage(true);
    }
  }
  lastTouch = t;
}

// --- tombol BOOT: tekan singkat = halaman berikutnya, tahan = mute ---
bool lastBtn = false, btnLong = false;
unsigned long btnStart = 0;

void handleButton(unsigned long now) {
  bool b = (digitalRead(BOOT_BTN_PIN) == LOW);

  if (b && !lastBtn) { btnStart = now; btnLong = false; }

  if (b && !btnLong && now - btnStart > 1200) {
    btnLong = true;
    lastActivity = now;
    wake();
    toggleMute();
  }

  if (!b && lastBtn && !btnLong && now - btnStart > 40) {
    Serial.println("button");
    lastActivity = now;
    bool was = (sleeping || screenOff);
    wake();
    if (was) {
      setPage(P_FACE);
      setFace(F_SURPRISED, true);
      reactUntil = now + 3000;
    } else {
      nextPage(true);
    }
  }
  lastBtn = b;
}

// ==================== SETUP & LOOP ====================
void setup() {
  Serial.begin(115200);
  pinMode(TOUCH_PIN, INPUT);
  pinMode(BOOT_BTN_PIN, INPUT_PULLUP);
  buzzInit();
  if (BATTERY_MODE == 1) {
    prefs.begin("buddy", false);
    usedMin = prefs.getFloat("used", 0);  // lanjutkan hitungan terakhir
    lastBatTick = millis();
  }

  SPI.begin(4, 5, 6, -1);
  tft.init(240, 240, SPI_MODE3);
  tft.setSPISpeed(40000000);
  tft.setRotation(SCREEN_ROTATION);
  tft.invertDisplay(INVERT_COLORS);
  tft.fillScreen(BG);
  backlightInit();
  randomSeed(micros());

  unsigned long now = millis();
  nextBlink = now + 2000;
  nextLook  = now + 1500;
  nextFace  = now + 8000;
  lastFrame = now;
  lastActivity = now;
  nextSync = now + 1500;  // sinkron jam + cuaca sesaat setelah nyala
  setPage(P_FACE);

  PLAY(SND_START);
}

void loop() {
  unsigned long now = millis();

  handleTouch(now);
  handleButton(now);
  updateSound();
  updateBattery(now);

  if (toastUntil && now > toastUntil) {
    toastUntil = 0;
    drawLabel();
  }

  if (now > nextSync) {  // sinkron WiFi (layar diam beberapa detik)
    syncNow();
    now = millis();
  }

  // mode malam kalau lama tidak disentuh
  if (!sleeping && now - lastActivity > SLEEP_DIM_MS) goSleep();
  if (sleeping && BACKLIGHT_PIN >= 0 && !screenOff && now - lastActivity > SCREEN_OFF_MS) {
    screenOff = true;
    setBacklight(0);
  }

  // ganti halaman otomatis
  if (AUTO_ROTATE && !sleeping && now > pageUntil && now > reactUntil) nextPage(false);

  if (!screenOff) drawStatusBar();

  // halaman wajah butuh ~30 FPS, halaman lain cukup pelan (hemat daya)
  unsigned long frameMs = (page == P_FACE) ? 33 : 100;
  if (now - lastFrame < frameMs) return;
  lastFrame = now;

  if (screenOff) return;

  if (page == P_FACE) {
    // kedip acak
    if (now > nextBlink && blinkDir == 0) {
      blinkDir = -1;
      nextBlink = now + random(2000, 5500);
    }
    if (blinkDir != 0) {
      blinkAmt += blinkDir * 0.25;
      if (blinkAmt <= 0.05) { blinkAmt = 0.05; blinkDir = 1; }
      if (blinkAmt >= 1.0)  { blinkAmt = 1.0;  blinkDir = 0; }
    }

    // melirik acak
    if (now > nextLook) {
      tgtX = random(-30, 31);
      tgtY = random(-15, 16);
      if (random(4) == 0 || face == F_DIZZY || face == F_COOL) { tgtX = 0; tgtY = 0; }
      nextLook = now + random(1200, 4000);
    }
    lookX += (tgtX - lookX) * 0.25;
    lookY += (tgtY - lookY) * 0.25;

    // ganti wajah otomatis (tanpa suara)
    if (AUTO_CHANGE && now > nextFace && now > reactUntil) {
      Face f;
      do { f = (Face)random(FACE_COUNT); } while (f == face);
      setFace(f, false);
    }

    if (shownFace != (int)face) {
      shownFace = face;
      drawLabel();
    }
    drawFace();
  } else if (page == P_CLOCK) {
    drawClockPage();
  } else {
    drawWeatherPage();
  }
}
