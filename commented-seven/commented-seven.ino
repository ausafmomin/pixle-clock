#include <Wire.h>                    // Include library for I2C communication (used by RTC)
#include <Adafruit_NeoPixel.h>       // Include library for controlling NeoPixel LEDs
#include <RTClib.h>                  // Include library for DS3231 Real Time Clock
#include <EEPROM.h>  
#include <JQ6500_Serial.h>
#include <HardwareSerial.h>                


// --- ENUMS ---
// Enum for steps in time setting procedure
enum SettingStep { SET_HOUR = 0, SET_MINUTE, SET_DAY, SET_MONTH, SET_YEAR, SET_DONE };
// Enum for system modes
enum Mode { NORMAL, SET_TIME, SET_ALARM, SET_ALARM_TIME };
#define JQ6500_RX 16  // ESP32 RX pin number (connects to JQ6500 TX)
#define JQ6500_TX 17  // ESP32 TX pin number (connects to JQ6500 RX)


// --- PINS & LAYOUT ---
// Pin number for NeoPixel LED chain
#define LED_PIN     4
// Pin number for buzzer output
#define BUZZER_PIN  12
// Increment button pin
#define BTN_INC     26
// Decrement button pin
#define BTN_DEC     27
// OK/confirm button pin
#define BTN_OK      14
// Configuration (settings) button pin
#define BTN_CFG     15
// I2C SDA pin for RTC
#define DS3231_SDA  21
// I2C SCL pin for RTC
#define DS3231_SCL  22

// Number of LEDs in the strip
#define NUM_LEDS         68
// Number of LEDs used to display one digit
#define LEDS_PER_DIGIT   14
// Number of LEDs for weekday indicators
#define NUM_WEEKDAYS     7
// Number of LEDs for separator (colon)
#define NUM_SEPARATOR_LEDS 2
// LED indices for each digit and indicator
#define HOURS_TENS_START      0
#define HOURS_UNITS_START     (HOURS_TENS_START + LEDS_PER_DIGIT)
#define SEPARATOR_START       (HOURS_UNITS_START + LEDS_PER_DIGIT)
#define MINUTES_TENS_START    (SEPARATOR_START + NUM_SEPARATOR_LEDS)
#define MINUTES_UNITS_START   (MINUTES_TENS_START + LEDS_PER_DIGIT)
#define WEEKDAY_START         (MINUTES_UNITS_START + LEDS_PER_DIGIT)
#define AM_PM_LED             (WEEKDAY_START + NUM_WEEKDAYS)
#define ALARM_LED             (AM_PM_LED + 1)
#define SECONDS_LED           (ALARM_LED + 1)


HardwareSerial mp3Serial(2); 
JQ6500_Serial mp3(mp3Serial);         // Then use it for the JQ6500_Serial object

bool alarmAudioPlaying = false;
int alarmAudioMinute = -1; // Keeps track of the minute when audio was played

// --- SEGMENT PATTERNS ---
// Patterns for 7 segments for digits 0-9, each bit is a segment
const bool segmentPatterns[12][7] = {
  {1,1,1,0,1,1,1}, // 0
  {0,0,1,0,0,0,1}, // 1
  {0,1,1,1,1,1,0}, // 2
  {0,1,1,1,0,1,1}, // 3
  {1,0,1,1,0,0,1}, // 4
  {1,1,0,1,0,1,1}, // 5
  {1,1,0,1,1,1,1}, // 6
  {0,1,1,0,0,0,1}, // 7
  {1,1,1,1,1,1,1}, // 8
  {1,1,1,1,0,1,1}, // 9
  //{1,1,1,1,1,1,1}, // A
  //{1,0,0,1,1,1,0}  // L
// Add these patterns after your digit patterns
//const bool segmentPatternA[7] = {1,1,1,1,1,1,1}; // All segments except D (for "A")
//const bool segmentPatternL[7] = {1,0,0,1,1,1,0}; // Segments E, F, D (for "L")
};
// Add these patterns after your digit patterns
const bool segmentPatternA[7] = {1,1,1,1,1,0,1}; // A
const bool segmentPatternL[7] = {1,0,0,0,1,1,0}; // L
const bool segmentPatternDEGREE[7] = {1,1,1,1,0,0,0}; // Segments A, B, F, G
const bool segmentPatternC[7] = {1,1,0,0,1,1,0}; // C

