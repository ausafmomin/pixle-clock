#include <Wire.h>
#include <Adafruit_NeoPixel.h>
#include <RTClib.h>
#include <EEPROM.h>

// --- ENUMS ---
enum SettingStep { SET_HOUR = 0, SET_MINUTE, SET_DAY, SET_MONTH, SET_YEAR, SET_DONE };
enum Mode { NORMAL, SET_TIME, SET_ALARM, SET_ALARM_TIME };

// --- PINS & LAYOUT ---
#define LED_PIN     4
#define BUZZER_PIN  12
#define BTN_INC     26
#define BTN_DEC     27
#define BTN_OK      14
#define BTN_CFG     15
#define DS3231_SDA  21
#define DS3231_SCL  22

#define NUM_LEDS         68
#define LEDS_PER_DIGIT   14
#define NUM_WEEKDAYS     7
#define NUM_SEPARATOR_LEDS 2
#define HOURS_TENS_START      0
#define HOURS_UNITS_START     (HOURS_TENS_START + LEDS_PER_DIGIT)
#define SEPARATOR_START       (HOURS_UNITS_START + LEDS_PER_DIGIT)
#define MINUTES_TENS_START    (SEPARATOR_START + NUM_SEPARATOR_LEDS)
#define MINUTES_UNITS_START   (MINUTES_TENS_START + LEDS_PER_DIGIT)
#define WEEKDAY_START         (MINUTES_UNITS_START + LEDS_PER_DIGIT)
#define AM_PM_LED             (WEEKDAY_START + NUM_WEEKDAYS)
#define ALARM_LED             (AM_PM_LED + 1)
#define SECONDS_LED           (ALARM_LED + 1)

// --- SEGMENT PATTERNS ---
const bool segmentPatterns[10][7] = {
  {1,1,1,0,1,1,1}, // 0
  {0,0,1,0,0,0,1}, // 1
  {0,1,1,1,1,1,0}, // 2
  {0,1,1,1,0,1,1}, // 3
  {1,0,1,1,0,0,1}, // 4
  {1,1,0,1,0,1,1}, // 5
  {1,1,0,1,1,1,1}, // 6
  {0,1,1,0,0,0,1}, // 7
  {1,1,1,1,1,1,1}, // 8
  {1,1,1,1,0,1,1}  // 9
};
const int segmentToLedMapping[7][2] = {
  {0,1},    // A
  {2,3},    // B
  {4,5},    // C
  {6,7},    // D
  {8,9},    // E
  {10,11},  // F
  {12,13}   // G
};

// --- COLORS ---
#define DIGIT_COLOR         pixels.Color(0, 255, 0)
#define BLINK_COLOR         pixels.Color(0, 0, 0)
#define SEPARATOR_COLOR     pixels.Color(255, 165, 0)
#define WEEKDAY_COLOR       pixels.Color(0, 0, 255)
#define AM_PM_COLOR         pixels.Color(255, 0, 255)
#define ALARM_COLOR         pixels.Color(255, 0, 0)
#define TEMP_COLOR          pixels.Color(0, 255, 255)
#define ALARM_SET_COLOR     pixels.Color(255, 255, 0)
#define SECONDS_LED_COLOR   pixels.Color(255, 255, 255)

// --- ALARMS & EEPROM ---
#define ALARM_MAX 5
#define EEPROM_ALARM_START 0
struct Alarm { int hour; int minute; bool enabled; };
Alarm alarms[ALARM_MAX];
int alarmCount = ALARM_MAX; // Always ALARM_MAX for consistency

void saveAlarmsToEEPROM() {
  EEPROM.begin(64);
  EEPROM.put(EEPROM_ALARM_START, alarms);
  EEPROM.commit();
}

void loadAlarmsFromEEPROM() {
  EEPROM.begin(64);
  EEPROM.get(EEPROM_ALARM_START, alarms);
  EEPROM.end();
  // Validate alarms: if garbage, initialize
  for (int i = 0; i < ALARM_MAX; i++) {
    if (alarms[i].hour < 0 || alarms[i].hour > 23 ||
        alarms[i].minute < 0 || alarms[i].minute > 59)
      alarms[i] = {0, 0, false};
  }
}

