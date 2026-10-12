#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>

// Wiring: SCL=4, SDA=6, DC=1, RES=0
Adafruit_ST7789 tft = Adafruit_ST7789(&SPI, -1, 1, 0);

// Pengaturan layar (sama seperti sketch sebelumnya)
const int  SCREEN_ROTATION = 1;
const bool INVERT_COLORS   = true;

const uint16_t BG = ST77XX_BLACK;
const uint16_t GRAY = 0x4208;

const char *labels[3] = {"CPU", "RAM", "GPU"};
int vals[3]     = {-1, -1, -1};   // -1 = tidak ada data
int shownVals[3] = {-2, -2, -2};  // nilai yang sedang tampil di layar

String buf;
unsigned long lastData = 0;
bool linked = false;
bool lastLinked = true;  // paksa gambar judul pertama kali

uint16_t colorFor(int v) {
  if (v < 60) return ST77XX_GREEN;
  if (v < 85) return ST77XX_YELLOW;
  return ST77XX_RED;
}

void drawTitle() {
  tft.fillRect(0, 0, 240, 28, BG);
  tft.setTextSize(2);
  tft.setCursor(linked ? 50 : 40, 6);
  if (linked) {
    tft.setTextColor(ST77XX_CYAN, BG);
    tft.print("PC MONITOR");
  } else {
    tft.setTextColor(ST77XX_RED, BG);
    tft.print("MENUNGGU PC");
  }
}

void drawRow(int i) {
  int v = vals[i];
  int y = 32 + i * 68;

  // label
  tft.setTextSize(2);
  tft.setTextColor(ST77XX_WHITE, BG);
  tft.setCursor(10, y + 8);
  tft.print(labels[i]);

  // angka besar (dipadding supaya angka lama tertimpa)
  tft.setTextSize(4);
  tft.setCursor(110, y);
  if (v < 0) {
    tft.setTextColor(GRAY, BG);
    tft.print(" -- ");
  } else {
    tft.setTextColor(colorFor(v), BG);
    char t[8];
    snprintf(t, sizeof(t), "%3d%%", v);
    tft.print(t);
  }

  // bar
  int bx = 10, by = y + 38, bw = 220, bh = 14;
  tft.drawRect(bx, by, bw, bh, GRAY);
  int fill = (v < 0) ? 0 : (bw - 4) * constrain(v, 0, 100) / 100;
  tft.fillRect(bx + 2, by + 2, fill, bh - 4, v < 0 ? GRAY : colorFor(v));
  tft.fillRect(bx + 2 + fill, by + 2, (bw - 4) - fill, bh - 4, BG);
}

void parseLine() {
  int c, r, g;
  if (sscanf(buf.c_str(), "%d,%d,%d", &c, &r, &g) == 3) {
    vals[0] = constrain(c, -1, 100);
    vals[1] = constrain(r, -1, 100);
    vals[2] = constrain(g, -1, 100);
    lastData = millis();
  }
}

void setup() {
  Serial.begin(115200);
  SPI.begin(4, 5, 6, -1);
  tft.init(240, 240, SPI_MODE3);
  tft.setSPISpeed(40000000);
  tft.setRotation(SCREEN_ROTATION);
  tft.invertDisplay(INVERT_COLORS);
  tft.fillScreen(BG);
  for (int i = 0; i < 3; i++) drawRow(i);
}

void loop() {
  // baca data dari PC, satu baris: "CPU,RAM,GPU\n"
  while (Serial.available()) {
    char ch = Serial.read();
    if (ch == '\n') {
      parseLine();
      buf = "";
    } else if (ch != '\r' && buf.length() < 40) {
      buf += ch;
    }
  }

  // status koneksi: dianggap putus kalau 3 detik tanpa data
  linked = (lastData != 0) && (millis() - lastData < 3000);
  if (linked != lastLinked) {
    lastLinked = linked;
    drawTitle();
    if (!linked) {
      for (int i = 0; i < 3; i++) vals[i] = -1;
    }
  }

  // gambar ulang hanya baris yang nilainya berubah
  for (int i = 0; i < 3; i++) {
    if (vals[i] != shownVals[i]) {
      shownVals[i] = vals[i];
      drawRow(i);
    }
  }
}