// Each segment maps to 2 LEDs for each digit
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
// Color constants for various parts of the display

Adafruit_NeoPixel pixels(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);

#define DIGIT_COLOR         pixels.Color(0, 255, 0)
#define BLINK_COLOR         pixels.Color(0, 0, 0)
#define SEPARATOR_COLOR     pixels.Color(255, 165, 0)
#define WEEKDAY_COLOR       pixels.Color(0, 0, 255)
#define AM_PM_COLOR         pixels.Color(255, 0, 255)
#define ALARM_COLOR         pixels.Color(255, 0, 0)
#define TEMP_COLOR          pixels.Color(0, 255, 255)
#define ALARM_SET_COLOR     pixels.Color(255, 255, 0)
#define SECONDS_LED_COLOR   pixels.Color(255, 255, 255)

// --- CLOCK COLOR PALETTE ---
uint32_t clockColors[] = {
  pixels.Color(0, 255, 0),     // Green
  pixels.Color(255, 0, 0),     // Red
  pixels.Color(0, 0, 255),     // Blue
  pixels.Color(255, 255, 0),   // Yellow
  pixels.Color(255, 0, 255),   // Magenta
  pixels.Color(0, 255, 255),   // Cyan
  pixels.Color(255, 255, 255)  // White
};
const int NUM_CLOCK_COLORS = sizeof(clockColors)/sizeof(clockColors[0]);
int clockColorIdx = 0;
// --- ALARMS & EEPROM ---

#define EEPROM_CLOCK_COLOR_IDX 64
// Maximum number of alarms
#define ALARM_MAX 5
// EEPROM address where alarms start
#define EEPROM_ALARM_START 0
// Alarm struct for storing alarm time and enabled status
struct Alarm { int hour; int minute; bool enabled; };
Alarm alarms[ALARM_MAX];   // Array to hold alarm settings
int alarmCount = ALARM_MAX; // Always ALARM_MAX for consistency (not currently dynamic)

// Save alarms to EEPROM for persistence across resets
void saveAlarmsToEEPROM() {
  EEPROM.begin(64);                       // Prepare EEPROM
  EEPROM.put(EEPROM_ALARM_START, alarms); // Write alarm array at start position
  EEPROM.commit();                        // Commit changes
}

// Load alarms from EEPROM, validate, and initialize if invalid data found
void loadAlarmsFromEEPROM() {
  EEPROM.begin(64);
  EEPROM.get(EEPROM_ALARM_START, alarms);
  EEPROM.end();
  // Validate alarms: if invalid, reset to default value
  for (int i = 0; i < ALARM_MAX; i++) {
    if (alarms[i].hour < 0 || alarms[i].hour > 23 ||
        alarms[i].minute < 0 || alarms[i].minute > 59)
      alarms[i] = {0, 0, false};
  }
}

void saveClockColorIdxToEEPROM() {
  EEPROM.begin(64);
  EEPROM.write(EEPROM_CLOCK_COLOR_IDX, clockColorIdx);
  EEPROM.commit();
}
void loadClockColorIdxFromEEPROM() {
  EEPROM.begin(64);
  clockColorIdx = EEPROM.read(EEPROM_CLOCK_COLOR_IDX);
  EEPROM.end();
  if (clockColorIdx < 0 || clockColorIdx >= NUM_CLOCK_COLORS) clockColorIdx = 0;
}

// The DS3231 RTC object
RTC_DS3231 rtc;
// Timing variables for loop timing and blinking
unsigned long lastMillis = 0;
// Current time/date values from RTC
int hour, minute, second, weekday, day, month, year;
// Current temperature from RTC
float rtcTemperature = 0;