// --- GLOBAL STATE ---
Adafruit_NeoPixel pixels(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);
RTC_DS3231 rtc;
unsigned long lastMillis = 0;
int hour, minute, second, weekday, day, month, year;
float rtcTemperature = 0;

Mode currentMode = NORMAL;
SettingStep settingStep;
int setHour, setMinute, setDay, setMonth, setYear;
bool setIsPM = false;
bool blinkState = false;
unsigned long lastBlink = 0;
const unsigned long BLINK_INTERVAL = 500;
unsigned long btnHoldStart[2] = {0, 0};
unsigned long lastRepeat[2] = {0, 0};
const unsigned long initialHoldDelay = 400;
const unsigned long repeatInterval = 70;
bool ampmToggleLatch = false;

// ALARM MODE
int selectedAlarm = 0; // 0..4
int alarmSettingStep = 0; // 0: hour, 1: min, 2: enable

// --- BUTTONS ---
unsigned long lastDebounce[4] = {0, 0, 0, 0};
bool btnState[4] = {false, false, false, false};
bool lastBtnState[4] = {false, false, false, false};
const unsigned long debounceDelay = 20;
enum BtnIdx { BTN_INC_IDX = 0, BTN_DEC_IDX = 1, BTN_OK_IDX = 2, BTN_CFG_IDX = 3 };

// --- BUZZER ---
void beep(uint16_t freq, uint16_t ms) {
  tone(BUZZER_PIN, freq, ms);
  delay(ms);
  noTone(BUZZER_PIN);
}

// --- BUTTON HANDLING ---
void readButtons() {
  int pins[4] = {BTN_INC, BTN_DEC, BTN_OK, BTN_CFG};
  for (int i = 0; i < 4; i++) {
    bool reading = (i == BTN_CFG_IDX) ? (digitalRead(pins[i]) == LOW) : (digitalRead(pins[i]) == HIGH);
    if (reading != lastBtnState[i]) lastDebounce[i] = millis();
    if ((millis() - lastDebounce[i]) > debounceDelay) btnState[i] = reading;
    lastBtnState[i] = reading;
  }
}
bool btnPressed(int idx) {
  static bool prev[4] = {false, false, false, false};
  bool pressed = btnState[idx] && !prev[idx];
  prev[idx] = btnState[idx];
  if (pressed) {
    beep(1800 + idx*300, 40 + idx*10);
    delay(40);
  }
  return pressed;
}

