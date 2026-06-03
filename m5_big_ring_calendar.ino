#include <M5Unified.h>
#include <WiFi.h>
#include <time.h>

#include "secrets.h"

// ============================================================
// M5Stack Basic V2.7 - Multi Mode Calendar
// ------------------------------------------------------------
// Modes:
//   1. Default: Big Ring Calendar
//   2. Work: Workday progress timeline
//   3. Table: cal-command style month table
//
// Button:
//   - A / left button: switch display mode
//   - B / center button: resync NTP
//   - C / right button: force redraw
//
// Brightness:
//   - 07:00 - 18:59 -> 60
//   - 19:00 - 23:59 -> 20
//   - 00:00 - 06:59 -> 10
//
// Notes:
//   - M5.update() must be called frequently.
//   - Do not use long delay() in loop(), otherwise button clicks are missed.
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
static constexpr uint16_t COLOR_PANEL     = 0x18E3;  // dark panel gray

// Weekday colors
static constexpr uint16_t COLOR_WEEKDAY = 0x07E0; // green
static constexpr uint16_t COLOR_SAT     = 0x07FF; // cyan
static constexpr uint16_t COLOR_SUN     = 0xF81F; // pink/magenta

// Default / Big Ring layout
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

// Runtime intervals.
static constexpr unsigned long LOOP_IDLE_DELAY_MS = 20;
static constexpr unsigned long CLOCK_CHECK_INTERVAL_MS = 1000;
static constexpr unsigned long WIFI_RETRY_INTERVAL_MS = 10UL * 60UL * 1000UL;
static constexpr unsigned long BUTTON_DEBOUNCE_MS = 250;

// ------------------------------------------------------------
// State
// ------------------------------------------------------------
enum DisplayMode {
  MODE_DEFAULT = 0,
  MODE_WORK = 1,
  MODE_TABLE = 2,
  MODE_COUNT = 3
};

static DisplayMode currentMode = MODE_DEFAULT;

static int lastDrawnYear = -1;
static int lastDrawnMonth = -1;
static int lastDrawnDay = -1;
static int lastDrawnMode = -1;

static int lastAppliedBrightness = -1;

static unsigned long lastWifiRetryAt = 0;
static unsigned long lastClockCheckAt = 0;
static unsigned long lastButtonAcceptedAt = 0;

// ------------------------------------------------------------
// Text helpers
// ------------------------------------------------------------
void drawTextCenter(const char* text, int x, int y, int textSize, uint16_t color, uint16_t bg = COLOR_BG) {
  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextSize(textSize);
  M5.Display.setTextColor(color, bg);
  M5.Display.drawString(text, x, y);
}

void drawTextLeft(const char* text, int x, int y, int textSize, uint16_t color, uint16_t bg = COLOR_BG) {
  M5.Display.setTextDatum(top_left);
  M5.Display.setTextSize(textSize);
  M5.Display.setTextColor(color, bg);
  M5.Display.drawString(text, x, y);
}

void drawTextRight(const char* text, int x, int y, int textSize, uint16_t color, uint16_t bg = COLOR_BG) {
  M5.Display.setTextDatum(top_right);
  M5.Display.setTextSize(textSize);
  M5.Display.setTextColor(color, bg);
  M5.Display.drawString(text, x, y);
}

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

const char* monthName(int month) {
  static const char* names[] = {
    "UNKNOWN",
    "JANUARY", "FEBRUARY", "MARCH", "APRIL", "MAY", "JUNE",
    "JULY", "AUGUST", "SEPTEMBER", "OCTOBER", "NOVEMBER", "DECEMBER"
  };

  if (month < 1 || month > 12) {
    return names[0];
  }
  return names[month];
}

const char* modeName(DisplayMode mode) {
  switch (mode) {
    case MODE_DEFAULT: return "DEFAULT";
    case MODE_WORK:    return "WORK";
    case MODE_TABLE:   return "TABLE";
    default:           return "UNKNOWN";
  }
}

