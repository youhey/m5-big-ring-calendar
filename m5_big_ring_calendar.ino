#include <M5Unified.h>
#include <WiFi.h>
#include <time.h>

#include "secrets.h"

// ============================================================
// M5Stack Basic V2.7 - Big Ring Calendar
// ------------------------------------------------------------
// Display:
//   - Year: small
//   - Month: small number
//   - Day: large number
//   - Weekday: SUN/MON/TUE/WED/THU/FRI/SAT
//   - Outer ring: progress of current month
//
// Accent color:
//   - Weekday: green
//   - Saturday: cyan
//   - Sunday: pink/red
// ============================================================

// ------------------------------------------------------------
// NTP settings
// ------------------------------------------------------------
// JST = UTC+9. POSIX TZ format uses reversed sign, so JST-9.
static const char* TIMEZONE = "JST-9";
static const char* NTP_SERVER_1 = "ntp.nict.jp";
static const char* NTP_SERVER_2 = "pool.ntp.org";
static const char* NTP_SERVER_3 = "time.google.com";

// ------------------------------------------------------------
// Display settings
// ------------------------------------------------------------
static constexpr int SCREEN_W = 320;
static constexpr int SCREEN_H = 240;

// RGB565 colors
static constexpr uint16_t COLOR_BG        = TFT_BLACK;
static constexpr uint16_t COLOR_MAIN      = TFT_WHITE;
static constexpr uint16_t COLOR_DIM       = TFT_DARKGREY;
static constexpr uint16_t COLOR_SUB       = 0x8410;  // middle gray
static constexpr uint16_t COLOR_RING_BASE = 0x2104;  // very dark gray

// Weekday colors
static constexpr uint16_t COLOR_WEEKDAY = 0x07E0; // green
static constexpr uint16_t COLOR_SAT     = 0x07FF; // cyan
static constexpr uint16_t COLOR_SUN     = 0xF81F; // pink/magenta

// Layout
static constexpr int RING_CX = 122;
static constexpr int RING_CY = 134;
static constexpr int RING_OUTER_R = 90;
static constexpr int RING_INNER_R = 84;

static constexpr int WEEKDAY_BOX_X = 226;
static constexpr int WEEKDAY_BOX_Y = 82;
static constexpr int WEEKDAY_BOX_W = 72;
static constexpr int WEEKDAY_BOX_H = 104;
static constexpr int WEEKDAY_BOX_R = 14;

// If true, month/day are displayed as 06 / 03.
// If false, month/day are displayed as 6 / 3.
static constexpr bool ZERO_PAD_DATE = true;

// Redraw check interval.
static constexpr unsigned long LOOP_DELAY_MS = 1000;
static constexpr unsigned long WIFI_RETRY_INTERVAL_MS = 10UL * 60UL * 1000UL;

// ------------------------------------------------------------
// State
// ------------------------------------------------------------
static int lastDrawnYear = -1;
static int lastDrawnMonth = -1;
static int lastDrawnDay = -1;

static unsigned long lastWifiRetryAt = 0;

// ------------------------------------------------------------
// Calendar utilities
// ------------------------------------------------------------
bool isLeapYear(int year) {
  if (year % 400 == 0) return true;
  if (year % 100 == 0) return false;
  return (year % 4 == 0);
}

int daysInMonth(int year, int month) {
  switch (month) {
    case 1:  return 31;
    case 2:  return isLeapYear(year) ? 29 : 28;
    case 3:  return 31;
    case 4:  return 30;
    case 5:  return 31;
    case 6:  return 30;
    case 7:  return 31;
    case 8:  return 31;
    case 9:  return 30;
    case 10: return 31;
    case 11: return 30;
    case 12: return 31;
    default: return 30;
  }
}

const char* weekdayName(int wday) {
  // tm_wday: 0=SUN, 1=MON, ... 6=SAT
  static const char* names[] = {
    "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"
  };

  if (wday < 0 || wday > 6) {
    return "---";
  }
  return names[wday];
}