// --- CONTINUOUS INC/DEC ---
void handleContinuousButtons() {
  unsigned long now = millis();
  // INC
  if (btnState[BTN_INC_IDX]) {
    if (btnHoldStart[0] == 0) {
      btnHoldStart[0] = now;
      lastRepeat[0] = now;
    } else if (now - btnHoldStart[0] > initialHoldDelay && now - lastRepeat[0] > repeatInterval) {
      if (currentMode == SET_TIME) {
        switch (settingStep) {
          case SET_HOUR: setHour = (setHour % 12) + 1; if (setHour == 0) setHour = 1; break;
          case SET_MINUTE: setMinute = (setMinute + 1) % 60; break;
          case SET_DAY: setDay = (setDay < 31) ? setDay + 1 : 1; break;
          case SET_MONTH: setMonth = (setMonth < 12) ? setMonth + 1 : 1; break;
          case SET_YEAR: setYear = (setYear < 2099) ? setYear + 1 : 2000; break;
          default: break;
        }
        beep(2100, 20);
      }
      if (currentMode == SET_ALARM_TIME) {
        switch (alarmSettingStep) {
          case 0: alarms[selectedAlarm].hour = (alarms[selectedAlarm].hour + 1) % 24; break;
          case 1: alarms[selectedAlarm].minute = (alarms[selectedAlarm].minute + 1) % 60; break;
          case 2: alarms[selectedAlarm].enabled = !alarms[selectedAlarm].enabled; break;
        }
        beep(2100, 20);
      }
      lastRepeat[0] = now;
    }
  } else {
    btnHoldStart[0] = 0;
    lastRepeat[0] = 0;
  }
  // DEC
  if (btnState[BTN_DEC_IDX]) {
    if (btnHoldStart[1] == 0) {
      btnHoldStart[1] = now;
      lastRepeat[1] = now;
    } else if (now - btnHoldStart[1] > initialHoldDelay && now - lastRepeat[1] > repeatInterval) {
      if (currentMode == SET_TIME) {
        switch (settingStep) {
          case SET_HOUR: setHour = (setHour > 1) ? setHour - 1 : 12; break;
          case SET_MINUTE: setMinute = (setMinute > 0) ? setMinute - 1 : 59; break;
          case SET_DAY: setDay = (setDay > 1) ? setDay - 1 : 31; break;
          case SET_MONTH: setMonth = (setMonth > 1) ? setMonth - 1 : 12; break;
          case SET_YEAR: setYear = (setYear > 2000) ? setYear - 1 : 2099; break;
          default: break;
        }
        beep(1800, 20);
      }
      if (currentMode == SET_ALARM_TIME) {
        switch (alarmSettingStep) {
          case 0: alarms[selectedAlarm].hour = (alarms[selectedAlarm].hour + 23) % 24; break;
          case 1: alarms[selectedAlarm].minute = (alarms[selectedAlarm].minute + 59) % 60; break;
          case 2: alarms[selectedAlarm].enabled = !alarms[selectedAlarm].enabled; break;
        }
        beep(1800, 20);
      }
      lastRepeat[1] = now;
    }
  } else {
    btnHoldStart[1] = 0;
    lastRepeat[1] = 0;
  }
}

// --- RTC & TEMP ---
void updateTimeFromRTC() {
  DateTime now = rtc.now();
  hour = now.hour(); minute = now.minute(); second = now.second();
  day = now.day(); month = now.month(); year = now.year(); weekday = now.dayOfTheWeek();
}
void updateTemperatureFromRTC() { rtcTemperature = rtc.getTemperature(); }

// --- RENDER DIGIT ---
void drawDigit(int digit, int start, uint32_t color, bool blink=false, bool blinkOn=true) {
  if (digit < 0 || digit > 9) {
    for (int seg=0; seg<7; seg++) {
      pixels.setPixelColor(start + segmentToLedMapping[seg][0], BLINK_COLOR);
      pixels.setPixelColor(start + segmentToLedMapping[seg][1], BLINK_COLOR);
    }
    return;
  }
  if (blink && !blinkOn) {
    for (int seg=0; seg<7; seg++) {
      pixels.setPixelColor(start + segmentToLedMapping[seg][0], BLINK_COLOR);
      pixels.setPixelColor(start + segmentToLedMapping[seg][1], BLINK_COLOR);
    }
    return;
  }
  for (int seg = 0; seg < 7; seg++) {
    if (segmentPatterns[digit][seg]) {
      pixels.setPixelColor(start + segmentToLedMapping[seg][0], color);
      pixels.setPixelColor(start + segmentToLedMapping[seg][1], color);
    }
  }
}