uint16_t weekdayColor(int wday) {
  // tm_wday: 0=SUN, 1=MON, ... 6=SAT
  if (wday == 0) return COLOR_SUN;
  if (wday == 6) return COLOR_SAT;
  return COLOR_WEEKDAY;
}

uint16_t weekdayColorByColumn(int col) {
  // Calendar table column: 0=SUN, ... 6=SAT
  if (col == 0) return COLOR_SUN;
  if (col == 6) return COLOR_SAT;
  return COLOR_MAIN;
}

uint16_t calendarAccentColorByColumn(int col) {
  if (col == 0) return COLOR_SUN;
  if (col == 6) return COLOR_SAT;
  return COLOR_WEEKDAY;
}

void formatNumber2(char* buffer, size_t size, int value) {
  if (ZERO_PAD_DATE) {
    snprintf(buffer, size, "%02d", value);
  } else {
    snprintf(buffer, size, "%d", value);
  }
}

int firstWeekdayOfMonth(const struct tm& timeinfo) {
  // Return 0=SUN, 1=MON, ... 6=SAT
  const int day = timeinfo.tm_mday;
  const int wday = timeinfo.tm_wday;
  return (wday - ((day - 1) % 7) + 7) % 7;
}

// ------------------------------------------------------------
// Brightness
// ------------------------------------------------------------
uint8_t brightnessByHour(int hour) {
  // 07:00 - 18:59
  if (hour >= 7 && hour <= 18) {
    return 60;
  }

  // 19:00 - 23:59
  if (hour >= 19 && hour <= 23) {
    return 20;
  }

  // 00:00 - 06:59
  return 10;
}

void applyAutoBrightness(const struct tm& timeinfo) {
  const int brightness = brightnessByHour(timeinfo.tm_hour);

  if (brightness == lastAppliedBrightness) {
    return;
  }

  M5.Display.setBrightness(brightness);
  lastAppliedBrightness = brightness;

  Serial.print("[brightness] ");
  Serial.println(brightness);
}

// ------------------------------------------------------------
// Wi-Fi / Time
// ------------------------------------------------------------
void drawStatus(const char* line1, const char* line2 = nullptr) {
  M5.Display.fillScreen(COLOR_BG);

  drawTextCenter(line1, SCREEN_W / 2, 100, 3, COLOR_MAIN);

  if (line2 != nullptr) {
    drawTextCenter(line2, SCREEN_W / 2, 140, 2, COLOR_SUB);
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
    M5.update();
    delay(20);
  }

  return WiFi.status() == WL_CONNECTED;
}