// Mode and state for the UI and settings
Mode currentMode = NORMAL;             // Current operating mode
SettingStep settingStep;               // Step for time setting
int setHour, setMinute, setDay, setMonth, setYear; // Temporary values during setting
bool setIsPM = false;                  // PM flag for time setting
bool blinkState = false;               // Blink state for UI
unsigned long lastBlink = 0;           // Timing for blink toggling
const unsigned long BLINK_INTERVAL = 500; // Blink interval in ms
unsigned long btnHoldStart[2] = {0, 0};   // Button hold timers for continuous inc/dec
unsigned long lastRepeat[2] = {0, 0};     // Last repeated inc/dec event
const unsigned long initialHoldDelay = 400; // Delay before repeat starts
const unsigned long repeatInterval = 70;    // Interval for repeat
bool ampmToggleLatch = false;              // Prevents multiple toggling of AM/PM



// Alarm mode variables
int selectedAlarm = 0;       // Index of alarm being edited
int alarmSettingStep = 0;    // Which parameter of alarm is being set (hour/minute/enabled)

// --- BUTTONS ---
// Debounce and previous state tracking for buttons
unsigned long lastDebounce[4] = {0, 0, 0, 0};
bool btnState[4] = {false, false, false, false};
bool lastBtnState[4] = {false, false, false, false};
const unsigned long debounceDelay = 20;      // ms debounce
// Button index enum for referencing button arrays
enum BtnIdx { BTN_INC_IDX = 0, BTN_DEC_IDX = 1, BTN_OK_IDX = 2, BTN_CFG_IDX = 3 };

// --- BUZZER ---
// Produce a beep for feedback on the buzzer
void beep(uint16_t freq, uint16_t ms) {
  tone(BUZZER_PIN, freq, ms);   // Start tone
  delay(ms);                    // Wait duration
  noTone(BUZZER_PIN);           // Stop tone
}

// --- BUTTON HANDLING ---
// Read and debounce all 4 buttons, update state arrays


void readButtons() {
  int pins[4] = {BTN_INC, BTN_DEC, BTN_OK, BTN_CFG};
  for (int i = 0; i < 4; i++) {
    // CFG button is active LOW, others are active HIGH
    bool reading = (i == BTN_CFG_IDX) ? (digitalRead(pins[i]) == LOW) : (digitalRead(pins[i]) == HIGH);
    if (reading != lastBtnState[i]) lastDebounce[i] = millis();
    if ((millis() - lastDebounce[i]) > debounceDelay) btnState[i] = reading;
    lastBtnState[i] = reading;
  }
  if (currentMode == NORMAL) {
    if (btnPressed(BTN_INC_IDX)) {
      clockColorIdx = (clockColorIdx + 1) % NUM_CLOCK_COLORS;
      saveClockColorIdxToEEPROM();
      beep(2000, 40);
    }
    if (btnPressed(BTN_DEC_IDX)) {
      clockColorIdx = (clockColorIdx - 1 + NUM_CLOCK_COLORS) % NUM_CLOCK_COLORS;
      saveClockColorIdxToEEPROM();
      beep(1500, 40);
    }
  }
}
// Detect a momentary button press (rising edge)
bool btnPressed(int idx) {
  static bool prev[4] = {false, false, false, false};
  bool pressed = btnState[idx] && !prev[idx];
  prev[idx] = btnState[idx];
  if (pressed) {
    beep(1800 + idx*300, 40 + idx*10);  // Provide button specific feedback
    delay(40);
  }
  return pressed;
}