// --- RENDER FUNCTIONS ---
void renderClockTime(int dispHour, int dispMinute, int dispWeekday, int dispSecond, bool blinkH=false, bool blinkHOn=true, bool blinkM=false, bool blinkMOn=true) {
  pixels.clear();

  // Only draw hours tens digit if it's not zero (hide leading zero)
  if (dispHour >= 10) {
    drawDigit(dispHour / 10, HOURS_TENS_START, DIGIT_COLOR, blinkH, blinkHOn);
  }
  drawDigit(dispHour % 10, HOURS_UNITS_START, DIGIT_COLOR, blinkH, blinkHOn);

  drawDigit(dispMinute / 10, MINUTES_TENS_START, DIGIT_COLOR, blinkM, blinkMOn);
  drawDigit(dispMinute % 10, MINUTES_UNITS_START, DIGIT_COLOR, blinkM, blinkMOn);

  int ledWeekday = (dispWeekday + 6) % 7;
  if (ledWeekday >= 0 && ledWeekday < NUM_WEEKDAYS)
    pixels.setPixelColor(WEEKDAY_START + ledWeekday, WEEKDAY_COLOR);

  if (hour >= 12) pixels.setPixelColor(AM_PM_LED, AM_PM_COLOR);

  // ALARM LED if any enabled alarm
  bool anyAlarm = false;
  for (int i=0; i<ALARM_MAX; i++) if (alarms[i].enabled) anyAlarm = true;
  pixels.setPixelColor(ALARM_LED, anyAlarm ? ALARM_COLOR : BLINK_COLOR);

  // Blinking colon separator and seconds LED
  static bool sepOn = true;
  static unsigned long lastSepBlink = 0;
  if (millis() - lastSepBlink >= 500) {
    sepOn = !sepOn;
    lastSepBlink = millis();
  }
  pixels.setPixelColor(SEPARATOR_START, sepOn ? SEPARATOR_COLOR : BLINK_COLOR);
  pixels.setPixelColor(SEPARATOR_START + 1, sepOn ? SEPARATOR_COLOR : BLINK_COLOR);
  pixels.setPixelColor(SECONDS_LED, sepOn ? SECONDS_LED_COLOR : BLINK_COLOR);

  pixels.show();
}

void renderClockDayMonth(int dispDay, int dispMonth, int dispWeekday, bool blinkDay=false, bool blinkDayOn=true, bool blinkMonth=false, bool blinkMonthOn=true) {
  pixels.clear();
  drawDigit(dispDay / 10, HOURS_TENS_START, DIGIT_COLOR, blinkDay, blinkDayOn);
  drawDigit(dispDay % 10, HOURS_UNITS_START, DIGIT_COLOR, blinkDay, blinkDayOn);
  drawDigit(dispMonth / 10, MINUTES_TENS_START, DIGIT_COLOR, blinkMonth, blinkMonthOn);
  drawDigit(dispMonth % 10, MINUTES_UNITS_START, DIGIT_COLOR, blinkMonth, blinkMonthOn);
  int ledWeekday = (dispWeekday + 6) % 7;
  if (ledWeekday >= 0 && ledWeekday < NUM_WEEKDAYS)
    pixels.setPixelColor(WEEKDAY_START + ledWeekday, WEEKDAY_COLOR);
  pixels.setPixelColor(SEPARATOR_START, BLINK_COLOR);
  pixels.setPixelColor(SEPARATOR_START + 1, BLINK_COLOR);
  pixels.setPixelColor(SECONDS_LED, BLINK_COLOR);
  pixels.show();
}

void renderClockYear(int dispYear, bool blinkYear=false, bool blinkYearOn=true) {
  pixels.clear();
  drawDigit((dispYear / 1000) % 10, HOURS_TENS_START, DIGIT_COLOR, blinkYear, blinkYearOn);
  drawDigit((dispYear / 100) % 10, HOURS_UNITS_START, DIGIT_COLOR, blinkYear, blinkYearOn);
  drawDigit((dispYear / 10) % 10, MINUTES_TENS_START, DIGIT_COLOR, blinkYear, blinkYearOn);
  drawDigit(dispYear % 10, MINUTES_UNITS_START, DIGIT_COLOR, blinkYear, blinkYearOn);
  pixels.setPixelColor(SEPARATOR_START, BLINK_COLOR);
  pixels.setPixelColor(SEPARATOR_START + 1, BLINK_COLOR);
  pixels.setPixelColor(SECONDS_LED, BLINK_COLOR);
  pixels.show();
}

void renderClockTemperature(int temp) {
  pixels.clear();
  drawDigit((abs(temp) / 10) % 10, HOURS_UNITS_START, TEMP_COLOR);
  drawDigit(abs(temp) % 10, MINUTES_TENS_START, TEMP_COLOR);
  pixels.setPixelColor(SEPARATOR_START, BLINK_COLOR);
  pixels.setPixelColor(SEPARATOR_START + 1, BLINK_COLOR);
  pixels.setPixelColor(SECONDS_LED, TEMP_COLOR);
  pixels.show();
}