bool syncTime() {
  if (!connectWiFi()) {
    drawStatus("NO WIFI", "CHECK SETTINGS");
    Serial.println("[ntp] wifi failed");
    return false;
  }

  drawStatus("NTP", "SYNCING");

  configTzTime(TIMEZONE, NTP_SERVER_1, NTP_SERVER_2, NTP_SERVER_3);

  struct tm timeinfo;
  const bool ok = getLocalTime(&timeinfo, 15000);

  if (!ok) {
    drawStatus("NO TIME", "NTP FAILED");
    Serial.println("[ntp] sync failed");
    return false;
  }

  applyAutoBrightness(timeinfo);
  Serial.println("[ntp] sync ok");
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

// ------------------------------------------------------------
// Mode: Default / Big Ring Calendar
// ------------------------------------------------------------
void drawDefaultMode(const struct tm& timeinfo) {
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

  // Year
  drawTextCenter(yearText, 44, 28, 2, COLOR_DIM);

  // Month progress ring
  drawMonthProgressRing(year, month, day, accent);

  // Month - small
  drawTextCenter(monthText, RING_CX, 84, 4, COLOR_SUB);

  // Separator
  M5.Display.drawLine(78, 112, 166, 112, COLOR_DIM);

  // Day - large
  drawTextCenter(dayText, RING_CX, 162, 7, COLOR_MAIN);

  // Weekday box
  M5.Display.drawRoundRect(
    WEEKDAY_BOX_X,
    WEEKDAY_BOX_Y,
    WEEKDAY_BOX_W,
    WEEKDAY_BOX_H,
    WEEKDAY_BOX_R,
    accent
  );

  drawTextCenter(
    weekdayName(wday),
    WEEKDAY_BOX_X + WEEKDAY_BOX_W / 2,
    WEEKDAY_BOX_Y + WEEKDAY_BOX_H / 2,
    3,
    accent
  );
}

// ------------------------------------------------------------
// Mode: Work
// ------------------------------------------------------------
void drawWorkModeWeekday(const struct tm& timeinfo) {
  const int year = timeinfo.tm_year + 1900;
  const int month = timeinfo.tm_mon + 1;
  const int day = timeinfo.tm_mday;
  const int wday = timeinfo.tm_wday; // 1=MON ... 5=FRI

  char yearMonthText[12];
  char dayText[4];
  char dayWithWeekdayText[16];
  char workdayText[16];

  snprintf(yearMonthText, sizeof(yearMonthText), "%d.%02d", year, month);
  snprintf(dayText, sizeof(dayText), "%02d", day);
  snprintf(dayWithWeekdayText, sizeof(dayWithWeekdayText), "%02d (%s)", day, weekdayName(wday));
  snprintf(workdayText, sizeof(workdayText), "DAY %d/5", wday);

  M5.Display.fillScreen(COLOR_BG);

  // Header: keep the year/month visible but small.
  drawTextCenter(yearMonthText, SCREEN_W / 2, 26, 2, COLOR_DIM);

  // Main: date and weekday are combined for better visibility.
  drawTextCenter(dayWithWeekdayText, SCREEN_W / 2, 82, 5, COLOR_MAIN);
  drawTextCenter(workdayText, SCREEN_W / 2, 130, 3, COLOR_WEEKDAY);

  // Timeline: five workdays.
  const int y = 178;
  const int nodeX[5] = { 52, 106, 160, 214, 268 };

  M5.Display.drawLine(nodeX[0], y, nodeX[4], y, COLOR_DIM);

  const int currentIndex = wday - 1;

  for (int i = 0; i < 5; ++i) {
    const bool done = (i <= currentIndex);
    const bool current = (i == currentIndex);

    const int r = current ? 14 : 10;
    const uint16_t fillColor = done ? COLOR_WEEKDAY : COLOR_PANEL;
    const uint16_t outlineColor = current ? COLOR_WEEKDAY : COLOR_DIM;

    M5.Display.fillCircle(nodeX[i], y, r, fillColor);
    M5.Display.drawCircle(nodeX[i], y, r, outlineColor);
  }

  // Minimal labels under the progress dots.
  drawTextCenter("MON", nodeX[0], y + 34, 1, COLOR_DIM);
  drawTextCenter("TUE", nodeX[1], y + 34, 1, COLOR_DIM);
  drawTextCenter("WED", nodeX[2], y + 34, 1, COLOR_DIM);
  drawTextCenter("THU", nodeX[3], y + 34, 1, COLOR_DIM);
  drawTextCenter("FRI", nodeX[4], y + 34, 1, COLOR_DIM);
}

void drawWorkModeWeekend(const struct tm& timeinfo) {
  const int year = timeinfo.tm_year + 1900;
  const int month = timeinfo.tm_mon + 1;
  const int day = timeinfo.tm_mday;
  const int wday = timeinfo.tm_wday;
  const uint16_t accent = weekdayColor(wday);

  char yearMonthText[12];
  char dayWithWeekdayText[16];

  snprintf(yearMonthText, sizeof(yearMonthText), "%d.%02d", year, month);
  snprintf(dayWithWeekdayText, sizeof(dayWithWeekdayText), "%02d (%s)", day, weekdayName(wday));

  M5.Display.fillScreen(COLOR_BG);

  // Header: keep the year/month visible but small.
  drawTextCenter(yearMonthText, SCREEN_W / 2, 26, 2, COLOR_DIM);

  // Main
  drawTextCenter(dayWithWeekdayText, SCREEN_W / 2, 78, 5, accent);
  drawTextCenter("FREE", SCREEN_W / 2, 130, 5, COLOR_MAIN);
  drawTextCenter("NO WORKDAY PROGRESS", SCREEN_W / 2, 170, 2, COLOR_DIM);

  // Weekend capsule
  M5.Display.drawRoundRect(50, 198, 220, 20, 10, accent);
  drawTextCenter("WEEKEND BONUS TIME", SCREEN_W / 2, 208, 1, accent);
}

void drawWorkMode(const struct tm& timeinfo) {
  const int wday = timeinfo.tm_wday;

  if (wday >= 1 && wday <= 5) {
    drawWorkModeWeekday(timeinfo);
  } else {
    drawWorkModeWeekend(timeinfo);
  }
}

// ------------------------------------------------------------
// Mode: Table
// ------------------------------------------------------------
void drawTableMode(const struct tm& timeinfo) {
  const int year = timeinfo.tm_year + 1900;
  const int month = timeinfo.tm_mon + 1;
  const int today = timeinfo.tm_mday;

  const int totalDays = daysInMonth(year, month);
  const int firstWday = firstWeekdayOfMonth(timeinfo);
  const int todayIndex = firstWday + today - 1;
  const int todayRow = todayIndex / 7;
  const int todayCol = todayIndex % 7;

  char commandText[24];
  char titleText[24];

  snprintf(commandText, sizeof(commandText), "$ cal %02d %d", month, year);
  snprintf(titleText, sizeof(titleText), "%s %d", monthName(month), year);

  M5.Display.fillScreen(COLOR_BG);

  // Command line
  drawTextLeft(commandText, 18, 12, 2, COLOR_WEEKDAY);

  // Month title
  drawTextCenter(titleText, SCREEN_W / 2, 53, 2, COLOR_MAIN);
  M5.Display.drawLine(24, 70, 296, 70, COLOR_DIM);

  // Calendar grid
  const int x0 = 25;
  const int y0 = 80;
  const int cw = 38;
  const int rowH = 20;

  const char* labels[7] = { "S", "M", "T", "W", "T", "F", "S" };

  for (int col = 0; col < 7; ++col) {
    uint16_t color = COLOR_DIM;
    if (col == 0) color = COLOR_SUN;
    if (col == 6) color = COLOR_SAT;

    drawTextCenter(labels[col], x0 + col * cw + cw / 2, y0 + 8, 1, color);
  }

  // Highlight current week first, so day numbers are drawn on top.
  const int highlightY = y0 + 22 + todayRow * rowH;
  M5.Display.fillRoundRect(x0 - 2, highlightY - 1, 7 * cw + 4, 18, 8, COLOR_PANEL);

  for (int day = 1; day <= totalDays; ++day) {
    const int cellIndex = firstWday + day - 1;
    const int row = cellIndex / 7;
    const int col = cellIndex % 7;

    const int cellCenterX = x0 + col * cw + cw / 2;
    const int cellCenterY = y0 + 22 + row * rowH + 8;

    char dayText[4];
    snprintf(dayText, sizeof(dayText), "%d", day);

    if (day == today) {
      const uint16_t accent = calendarAccentColorByColumn(todayCol);
      M5.Display.fillCircle(cellCenterX, cellCenterY, 10, accent);
      drawTextCenter(dayText, cellCenterX, cellCenterY, 1, TFT_BLACK, accent);
    } else {
      drawTextCenter(dayText, cellCenterX, cellCenterY, 1, weekdayColorByColumn(col));
    }
  }
}

// ------------------------------------------------------------
// Mode dispatcher
// ------------------------------------------------------------
void drawCurrentMode(const struct tm& timeinfo) {
  applyAutoBrightness(timeinfo);

  Serial.print("[draw] mode=");
  Serial.print(modeName(currentMode));
  Serial.print(" date=");
  Serial.print(timeinfo.tm_year + 1900);
  Serial.print("-");
  Serial.print(timeinfo.tm_mon + 1);
  Serial.print("-");
  Serial.println(timeinfo.tm_mday);

  switch (currentMode) {
    case MODE_DEFAULT:
      drawDefaultMode(timeinfo);
      break;
    case MODE_WORK:
      drawWorkMode(timeinfo);
      break;
    case MODE_TABLE:
      drawTableMode(timeinfo);
      break;
    default:
      drawDefaultMode(timeinfo);
      break;
  }

  lastDrawnYear = timeinfo.tm_year + 1900;
  lastDrawnMonth = timeinfo.tm_mon + 1;
  lastDrawnDay = timeinfo.tm_mday;
  lastDrawnMode = currentMode;
}

void redrawIfNeeded() {
  struct tm timeinfo;

  if (!getLocalTime(&timeinfo, 100)) {
    return;
  }

  applyAutoBrightness(timeinfo);

  const int year = timeinfo.tm_year + 1900;
  const int month = timeinfo.tm_mon + 1;
  const int day = timeinfo.tm_mday;

  if (
    year != lastDrawnYear ||
    month != lastDrawnMonth ||
    day != lastDrawnDay ||
    currentMode != lastDrawnMode
  ) {
    drawCurrentMode(timeinfo);
  }
}

void forceRedraw() {
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 1000)) {
    drawCurrentMode(timeinfo);
  }
}