uint16_t weekdayColor(int wday) {
  // tm_wday: 0=SUN, 1=MON, ... 6=SAT
  if (wday == 0) return COLOR_SUN;
  if (wday == 6) return COLOR_SAT;
  return COLOR_WEEKDAY;
}

void formatNumber2(char* buffer, size_t size, int value) {
  if (ZERO_PAD_DATE) {
    snprintf(buffer, size, "%02d", value);
  } else {
    snprintf(buffer, size, "%d", value);
  }
}

// ------------------------------------------------------------
// Wi-Fi / Time
// ------------------------------------------------------------
void drawStatus(const char* line1, const char* line2 = nullptr) {
  M5.Display.fillScreen(COLOR_BG);
  M5.Display.setTextDatum(middle_center);

  M5.Display.setTextColor(COLOR_MAIN, COLOR_BG);
  M5.Display.setTextSize(3);
  M5.Display.drawString(line1, SCREEN_W / 2, 100);

  if (line2 != nullptr) {
    M5.Display.setTextColor(COLOR_SUB, COLOR_BG);
    M5.Display.setTextSize(2);
    M5.Display.drawString(line2, SCREEN_W / 2, 140);
  }
}

bool connectWiFi(unsigned long timeoutMs = 15000) {
  if (WiFi.status() == WL_CONNECTED) {
    return true;
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  drawStatus("WIFI", "CONNECTING");

  const unsigned long startedAt = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - startedAt) < timeoutMs) {
    delay(250);
    M5.update();
  }

  return WiFi.status() == WL_CONNECTED;
}

bool syncTime() {
  if (!connectWiFi()) {
    drawStatus("NO WIFI", "CHECK SETTINGS");
    return false;
  }

  drawStatus("NTP", "SYNCING");

  configTzTime(TIMEZONE, NTP_SERVER_1, NTP_SERVER_2, NTP_SERVER_3);

  struct tm timeinfo;
  const bool ok = getLocalTime(&timeinfo, 15000);

  if (!ok) {
    drawStatus("NO TIME", "NTP FAILED");
    return false;
  }

  return true;
}

// ------------------------------------------------------------
// Drawing helpers
// ------------------------------------------------------------
void drawMonthProgressRing(int year, int month, int day, uint16_t accent) {
  const int totalDays = daysInMonth(year, month);

  // Base ring: full month.
  M5.Display.fillArc(
    RING_CX,
    RING_CY,
    RING_INNER_R,
    RING_OUTER_R,
    0,
    360,
    COLOR_RING_BASE
  );

  // Progress ring: elapsed portion of current month.
  float progress = (float)day / (float)totalDays;
  int sweep = (int)(360.0f * progress + 0.5f);

  if (sweep < 1) {
    sweep = 1;
  }
  if (sweep > 360) {
    sweep = 360;
  }

  // Start from 12 o'clock.
  const int startDeg = 270;
  int endDeg = startDeg + sweep;

  if (endDeg <= 360) {
    M5.Display.fillArc(
      RING_CX,
      RING_CY,
      RING_INNER_R,
      RING_OUTER_R,
      startDeg,
      endDeg,
      accent
    );
  } else {
    M5.Display.fillArc(
      RING_CX,
      RING_CY,
      RING_INNER_R,
      RING_OUTER_R,
      startDeg,
      360,
      accent
    );
    M5.Display.fillArc(
      RING_CX,
      RING_CY,
      RING_INNER_R,
      RING_OUTER_R,
      0,
      endDeg - 360,
      accent
    );
  }
}