// --- ALARM RENDERING ---
void renderAlarmSelection(int alarmIdx, bool enabled) {
  pixels.clear();
  // AL on hours digits, n on minute units
  drawDigit(1, HOURS_TENS_START, DIGIT_COLOR); // fake "A"
  drawDigit(1, HOURS_UNITS_START, DIGIT_COLOR); // fake "L"
  drawDigit(alarmIdx, MINUTES_UNITS_START, DIGIT_COLOR);
  pixels.setPixelColor(ALARM_LED, enabled ? ALARM_COLOR : BLINK_COLOR);
  pixels.show();
}

void renderAlarmTimeEdit(int alarmIdx, int step, bool blink) {
  Alarm &a = alarms[alarmIdx];
  pixels.clear();
  if (step == 0) { // hour
    drawDigit(a.hour / 10, HOURS_TENS_START, DIGIT_COLOR, true, blink);
    drawDigit(a.hour % 10, HOURS_UNITS_START, DIGIT_COLOR, true, blink);
    drawDigit(a.minute / 10, MINUTES_TENS_START, DIGIT_COLOR, false, true);
    drawDigit(a.minute % 10, MINUTES_UNITS_START, DIGIT_COLOR, false, true);
  } else if (step == 1) { // minute
    drawDigit(a.hour / 10, HOURS_TENS_START, DIGIT_COLOR, false, true);
    drawDigit(a.hour % 10, HOURS_UNITS_START, DIGIT_COLOR, false, true);
    drawDigit(a.minute / 10, MINUTES_TENS_START, DIGIT_COLOR, true, blink);
    drawDigit(a.minute % 10, MINUTES_UNITS_START, DIGIT_COLOR, true, blink);
  } else if (step == 2) { // enable
    // Blank digits, blink ALARM_LED
    drawDigit(10, HOURS_TENS_START, BLINK_COLOR);
    drawDigit(10, HOURS_UNITS_START, BLINK_COLOR);
    drawDigit(10, MINUTES_TENS_START, BLINK_COLOR);
    drawDigit(10, MINUTES_UNITS_START, BLINK_COLOR);
  }
  pixels.setPixelColor(ALARM_LED, (step == 2 && blink) ? ALARM_COLOR : (a.enabled ? ALARM_COLOR : BLINK_COLOR));
  pixels.show();
}

// --- SETTINGS MODE ---
void enterTimeSettingMode() {
  currentMode = SET_TIME;
  settingStep = SET_HOUR;
  int sysHour = hour % 12;
  if (sysHour == 0) sysHour = 12;
  setHour = sysHour;
  setMinute = minute;
  setDay = day;
  setMonth = month;
  setYear = year;
  setIsPM = (hour >= 12);
  blinkState = false;
  lastBlink = millis();
  btnHoldStart[0] = btnHoldStart[1] = 0;
  lastRepeat[0] = lastRepeat[1] = 0;
  ampmToggleLatch = false;
}