// --- CONTINUOUS INC/DEC ---
// Support for holding INC/DEC buttons to auto-repeat (for faster settings)
void handleContinuousButtons() {
  unsigned long now = millis();
  // INC button held
  if (btnState[BTN_INC_IDX]) {
    if (btnHoldStart[0] == 0) {
      btnHoldStart[0] = now;
      lastRepeat[0] = now;
    } else if (now - btnHoldStart[0] > initialHoldDelay && now - lastRepeat[0] > repeatInterval) {
      // If in time setting mode, increment relevant value
      if (currentMode == SET_TIME) {
        switch (settingStep) {
          case SET_HOUR: setHour = (setHour % 12) + 1; if (setHour == 0) setHour = 1; break;
          case SET_MINUTE: setMinute = (setMinute + 1) % 60; break;
          case SET_DAY: setDay = (setDay < 31) ? setDay + 1 : 1; break;
          case SET_MONTH: setMonth = (setMonth < 12) ? setMonth + 1 : 1; break;
          case SET_YEAR: setYear = (setYear < 2099) ? setYear + 1 : 2000; break;
          default: break;
        }
        beep(2100, 20); // Feedback beep
      }
      // If in alarm setting mode, increment alarm values
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
  // DEC button held
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
// Update global time variables with the current RTC time
void updateTimeFromRTC() {
  DateTime now = rtc.now();          // Get current time from RTC
  hour = now.hour(); minute = now.minute(); second = now.second();
  day = now.day(); month = now.month(); year = now.year(); weekday = now.dayOfTheWeek();
}
// Update the RTC temperature reading
void updateTemperatureFromRTC() { rtcTemperature = rtc.getTemperature(); }

// --- RENDER DIGIT ---
// Draw a digit at a given LED index (optionally blink)
void drawDigit(int digit, int start, uint32_t color, bool blink=false, bool blinkOn=true) {
  if (digit < 0 || digit > 9) {
    // Invalid digit: blank all segments
    for (int seg=0; seg<7; seg++) {
      pixels.setPixelColor(start + segmentToLedMapping[seg][0], BLINK_COLOR);
      pixels.setPixelColor(start + segmentToLedMapping[seg][1], BLINK_COLOR);
    }
    return;
  }
  if (blink && !blinkOn) {
    // Blank for blink-off state
    for (int seg=0; seg<7; seg++) {
      pixels.setPixelColor(start + segmentToLedMapping[seg][0], BLINK_COLOR);
      pixels.setPixelColor(start + segmentToLedMapping[seg][1], BLINK_COLOR);
    }
    return;
  }
  // Draw each segment that should be lit for this digit
  for (int seg = 0; seg < 7; seg++) {
    if (segmentPatterns[digit][seg]) {
      pixels.setPixelColor(start + segmentToLedMapping[seg][0], color);
      pixels.setPixelColor(start + segmentToLedMapping[seg][1], color);
    }
  }
}

// --- RENDER FUNCTIONS ---
// Render the main time (hours, minutes, weekday, indicators)
void renderClockTime(int dispHour, int dispMinute, int dispWeekday, int dispSecond, bool blinkH=false, bool blinkHOn=true, bool blinkM=false, bool blinkMOn=true) {
  pixels.clear();

  // Draw hours tens digit if not zero (for 12-hour format)
  if (dispHour >= 10) {
    drawDigit(dispHour / 10, HOURS_TENS_START, clockColors[clockColorIdx], blinkH, blinkHOn);
  }
  drawDigit(dispHour % 10, HOURS_UNITS_START, clockColors[clockColorIdx], blinkH, blinkHOn);

  drawDigit(dispMinute / 10, MINUTES_TENS_START, clockColors[clockColorIdx], blinkM, blinkMOn);
  drawDigit(dispMinute % 10, MINUTES_UNITS_START, clockColors[clockColorIdx], blinkM, blinkMOn);

  // Show weekday as a lit dot
  int ledWeekday = (dispWeekday + 6) % 7;
  if (ledWeekday >= 0 && ledWeekday < NUM_WEEKDAYS)
    pixels.setPixelColor(WEEKDAY_START + ledWeekday, WEEKDAY_COLOR);

  // Show AM/PM indicator if PM
  if (hour >= 12) pixels.setPixelColor(AM_PM_LED, AM_PM_COLOR);

  // Show alarm LED if any alarm enabled
  bool anyAlarm = false;
  for (int i=0; i<ALARM_MAX; i++) if (alarms[i].enabled) anyAlarm = true;
  pixels.setPixelColor(ALARM_LED, anyAlarm ? ALARM_COLOR : BLINK_COLOR);

  // Blinking colon separator and seconds LED
  static bool sepOn = true;
  static unsigned long lastSepBlink = 0;
  if (millis() - lastSepBlink >= 1000) {
    sepOn = !sepOn;
    lastSepBlink = millis();
  }
  pixels.setPixelColor(SEPARATOR_START, sepOn ? clockColors[clockColorIdx] : BLINK_COLOR);
  pixels.setPixelColor(SEPARATOR_START + 1, sepOn ? clockColors[clockColorIdx] : BLINK_COLOR);
  pixels.setPixelColor(SECONDS_LED, sepOn ? clockColors[clockColorIdx] : BLINK_COLOR);

  pixels.show();
}

// Render day/month
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

// Render year
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

// Render temperature on display (as two digits)
void drawLetter(const bool pattern[7], int start, uint32_t color) {
  for (int seg = 0; seg < 7; seg++) {
    if (pattern[seg]) {
      pixels.setPixelColor(start + segmentToLedMapping[seg][0], color);
      pixels.setPixelColor(start + segmentToLedMapping[seg][1], color);
    } else {
      pixels.setPixelColor(start + segmentToLedMapping[seg][0], BLINK_COLOR);
      pixels.setPixelColor(start + segmentToLedMapping[seg][1], BLINK_COLOR);
    }
  }
}

void renderClockTemperature(int temp) {
  pixels.clear();
  // Draw temp tens digit
  drawDigit((abs(temp) / 10) % 10, HOURS_TENS_START, TEMP_COLOR);
  // Draw temp units digit
  drawDigit(abs(temp) % 10, HOURS_UNITS_START, TEMP_COLOR);
  // Draw degree symbol
  drawLetter(segmentPatternDEGREE, MINUTES_TENS_START, TEMP_COLOR);
  // Draw 'C'
  drawLetter(segmentPatternC, MINUTES_UNITS_START, TEMP_COLOR);
  // Optionally set other indicators
  pixels.setPixelColor(SEPARATOR_START, BLINK_COLOR);
  pixels.setPixelColor(SEPARATOR_START + 1, BLINK_COLOR);
  pixels.setPixelColor(SECONDS_LED, TEMP_COLOR);
  pixels.show();
}


  // Fake "A" and "L" using digit 1
 
  void renderAlarmSelection(int alarmIdx, bool enabled) {
  pixels.clear();
  drawLetter(segmentPatternA, HOURS_TENS_START, DIGIT_COLOR);       // Draw "A"
  drawLetter(segmentPatternL, HOURS_UNITS_START, DIGIT_COLOR);      // Draw "L"
  drawDigit(alarmIdx, MINUTES_UNITS_START, DIGIT_COLOR);            // Draw alarm number
  pixels.setPixelColor(ALARM_LED, enabled ? ALARM_COLOR : BLINK_COLOR);
  pixels.show();
}



// Render alarm time edit (hour/minute/enable blink)
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
// Enter time setting mode, initialize temporary setting values
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
// Main handler for time setting, step by step
void handleTimeSetting() {
  if (millis() - lastBlink > BLINK_INTERVAL) { blinkState = !blinkState; lastBlink = millis(); }
  readButtons();

  // Preview weekday as date is changed
  DateTime previewDate(setYear, setMonth, setDay, (setIsPM ? (setHour % 12) + 12 : (setHour % 12)), setMinute, 0);
  int previewWeekday = previewDate.dayOfTheWeek();

  // Each setting step (hour/minute/day/month/year)
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
    // Both INC and DEC to toggle AM/PM
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

  // Repeat for other steps (minute, day, month, year, and done)
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
// Handle alarm selection mode (choose which alarm to edit)
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

// Handle editing the time and enabled state of the selected alarm
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
// Check if any enabled alarm matches the current time and sound the buzzer
void checkAlarms() {
  // Only trigger once per minute
  if (minute != alarmAudioMinute) {
    for (int i = 0; i < ALARM_MAX; i++) {
      if (alarms[i].enabled && alarms[i].hour == hour && alarms[i].minute == minute) {
        mp3.playFileByIndexNumber(6); // Play audio once
        alarmAudioPlaying = true;
        alarmAudioMinute = minute;
        break; // Only trigger one alarm
      }
    }
  }
}


// --- SETUP & LOOP ---
// Arduino setup: initialize hardware, RTC, and load alarms
void setup() {
  Serial.begin(115200);
  pinMode(BUZZER_PIN, OUTPUT);
  pixels.begin(); pixels.setBrightness(255); pixels.show();
  pinMode(BTN_INC, INPUT); pinMode(BTN_DEC, INPUT); pinMode(BTN_OK, INPUT); pinMode(BTN_CFG, INPUT_PULLUP);
  Wire.begin(DS3231_SDA, DS3231_SCL);

  // --- JQ6500 INITIALIZATION ---
  mp3Serial.begin(9600, SERIAL_8N1, JQ6500_RX, JQ6500_TX); // UART2
  delay(600); // Let the module power up

  // --- PLAY TRACK 001.mp3 AT BOOT ---
  mp3.setVolume(30); // Set volume to maximum
  mp3.playFileByIndexNumber(7); // Play seventh audio (007.mp3) masjid  e akram

  if (!rtc.begin()) { while (1) { beep(500, 100); delay(500); } }
  if (rtc.lostPower()) rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  updateTimeFromRTC(); updateTemperatureFromRTC();
  loadAlarmsFromEEPROM();
  loadClockColorIdxFromEEPROM();
}

// Arduino main loop: handle UI, modes, display, alarms
void loop() {
  static unsigned long lastMillis = 0;
  static int cycleState = 0; // 0: clock, 1: day/month, 2: year, 3: temp

  readButtons();

  // --- OK button long press for ALARM MODE ---
  static unsigned long okBtnPressTime = 0;
  static bool okLongPressArmed = true;

  if (btnState[BTN_OK_IDX]) {
    if (okLongPressArmed) {
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
        okLongPressArmed = false; // Prevent repeated toggling until released
      }
    }
  } 
  else {
    okBtnPressTime = 0;
    okLongPressArmed = true; // Re-arm after button is released
  }
   checkAlarms();

  if (alarmAudioPlaying) {
    if (mp3.getStatus() != 1) {
      alarmAudioPlaying = false;
      // (Optional: more logic after audio)
    }
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

  // --- Normal cycling display (clock, date, year, temp) ---
  static unsigned long modeStartTime = 0;
  if (cycleState == 0 && nowMillis - modeStartTime > 10000) {
    cycleState = 1; modeStartTime = nowMillis;
  } else if (cycleState == 1 && nowMillis - modeStartTime > 2000) {
    cycleState = 2; modeStartTime = nowMillis;
  } else if (cycleState == 2 && nowMillis - modeStartTime > 2000) {
    cycleState = 3; modeStartTime = nowMillis;
  } else if (cycleState == 3 && nowMillis - modeStartTime > 2000) {
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

  delay(10); // Small delay for loop stability
}

/*
USE CASES:

- Digital clock with 12-hour display, weekday, temperature, and year cycling.
- Set time/date interactively with buttons (INC/DEC/OK/CFG).
- Hold OK for 2s in normal mode to enter alarm mode; select and set up to 5 alarms.
- Alarms are saved in EEPROM and survive resets.
- Buzzer feedback and alarm sound.
- NeoPixel display with color-coded indicators for AM/PM, alarm, etc.
- Shows temperature from DS3231 sensor.
- Hold INC/DEC for fast setting.
*/