void drawCalendar(const struct tm& timeinfo) {
  const int year = timeinfo.tm_year + 1900;
  const int month = timeinfo.tm_mon + 1;
  const int day = timeinfo.tm_mday;
  const int wday = timeinfo.tm_wday;

  const uint16_t accent = weekdayColor(wday);

  char yearText[8];
  char monthText[4];
  char dayText[4];

  snprintf(yearText, sizeof(yearText), "%d", year);
  formatNumber2(monthText, sizeof(monthText), month);
  formatNumber2(dayText, sizeof(dayText), day);

  M5.Display.fillScreen(COLOR_BG);
  M5.Display.setTextDatum(middle_center);

  // Year
  M5.Display.setTextColor(COLOR_DIM, COLOR_BG);
  M5.Display.setTextSize(2);
  M5.Display.drawString(yearText, 44, 28);

  // Month progress ring
  drawMonthProgressRing(year, month, day, accent);

  // Month - small
  M5.Display.setTextColor(COLOR_SUB, COLOR_BG);
  M5.Display.setTextSize(4);
  M5.Display.drawString(monthText, RING_CX, 84);

  // Separator
  M5.Display.drawLine(78, 112, 166, 112, COLOR_DIM);

  // Day - large
  M5.Display.setTextColor(COLOR_MAIN, COLOR_BG);
  M5.Display.setTextSize(7);
  M5.Display.drawString(dayText, RING_CX, 162);

  // Weekday box
  M5.Display.drawRoundRect(
    WEEKDAY_BOX_X,
    WEEKDAY_BOX_Y,
    WEEKDAY_BOX_W,
    WEEKDAY_BOX_H,
    WEEKDAY_BOX_R,
    accent
  );

  M5.Display.setTextColor(accent, COLOR_BG);
  M5.Display.setTextSize(3);
  M5.Display.drawString(
    weekdayName(wday),
    WEEKDAY_BOX_X + WEEKDAY_BOX_W / 2,
    WEEKDAY_BOX_Y + WEEKDAY_BOX_H / 2
  );

  lastDrawnYear = year;
  lastDrawnMonth = month;
  lastDrawnDay = day;
}

void redrawIfNeeded() {
  struct tm timeinfo;

  if (!getLocalTime(&timeinfo, 1000)) {
    return;
  }

  const int year = timeinfo.tm_year + 1900;
  const int month = timeinfo.tm_mon + 1;
  const int day = timeinfo.tm_mday;

  if (year != lastDrawnYear || month != lastDrawnMonth || day != lastDrawnDay) {
    drawCalendar(timeinfo);
  }
}

// ------------------------------------------------------------
// Arduino lifecycle
// ------------------------------------------------------------
void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);

  M5.Display.setRotation(1);
  M5.Display.setBrightness(120);
  M5.Display.fillScreen(COLOR_BG);

  bool timeOk = syncTime();

  if (timeOk) {
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 1000)) {
      drawCalendar(timeinfo);
    }
  }
}

void loop() {
  M5.update();

  // Button A: force redraw
  if (M5.BtnA.wasClicked()) {
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 1000)) {
      drawCalendar(timeinfo);
    }
  }

  // Button B: resync NTP
  if (M5.BtnB.wasClicked()) {
    if (syncTime()) {
      struct tm timeinfo;
      if (getLocalTime(&timeinfo, 1000)) {
        drawCalendar(timeinfo);
      }
    }
  }

  // Button C: toggle brightness roughly
  if (M5.BtnC.wasClicked()) {
    static bool bright = false;
    bright = !bright;
    M5.Display.setBrightness(bright ? 200 : 80);

    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 1000)) {
      drawCalendar(timeinfo);
    }
  }

  // If Wi-Fi was not available on boot, retry occasionally.
  if (WiFi.status() != WL_CONNECTED) {
    const unsigned long now = millis();
    if (now - lastWifiRetryAt > WIFI_RETRY_INTERVAL_MS) {
      lastWifiRetryAt = now;
      syncTime();
    }
  }

  // Redraw only when date changes.
  redrawIfNeeded();

  delay(LOOP_DELAY_MS);
}