// --- TIME SETTING HANDLER ---
void handleTimeSetting() {
  if (millis() - lastBlink > BLINK_INTERVAL) { blinkState = !blinkState; lastBlink = millis(); }
  readButtons();

  DateTime previewDate(setYear, setMonth, setDay, (setIsPM ? (setHour % 12) + 12 : (setHour % 12)), setMinute, 0);
  int previewWeekday = previewDate.dayOfTheWeek();

  if (settingStep == SET_HOUR) {
    pixels.clear();

    if (setHour >= 10) {
      drawDigit(setHour / 10, HOURS_TENS_START, DIGIT_COLOR, true, blinkState);
    }
    drawDigit(setHour % 10, HOURS_UNITS_START, DIGIT_COLOR, true, blinkState);

    drawDigit(setMinute / 10, MINUTES_TENS_START, DIGIT_COLOR, false, true);
    drawDigit(setMinute % 10, MINUTES_UNITS_START, DIGIT_COLOR, false, true);
    int ledWeekday = (previewWeekday + 6) % 7;
    if (ledWeekday >= 0 && ledWeekday < NUM_WEEKDAYS)
      pixels.setPixelColor(WEEKDAY_START + ledWeekday, WEEKDAY_COLOR);

    static bool sepOn = true;
    static unsigned long lastSepBlink = 0;
    if (millis() - lastSepBlink >= 500) {
      sepOn = !sepOn; lastSepBlink = millis();
    }
    pixels.setPixelColor(SEPARATOR_START, sepOn ? SEPARATOR_COLOR : BLINK_COLOR);
    pixels.setPixelColor(SEPARATOR_START + 1, sepOn ? SEPARATOR_COLOR : BLINK_COLOR);
    pixels.setPixelColor(SECONDS_LED, sepOn ? SECONDS_LED_COLOR : BLINK_COLOR);

    pixels.setPixelColor(AM_PM_LED, setIsPM ? AM_PM_COLOR : BLINK_COLOR);
    pixels.show();

    if (btnPressed(BTN_INC_IDX)) setHour = (setHour % 12) + 1;
    if (btnPressed(BTN_DEC_IDX)) setHour = (setHour > 1) ? setHour - 1 : 12;
    if (btnPressed(BTN_OK_IDX)) settingStep = SET_MINUTE;
    if (btnState[BTN_INC_IDX] && btnState[BTN_DEC_IDX]) {
      if (!ampmToggleLatch) {
        setIsPM = !setIsPM;
        beep(2500, 60);
        ampmToggleLatch = true;
      }
    } else {
      ampmToggleLatch = false;
    }
    handleContinuousButtons();
    return;
  }

  if (settingStep == SET_MINUTE) {
    pixels.clear();
    if (setHour >= 10) {
      drawDigit(setHour / 10, HOURS_TENS_START, DIGIT_COLOR, false, true);
    }
    drawDigit(setHour % 10, HOURS_UNITS_START, DIGIT_COLOR, false, true);
    drawDigit(setMinute / 10, MINUTES_TENS_START, DIGIT_COLOR, true, blinkState);
    drawDigit(setMinute % 10, MINUTES_UNITS_START, DIGIT_COLOR, true, blinkState);
    int ledWeekday = (previewWeekday + 6) % 7;
    if (ledWeekday >= 0 && ledWeekday < NUM_WEEKDAYS)
      pixels.setPixelColor(WEEKDAY_START + ledWeekday, WEEKDAY_COLOR);
    static bool sepOn = true;
    static unsigned long lastSepBlink = 0;
    if (millis() - lastSepBlink >= 500) {
      sepOn = !sepOn; lastSepBlink = millis();
    }
    pixels.setPixelColor(SEPARATOR_START, sepOn ? SEPARATOR_COLOR : BLINK_COLOR);
    pixels.setPixelColor(SEPARATOR_START + 1, sepOn ? SEPARATOR_COLOR : BLINK_COLOR);
    pixels.setPixelColor(SECONDS_LED, sepOn ? SECONDS_LED_COLOR : BLINK_COLOR);
    pixels.setPixelColor(AM_PM_LED, setIsPM ? AM_PM_COLOR : BLINK_COLOR);
    pixels.show();

    if (btnPressed(BTN_INC_IDX)) setMinute = (setMinute + 1) % 60;
    if (btnPressed(BTN_DEC_IDX)) setMinute = (setMinute > 0) ? setMinute - 1 : 59;
    if (btnPressed(BTN_OK_IDX)) settingStep = SET_DAY;
    handleContinuousButtons();
    return;
  }
  if (settingStep == SET_DAY) {
    renderClockDayMonth(setDay, setMonth, previewWeekday, true, blinkState, false, true);
    pixels.setPixelColor(AM_PM_LED, setIsPM ? AM_PM_COLOR : BLINK_COLOR);
    if (btnPressed(BTN_INC_IDX)) setDay = (setDay < 31) ? setDay + 1 : 1;
    if (btnPressed(BTN_DEC_IDX)) setDay = (setDay > 1) ? setDay - 1 : 31;
    if (btnPressed(BTN_OK_IDX)) settingStep = SET_MONTH;
    handleContinuousButtons();
    return;
  }
  if (settingStep == SET_MONTH) {
    renderClockDayMonth(setDay, setMonth, previewWeekday, false, true, true, blinkState);
    pixels.setPixelColor(AM_PM_LED, setIsPM ? AM_PM_COLOR : BLINK_COLOR);
    if (btnPressed(BTN_INC_IDX)) setMonth = (setMonth < 12) ? setMonth + 1 : 1;
    if (btnPressed(BTN_DEC_IDX)) setMonth = (setMonth > 1) ? setMonth - 1 : 12;
    if (btnPressed(BTN_OK_IDX)) settingStep = SET_YEAR;
    handleContinuousButtons();
    return;
  }
  if (settingStep == SET_YEAR) {
    renderClockYear(setYear, true, blinkState);
    pixels.setPixelColor(AM_PM_LED, setIsPM ? AM_PM_COLOR : BLINK_COLOR);
    if (btnPressed(BTN_INC_IDX)) setYear = (setYear < 2099) ? setYear + 1 : 2000;
    if (btnPressed(BTN_DEC_IDX)) setYear = (setYear > 2000) ? setYear - 1 : 2099;
    if (btnPressed(BTN_OK_IDX)) settingStep = SET_DONE;
    handleContinuousButtons();
    return;
  }
  if (settingStep == SET_DONE) {
    int newHour = setHour % 12;
    if (setIsPM) newHour += 12;
    rtc.adjust(DateTime(setYear, setMonth, setDay, newHour, setMinute, 0));
    beep(2000,60); beep(2000,60);
    currentMode = NORMAL; settingStep = SET_HOUR;
    return;
  }
}