// ------------------------------------------------------------
// Button handling
// ------------------------------------------------------------
bool acceptButtonEvent() {
  const unsigned long now = millis();

  if (now - lastButtonAcceptedAt < BUTTON_DEBOUNCE_MS) {
    return false;
  }

  lastButtonAcceptedAt = now;
  return true;
}

void switchMode() {
  currentMode = static_cast<DisplayMode>((currentMode + 1) % MODE_COUNT);

  Serial.print("[button] A switch mode -> ");
  Serial.println(modeName(currentMode));

  forceRedraw();
}

void handleButtons() {
  // wasClicked() becomes reliable only when M5.update() is called frequently.
  if (M5.BtnA.wasClicked() && acceptButtonEvent()) {
    switchMode();
  }

  if (M5.BtnB.wasClicked() && acceptButtonEvent()) {
    Serial.println("[button] B ntp sync");

    if (syncTime()) {
      forceRedraw();
    }
  }

  if (M5.BtnC.wasClicked() && acceptButtonEvent()) {
    Serial.println("[button] C redraw");
    forceRedraw();
  }
}

// ------------------------------------------------------------
// Arduino lifecycle
// ------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(200);

  auto cfg = M5.config();
  M5.begin(cfg);

  M5.Display.setRotation(1);
  M5.Display.setBrightness(60);
  M5.Display.fillScreen(COLOR_BG);

  Serial.println("[boot] M5 Multi Mode Calendar");

  bool timeOk = syncTime();

  if (timeOk) {
    forceRedraw();
  }
}

void loop() {
  M5.update();

  handleButtons();

  const unsigned long now = millis();

  // If Wi-Fi was not available on boot, retry occasionally.
  if (WiFi.status() != WL_CONNECTED) {
    if (now - lastWifiRetryAt > WIFI_RETRY_INTERVAL_MS) {
      lastWifiRetryAt = now;
      if (syncTime()) {
        forceRedraw();
      }
    }
  }

  // Clock/date/brightness check.
  if (now - lastClockCheckAt >= CLOCK_CHECK_INTERVAL_MS) {
    lastClockCheckAt = now;
    redrawIfNeeded();
  }

  delay(LOOP_IDLE_DELAY_MS);
}