// --- ALARM MODE HANDLERS ---
void handleAlarmSelection() {
  renderAlarmSelection(selectedAlarm, alarms[selectedAlarm].enabled);
  if (btnPressed(BTN_INC_IDX)) {
    selectedAlarm = (selectedAlarm + 1) % ALARM_MAX;
  }
  if (btnPressed(BTN_DEC_IDX)) {
    selectedAlarm = (selectedAlarm + ALARM_MAX - 1) % ALARM_MAX;
  }
  if (btnPressed(BTN_OK_IDX)) {
    alarmSettingStep = 0;
    blinkState = false;
    lastBlink = millis();
    currentMode = SET_ALARM_TIME;
  }
}

void handleAlarmTimeEdit() {
  if (millis() - lastBlink > BLINK_INTERVAL) { blinkState = !blinkState; lastBlink = millis(); }
  renderAlarmTimeEdit(selectedAlarm, alarmSettingStep, blinkState);
  Alarm &a = alarms[selectedAlarm];

  if (alarmSettingStep == 0) {
    if (btnPressed(BTN_INC_IDX)) a.hour = (a.hour + 1) % 24;
    if (btnPressed(BTN_DEC_IDX)) a.hour = (a.hour + 23) % 24;
    if (btnPressed(BTN_OK_IDX)) alarmSettingStep = 1;
  } else if (alarmSettingStep == 1) {
    if (btnPressed(BTN_INC_IDX)) a.minute = (a.minute + 1) % 60;
    if (btnPressed(BTN_DEC_IDX)) a.minute = (a.minute + 59) % 60;
    if (btnPressed(BTN_OK_IDX)) alarmSettingStep = 2;
  } else if (alarmSettingStep == 2) {
    if (btnPressed(BTN_INC_IDX) || btnPressed(BTN_DEC_IDX)) a.enabled = !a.enabled;
    if (btnPressed(BTN_OK_IDX)) currentMode = SET_ALARM;
  }
  handleContinuousButtons();
}

// --- ALARM CHECKING ---
void checkAlarms() {
  static int lastCheckedMinute = -1;
  if (minute != lastCheckedMinute) {
    lastCheckedMinute = minute;
    for (int i = 0; i < ALARM_MAX; i++) {
      if (alarms[i].enabled && alarms[i].hour == hour && alarms[i].minute == minute) {
        for (int j = 0; j < 5; j++) { beep(2500, 90); delay(50); }
      }
    }
  }
}

// --- SETUP & LOOP ---
void setup() {
  Serial.begin(115200);
  pinMode(BUZZER_PIN, OUTPUT);
  pixels.begin(); pixels.setBrightness(255); pixels.show();
  pinMode(BTN_INC, INPUT); pinMode(BTN_DEC, INPUT); pinMode(BTN_OK, INPUT); pinMode(BTN_CFG, INPUT_PULLUP);
  Wire.begin(DS3231_SDA, DS3231_SCL);
  if (!rtc.begin()) { while (1) { beep(500, 100); delay(500); } }
  if (rtc.lostPower()) rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  updateTimeFromRTC(); updateTemperatureFromRTC();
  // Load alarms from EEPROM
  loadAlarmsFromEEPROM();
}

void loop() {
  static unsigned long lastMillis = 0;
  static int cycleState = 0; // 0: clock, 1: day/month, 2: year, 3: temp

  readButtons();

  // --- OK button long press for ALARM MODE ---
  static unsigned long okBtnPressTime = 0;
  if (btnState[BTN_OK_IDX]) {
    if (okBtnPressTime == 0) okBtnPressTime = millis();
    if ((millis() - okBtnPressTime > 2000)) {
      if (currentMode == NORMAL) {
        currentMode = SET_ALARM;
        selectedAlarm = 0;
        alarmSettingStep = 0;
        blinkState = false;
        lastBlink = millis();
        beep(2200, 100);
        delay(200);
      } else if (currentMode == SET_ALARM || currentMode == SET_ALARM_TIME) {
        saveAlarmsToEEPROM();
        currentMode = NORMAL;
        pixels.setPixelColor(ALARM_LED, ALARM_SET_COLOR); pixels.show();
        beep(2500, 150);
        delay(600);
        pixels.setPixelColor(ALARM_LED, BLINK_COLOR); pixels.show();
      }
    }
  } else {
    okBtnPressTime = 0;
  }

  unsigned long nowMillis = millis();
  if (nowMillis - lastMillis >= 1000) {
    lastMillis = nowMillis;
    updateTimeFromRTC();
    updateTemperatureFromRTC();
    checkAlarms();
  }

  // --- Modes ---
  if (currentMode == SET_TIME) {
    handleTimeSetting();
    return;
  }
  if (currentMode == SET_ALARM) {
    handleAlarmSelection();
    return;
  }
  if (currentMode == SET_ALARM_TIME) {
    handleAlarmTimeEdit();
    return;
  }

  // --- Enter time setting mode ---
  if (btnPressed(BTN_CFG_IDX)) {
    enterTimeSettingMode();
    return;
  }

  // --- Normal cycling display ---
  static unsigned long modeStartTime = 0;
  if (cycleState == 0 && nowMillis - modeStartTime > 10000) {
    cycleState = 1; modeStartTime = nowMillis;
  } else if (cycleState == 1 && nowMillis - modeStartTime > 1500) {
    cycleState = 2; modeStartTime = nowMillis;
  } else if (cycleState == 2 && nowMillis - modeStartTime > 1500) {
    cycleState = 3; modeStartTime = nowMillis;
  } else if (cycleState == 3 && nowMillis - modeStartTime > 1500) {
    cycleState = 0; modeStartTime = nowMillis;
  }

  int dispHour = hour % 12; if (dispHour == 0) dispHour = 12;

  if (cycleState == 0) {
    renderClockTime(dispHour, minute, weekday, second, false, true, false, true);
  } else if (cycleState == 1) {
    renderClockDayMonth(day, month, weekday, false, true, false, true);
  } else if (cycleState == 2) {
    renderClockYear(year, false, true);
  } else if (cycleState == 3) {
    int tempShow = (int)(rtcTemperature + 0.5);
    renderClockTemperature(tempShow);
  }

  delay(10);
}