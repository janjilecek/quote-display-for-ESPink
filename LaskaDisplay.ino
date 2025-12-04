/*
 * Quote Display for LaskaKit ESPink-Shelf-2.13 ESP32 e-Paper
 * Display: GDEY0213B74 (128x250)
 * Loads quotes from SD card, displays randomly without repeating
 * Tracks shown quotes in ESP32 NVS (persistent storage)
 * Battery: 2500mAh 3.7V 9.25Wh LiPo
 * Features: Deep sleep, button wake with audio, timer wake for quotes
 * FIX: Uses Streaming JSON Parsing to prevent "No Memory" errors
 */

#define ENABLE_GxEPD2_GFX 0

#include <GxEPD2_BW.h>
#include <SD.h>
#include <SPI.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <vector>
#include <driver/dac.h>
#include <driver/rtc_io.h>
#include <time.h>
#include <sys/time.h>
#include <WiFi.h>

// Build ID for detecting new code uploads
const char* BUILD_ID = __DATE__ " " __TIME__;

// WiFi credentials (only used once on new code upload)
const char* WIFI_SSID = "SSID";
const char* WIFI_PASS = "PASSWORD";
const char* NTP_SERVER = "pool.ntp.org";
const long GMT_OFFSET = 0;
const int DAYLIGHT_OFFSET = 0;

// Fonts
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSans12pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSerif9pt7b.h>

// E-Paper pins
#define DC    17 
#define RST   16  
#define BUSY  4 
#define POWER 2

// SD Card pins
#define SD_CS           15
#define SD_MOSI         23
#define SD_MISO         19
#define SD_SCK          18

// Button pin for wake - MUST be an RTC GPIO for deep sleep wake!
#define BUTTON_PIN 32

// Audio DAC
#define DAC_PIN DAC_CHANNEL_1
#define DC_OFFSET 128
#define AUDIO_VOLUME 0.8
#define AUDIO_BUFFER_SIZE 1024

// Battery monitoring
#define BAT_ADC   34
#define BAT_FULL  4.2
#define BAT_EMPTY 3.0
#define BAT_CRITICAL 3.2

// Display refresh interval 24hours
#define REFRESH_INTERVAL (24 * 60 * 60)
// Deep sleep conversion factor
#define uS_TO_S_FACTOR 1000000ULL

// RTC memory - survives deep sleep
RTC_DATA_ATTR time_t nextQuoteEpoch = 0;
RTC_DATA_ATTR int currentQuoteId = 0;
RTC_DATA_ATTR int lastCycleQuoteId = -1;  // Track last quote of previous cycle

// Display initialization - GDEY0213B74
GxEPD2_BW<GxEPD2_213_B74, GxEPD2_213_B74::HEIGHT> display(GxEPD2_213_B74(SS, DC, RST, BUSY));

// Preferences for persistent storage
Preferences preferences;

// --- MEMORY FIX: Struct for single quote buffering ---
struct QuoteData {
  int id;
  String text;
  String author;
};

// We only store IDs in RAM to save memory
std::vector<int> allQuoteIds;
int quoteCount = 0;

// WAV file header structure
typedef struct {
  char     riff[4];
  uint32_t fileSize;
  char     wave[4];
  char     fmt[4];
  uint32_t fmtSize;
  uint16_t audioFormat;
  uint16_t numChannels;
  uint32_t sampleRate;
  uint32_t byteRate;
  uint16_t blockAlign;
  uint16_t bitsPerSample;
} WavHeader;

// Tracking shown quotes (resets when all shown)
std::vector<int> shownQuoteIds;
int currentQuoteIdx = -1;

// Display counter - resets on new code upload
uint32_t totalDisplayCount = 0;

// Battery reading
float batteryVoltage = 0.0;
int batteryPercent = -1;

// SD card state tracking
bool sdInitialized = false;

// Function declarations
void playAudioFromSD(int quoteId);
bool readWavHeader(File &file, WavHeader &header);
bool findDataChunk(File &file, uint32_t &dataSize);
void displayRandomQuote();
void goToSleep(uint64_t sleepDurationSec);
void checkNewCodeUpload();
void syncTimeNTP();
void killWiFi();
void saveStatsToSD();
void clearShownQuotes();
String getTimestamp();
void cleanupAndSleep(uint64_t sleepDurationSec);
void showLowBatteryAndHalt();
bool initSD();
void closeSD();
void showError(const char* message);
bool scanQuoteIds(const char* filename);
bool getQuoteFromSD(int targetId, QuoteData &qData);
void loadShownQuotes();
void saveShownQuotes();
bool isQuoteShown(int id);
String determineBestFont(String text, int16_t maxW, int16_t maxH);
void drawQuoteText(String text, int16_t x, int16_t y, int16_t maxW, int16_t maxH);
void wrapText(String text, int16_t maxWidth, std::vector<String>& lines);

float readBatteryVoltage() {
  uint32_t sum = 0;
  for (int i = 0; i < 16; i++) {
    sum += analogRead(BAT_ADC);
    delay(2);
  }
  float avg = sum / 16.0;
  float voltage = (avg / 4095.0) * 3.3 * 2.0;
  return voltage;
}

int calcBatteryPercent(float voltage) {
  if (voltage < 1.0) return -1;
  if (voltage >= BAT_FULL) return 100;
  if (voltage <= BAT_EMPTY) return 0;
  return (int)(((voltage - BAT_EMPTY) / (BAT_FULL - BAT_EMPTY)) * 100.0);
}

// ==================== SD CARD MANAGEMENT ====================
bool initSD() {
  if (sdInitialized) return true;
   
  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if (!SD.begin(SD_CS)) {
    Serial.println("SD card initialization failed!");
    return false;
  }
  sdInitialized = true;
  Serial.println("SD card initialized.");
  return true;
}

void closeSD() {
  if (sdInitialized) {
    SD.end();
    sdInitialized = false;
    Serial.println("SD card closed.");
  }
}

// ==================== TIME FUNCTIONS ====================
void killWiFi() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  btStop();
  delay(100);
  Serial.println("WiFi OFF - permanently disabled");
}

void syncTimeNTP() {
  Serial.println("Connecting to WiFi for time sync...");
   
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
   
  int timeout = 30;
  while (WiFi.status() != WL_CONNECTED && timeout > 0) {
    delay(500);
    Serial.print(".");
    timeout--;
  }
   
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\nWiFi FAILED - using compile time instead");
     
    const char* date = __DATE__;
    const char* timeStr = __TIME__;
    char monthStr[4];
    int day, year, hour, min, sec;
    sscanf(date, "%s %d %d", monthStr, &day, &year);
    sscanf(timeStr, "%d:%d:%d", &hour, &min, &sec);
    const char* months[] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
    int month = 0;
    for (int i = 0; i < 12; i++) {
      if (strcmp(monthStr, months[i]) == 0) { month = i; break; }
    }
    struct tm t = {0};
    t.tm_year = year - 1900; t.tm_mon = month; t.tm_mday = day;
    t.tm_hour = hour; t.tm_min = min; t.tm_sec = sec;
    time_t epochTime = mktime(&t);
    struct timeval tv = { .tv_sec = epochTime, .tv_usec = 0 };
    settimeofday(&tv, NULL);
     
    killWiFi();
    return;
  }
   
  Serial.println("\nWiFi connected!");
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());
   
  Serial.println("Syncing time via NTP...");
  configTime(GMT_OFFSET, DAYLIGHT_OFFSET, NTP_SERVER);
   
  struct tm timeinfo;
  timeout = 20;
  while (!getLocalTime(&timeinfo) && timeout > 0) {
    delay(500);
    Serial.print(".");
    timeout--;
  }
   
  if (timeout <= 0) {
    Serial.println("\nNTP sync FAILED!");
  } else {
    Serial.println("\nNTP sync SUCCESS!");
    Serial.print("Time: ");
    Serial.println(getTimestamp());
  }
   
  killWiFi();
}

String getTimestamp() {
  time_t now;
  time(&now);
  struct tm* timeinfo = localtime(&now);
   
  char buf[25];
  sprintf(buf, "%04d-%02d-%02d_%02d-%02d-%02d",
    timeinfo->tm_year + 1900,
    timeinfo->tm_mon + 1,
    timeinfo->tm_mday,
    timeinfo->tm_hour,
    timeinfo->tm_min,
    timeinfo->tm_sec);
   
  return String(buf);
}

void checkNewCodeUpload() {
  String storedBuildId = preferences.getString("buildId", "");
   
  if (storedBuildId != BUILD_ID) {
    Serial.println("========================================");
    Serial.println("NEW CODE UPLOAD DETECTED!");
    Serial.println("========================================");
    Serial.print("Old build: ");
    Serial.println(storedBuildId.length() > 0 ? storedBuildId : "(none)");
    Serial.print("New build: ");
    Serial.println(BUILD_ID);
     
    totalDisplayCount = 0;
    preferences.putUInt("totalCnt", 0);
    Serial.println("Quote counter RESET to 0");
     
    clearShownQuotes();
    lastCycleQuoteId = -1;
     
    syncTimeNTP();
     
    preferences.putString("buildId", BUILD_ID);
     
    Serial.println("========================================");
  } else {
    Serial.print("Same build: ");
    Serial.println(BUILD_ID);
     
    WiFi.mode(WIFI_OFF);
    btStop();
  }
}

void saveStatsToSD() {
  String timestamp = getTimestamp();
  String filename = "/" + timestamp + ".txt";
   
  Serial.println("========================================");
  Serial.println("SAVING STATS TO SD CARD");
  Serial.print("Filename: ");
  Serial.println(filename);
   
  File file = SD.open(filename, FILE_WRITE);
  if (!file) {
    Serial.println("Failed to create stats file!");
    return;
  }
   
  float voltage = readBatteryVoltage();
  int percent = calcBatteryPercent(voltage);
   
  file.println("=== Quote Display Stats ===");
  file.print("Timestamp: ");
  file.println(timestamp);
  file.print("Build: ");
  file.println(BUILD_ID);
  file.println();
  file.print("Total Quotes Displayed: ");
  file.println(totalDisplayCount);
  file.print("Current Quote ID: ");
  file.println(currentQuoteId);
  file.println();
  file.print("Battery Voltage: ");
  file.print(voltage, 2);
  file.println(" V");
  file.print("Battery Percent: ");
  if (percent >= 0) {
    file.print(percent);
    file.println(" %");
  } else {
    file.println("Not connected");
  }
  file.println();
  file.print("Quotes in current cycle: ");
  file.print(shownQuoteIds.size());
  file.print("/");
  file.println(quoteCount);
   
  file.close();
   
  Serial.println("Stats saved successfully!");
  Serial.println("========================================");
}

// ==================== LOW BATTERY HANDLING ====================
void showLowBatteryAndHalt() {
  Serial.println("CRITICAL: Low battery detected!");
   
  pinMode(POWER, OUTPUT);
  digitalWrite(POWER, HIGH);
  delay(200);
   
  display.init(115200, true, 2, false);
  display.setRotation(1);
  display.setTextColor(GxEPD_BLACK);
   
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setFont(&FreeSansBold12pt7b);
     
    const char* msg1 = "Low Battery";
    const char* msg2 = "Please Recharge";
     
    int16_t tbx, tby;
    uint16_t tbw, tbh;
     
    display.getTextBounds(msg1, 0, 0, &tbx, &tby, &tbw, &tbh);
    display.setCursor((display.width() - tbw) / 2, display.height() / 2 - 10);
    display.print(msg1);
     
    display.setFont(&FreeSans9pt7b);
    display.getTextBounds(msg2, 0, 0, &tbx, &tby, &tbw, &tbh);
    display.setCursor((display.width() - tbw) / 2, display.height() / 2 + 20);
    display.print(msg2);
     
  } while (display.nextPage());
   
  display.powerOff();
  digitalWrite(POWER, LOW);
  gpio_hold_en((gpio_num_t)POWER);
   
  closeSD();
  preferences.end();
   
  Serial.println("Entering indefinite sleep (no wake sources)...");
  Serial.flush();
   
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  esp_deep_sleep_start();
}

// ==================== WAV HEADER READER ====================
bool readWavHeader(File &file, WavHeader &header) {
  if (file.read((uint8_t*)&header, sizeof(WavHeader)) != sizeof(WavHeader)) {
    return false;
  }
   
  if (strncmp(header.riff, "RIFF", 4) != 0 || 
      strncmp(header.wave, "WAVE", 4) != 0 ||
      strncmp(header.fmt, "fmt ", 4) != 0) {
    return false;
  }
   
  if (header.fmtSize > 16) {
    file.seek(file.position() + (header.fmtSize - 16));
  }
   
  return true;
}

// ==================== FIND DATA CHUNK ====================
bool findDataChunk(File &file, uint32_t &dataSize) {
  char chunkId[4];
  uint32_t chunkSize;
   
  while (file.available()) {
    if (file.read((uint8_t*)chunkId, 4) != 4) return false;
    if (file.read((uint8_t*)&chunkSize, 4) != 4) return false;
     
    if (strncmp(chunkId, "data", 4) == 0) {
      dataSize = chunkSize;
      return true;
    }
    file.seek(file.position() + chunkSize);
  }
  return false;
}

// ==================== WAV AUDIO PLAYER ====================
void playAudioFromSD(int quoteId) {
  String filename = "/wavs/" + String(quoteId) + ".wav";
   
  Serial.print("Loading WAV audio: ");
  Serial.println(filename);
   
  File wavFile = SD.open(filename);
  if (!wavFile) {
    Serial.println("WAV file not found!");
    return;
  }
   
  WavHeader header;
  if (!readWavHeader(wavFile, header)) {
    Serial.println("Invalid WAV header!");
    wavFile.close();
    return;
  }
   
  Serial.print("  Sample Rate: ");
  Serial.print(header.sampleRate);
  Serial.println(" Hz");
  Serial.print("  Format: ");
  Serial.print(header.bitsPerSample);
  Serial.print("-bit ");
  Serial.println(header.numChannels == 1 ? "Mono" : "Stereo");
   
  if (header.audioFormat != 1) {
    Serial.println("Error: Only PCM format supported!");
    wavFile.close();
    return;
  }
   
  uint32_t dataSize;
  if (!findDataChunk(wavFile, dataSize)) {
    Serial.println("Data chunk not found!");
    wavFile.close();
    return;
  }
   
  float duration = (float)dataSize / header.byteRate;
  Serial.print("  Duration: ");
  Serial.print(duration, 1);
  Serial.println(" sec");
  Serial.println("  Playing...");
   
  dac_output_enable(DAC_PIN);
  dac_output_voltage(DAC_PIN, DC_OFFSET);
  delay(50);
   
  uint32_t usPerSample = 1000000 / header.sampleRate;
  int bytesPerSample = header.bitsPerSample / 8 * header.numChannels;
   
  uint8_t audioBuffer[AUDIO_BUFFER_SIZE];
   
  uint32_t bytesPlayed = 0;
  uint32_t samplesPlayed = 0;
  uint32_t startTime = micros();
   
  while (wavFile.available() && bytesPlayed < dataSize) {
    int bytesToRead = min((uint32_t)AUDIO_BUFFER_SIZE, dataSize - bytesPlayed);
    int bytesRead = wavFile.read(audioBuffer, bytesToRead);
    if (bytesRead <= 0) break;
     
    for (int i = 0; i < bytesRead; i += bytesPerSample) {
      uint8_t sample;
       
      if (header.bitsPerSample == 16) {
        if (i + 1 < bytesRead) {
          int16_t sample16 = (int16_t)((audioBuffer[i + 1] << 8) | audioBuffer[i]);
          sample = (uint8_t)((sample16 + 32768) >> 8);
        } else {
          continue;
        }
      } else {
        sample = audioBuffer[i];
      }
       
      uint32_t targetTime = startTime + (samplesPlayed * usPerSample);
      while (micros() < targetTime) {
        // Busy wait for precise timing
      }
       
      int signedSample = sample - DC_OFFSET;
      signedSample = (int)(signedSample * AUDIO_VOLUME);
      int output = constrain(signedSample + DC_OFFSET, 0, 255);
      dac_output_voltage(DAC_PIN, output);
       
      samplesPlayed++;
    }
     
    bytesPlayed += bytesRead;
  }
   
  dac_output_voltage(DAC_PIN, DC_OFFSET);
  dac_output_disable(DAC_PIN);
   
  wavFile.close();
   
  Serial.println("Audio playback complete.");
}

// ==================== CLEANUP AND SLEEP ====================
void cleanupAndSleep(uint64_t sleepDurationSec) {
  // Close SD card to save power
  closeSD();
   
  // Close preferences
  preferences.end();
   
  // Hold display power pin LOW during sleep
  gpio_hold_en((gpio_num_t)POWER);
   
  // Configure button GPIO for wake with internal pullup
  rtc_gpio_init((gpio_num_t)BUTTON_PIN);
  rtc_gpio_set_direction((gpio_num_t)BUTTON_PIN, RTC_GPIO_MODE_INPUT_ONLY);
  rtc_gpio_pullup_en((gpio_num_t)BUTTON_PIN);
  rtc_gpio_pulldown_dis((gpio_num_t)BUTTON_PIN);
   
  // Configure button wake (wake on LOW/pressed)
  esp_sleep_enable_ext0_wakeup((gpio_num_t)BUTTON_PIN, 0);
   
  // Configure timer wake
  esp_sleep_enable_timer_wakeup(sleepDurationSec * uS_TO_S_FACTOR);
   
  Serial.print("Entering deep sleep for ");
  Serial.print(sleepDurationSec);
  Serial.println(" seconds (or until button press)...");
  Serial.flush();
   
  esp_deep_sleep_start();
}

void goToSleep(uint64_t sleepDurationSec) {
  // Update next quote time
  time_t now;
  time(&now);
  nextQuoteEpoch = now + sleepDurationSec;
   
  cleanupAndSleep(sleepDurationSec);
}

void setup() {
  Serial.begin(115200);
  Serial.println("\n\nQuote Display Starting...");
   
  // Release GPIO hold from previous sleep
  gpio_hold_dis((gpio_num_t)POWER);
   
  // Check wake reason
  esp_sleep_wakeup_cause_t wakeupReason = esp_sleep_get_wakeup_cause();
   
  Serial.print("Wake reason: ");
  switch (wakeupReason) {
    case ESP_SLEEP_WAKEUP_EXT0:
      Serial.println("Button press (EXT0)");
      break;
    case ESP_SLEEP_WAKEUP_TIMER:
      Serial.println("Timer");
      break;
    default:
      Serial.println("Power on / Reset");
      break;
  }
   
  // Initialize ADC for battery reading
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);
   
  // Check battery early
  delay(100);  // Let voltage stabilize
  batteryVoltage = readBatteryVoltage();
  batteryPercent = calcBatteryPercent(batteryVoltage);
   
  if (batteryVoltage > 1.0 && batteryVoltage < BAT_CRITICAL) {
    showLowBatteryAndHalt();
    return;  // Never reached
  }
   
  // Handle button wake - play audio and go back to sleep
  if (wakeupReason == ESP_SLEEP_WAKEUP_EXT0) {
    Serial.println("Button pressed - playing audio...");
     
    // Initialize SD card to load audio
    if (initSD()) {
      playAudioFromSD(currentQuoteId);
      closeSD();
    }
     
    // Calculate remaining sleep time using RTC
    time_t now;
    time(&now);
     
    uint64_t remainingSec = 0;
    if (nextQuoteEpoch > now) {
      remainingSec = nextQuoteEpoch - now;
    }
     
    // If very close to next quote time or past it, use full interval
    if (remainingSec < 60) {
      remainingSec = REFRESH_INTERVAL;
      Serial.println("Timer nearly done or expired, restarting full interval.");
    }
     
    Serial.print("Remaining sleep time: ");
    Serial.print(remainingSec);
    Serial.println(" seconds");
     
    // Go back to sleep (don't update nextQuoteEpoch)
    cleanupAndSleep(remainingSec);
    return;  // Never reached
  }
   
  // Normal startup - either timer wake or power on
  randomSeed(analogRead(0) + millis());
   
  pinMode(POWER, OUTPUT);
  digitalWrite(POWER, HIGH);
  Serial.println("Display power ON");
  delay(200);
   
  display.init(115200, true, 2, false);
  display.setRotation(1);
  display.setTextColor(GxEPD_BLACK);
  delay(100);
   
  // Initialize persistent storage
  preferences.begin("quotes", false);
   
  // Check for new code upload (resets counter, sets time)
  checkNewCodeUpload();
   
  // Load permanent display counter
  totalDisplayCount = preferences.getUInt("totalCnt", 0);
  Serial.print("Total quotes displayed: ");
  Serial.println(totalDisplayCount);
   
  // Initialize SD card
  if (!initSD()) {
    showError("SD Card Error");
    goToSleep(REFRESH_INTERVAL);
    return;
  }
   
  // --- CHANGED: Use scanQuoteIds instead of loadQuotes ---
  if (!scanQuoteIds("/quotes.json")) {
    showError("JSON Scan Error");
    goToSleep(REFRESH_INTERVAL);
    return;
  }
   
  Serial.print("Scanned IDs for ");
  Serial.print(quoteCount);
  Serial.println(" quotes.");
   
  // Load shown quotes from persistent storage
  loadShownQuotes();
   
  Serial.print("In current cycle: ");
  Serial.print(shownQuoteIds.size());
  Serial.println(" quotes shown.");
   
  // Display quote
  displayRandomQuote();
   
  // Power off display
  display.powerOff();
  delay(100);
   
  // Turn off display power supply
  digitalWrite(POWER, LOW);
  Serial.println("Display power OFF");
   
  // Go to sleep for full interval
  goToSleep(REFRESH_INTERVAL);
}

void loop() {
  // Never reached - deep sleep restarts from setup()
}

// ==================== NEW JSON STREAMING FUNCTIONS ====================

// Pass 1: Scan file just to get the IDs. Uses very little RAM.
bool scanQuoteIds(const char* filename) {
  File file = SD.open(filename);
  if (!file) {
    Serial.println("Failed to open quotes file");
    return false;
  }

  // 1. Jump to the start of the array
  // We look for "quotes" key, then the opening bracket '['
  if (!file.find("\"quotes\"")) {
    Serial.println("JSON format error: 'quotes' key not found");
    return false;
  }
  if (!file.find("[")) {
    Serial.println("JSON format error: Array start '[' not found");
    return false;
  }

  allQuoteIds.clear();

  // 2. Loop through the array one object at a time
  while (file.available()) {
    // We allocate enough for just ONE quote structure ID
    StaticJsonDocument<256> filter; 
    filter["id"] = true; // We only want the ID right now

    StaticJsonDocument<512> doc;
    
    // Deserialize only the current object in the stream
    DeserializationError error = deserializeJson(doc, file, DeserializationOption::Filter(filter));

    if (error) {
       // If we hit the end of the array or file, break gracefully
       if (error == DeserializationError::InvalidInput || error == DeserializationError::IncompleteInput) break;
       Serial.print("Parse error in loop: ");
       Serial.println(error.c_str());
       break; 
    }

    int id = doc["id"];
    if (id != 0) { // Valid ID found
      allQuoteIds.push_back(id);
    }

    // 3. Advance to the next object
    // We need to skip the comma ',' or find the end ']'
    while (file.available()) {
      char c = file.peek();
      if (c == ',') {
        file.read(); // Consume comma
        break; // Ready for next object
      } else if (c == ']') {
        file.close();
        quoteCount = allQuoteIds.size();
        return true; // End of array found
      } else {
        file.read(); // Consume whitespace/newlines
      }
    }
  }
  
  file.close();
  quoteCount = allQuoteIds.size();
  return quoteCount > 0;
}

// Pass 2: Fetch the full text for a single specific ID
bool getQuoteFromSD(int targetId, QuoteData &qData) {
  File file = SD.open("/quotes.json"); // Assuming filename is fixed
  if (!file) return false;

  if (!file.find("\"quotes\"")) return false;
  if (!file.find("[")) return false;

  while (file.available()) {
    // Allocate enough for ONE full quote (Text + Author)
    // 2KB is usually plenty for one quote
    StaticJsonDocument<2048> doc; 

    DeserializationError error = deserializeJson(doc, file);
    
    if (!error) {
      if (doc["id"] == targetId) {
        qData.id = doc["id"];
        qData.text = doc["text"].as<String>();
        qData.author = doc["author"].as<String>();
        file.close();
        return true;
      }
    }

    // Advance to next
    while (file.available()) {
      char c = file.peek();
      if (c == ',') {
        file.read();
        break;
      } else if (c == ']') {
        file.close();
        return false; // ID not found
      } else {
        file.read();
      }
    }
  }
  file.close();
  return false;
}

// ==================== HISTORY & DISPLAY LOGIC ====================

void loadShownQuotes() {
  shownQuoteIds.clear();
   
  String shown = preferences.getString("shown", "");
   
  if (shown.length() == 0) {
    Serial.println("No previously shown quotes in this cycle.");
    return;
  }
   
  Serial.print("Loading shown IDs: ");
  Serial.println(shown);
   
  int start = 0;
  for (int i = 0; i <= (int)shown.length(); i++) {
    if (i == (int)shown.length() || shown[i] == ',') {
      if (i > start) {
        String idStr = shown.substring(start, i);
        int id = idStr.toInt();
        if (id != 0 || idStr == "0") {
          shownQuoteIds.push_back(id);
        }
      }
      start = i + 1;
    }
  }
}

void saveShownQuotes() {
  String shown = "";
  for (size_t i = 0; i < shownQuoteIds.size(); i++) {
    if (i > 0) shown += ",";
    shown += String(shownQuoteIds[i]);
  }
   
  preferences.putString("shown", shown);
  Serial.print("Saved shown quotes: ");
  Serial.println(shown);
}

void clearShownQuotes() {
  shownQuoteIds.clear();
  preferences.putString("shown", "");
  Serial.println("=== ALL QUOTES SHOWN! Resetting cycle. ===");
}

bool isQuoteShown(int id) {
  for (size_t i = 0; i < shownQuoteIds.size(); i++) {
    if (shownQuoteIds[i] == id) return true;
  }
  return false;
}

int countWords(String text) {
  int count = 0;
  bool inWord = false;
  for (unsigned int i = 0; i < text.length(); i++) {
    if (text[i] == ' ' || text[i] == '\n' || text[i] == '\t') {
      inWord = false;
    } else if (!inWord) {
      inWord = true;
      count++;
    }
  }
  return count;
}

void displayRandomQuote() {
  if (allQuoteIds.empty()) {
    Serial.println("No quote IDs found!");
    return;
  }
   
  // Identify unshown IDs
  std::vector<int> unshown;
  for (int id : allQuoteIds) {
    if (!isQuoteShown(id)) {
      unshown.push_back(id);
    }
  }
   
  // Reset cycle if all shown
  if (unshown.empty()) {
    lastCycleQuoteId = currentQuoteId;
    clearShownQuotes();
    // Refill unshown
    for (int id : allQuoteIds) {
      if (!isQuoteShown(id)) unshown.push_back(id);
    }
  }
   
  // Avoid repeating last quote of previous cycle
  if (lastCycleQuoteId >= 0 && unshown.size() > 1) {
    for (auto it = unshown.begin(); it != unshown.end(); ++it) {
      if (*it == lastCycleQuoteId) {
        unshown.erase(it);
        Serial.print("Excluding last cycle quote ID: ");
        Serial.println(lastCycleQuoteId);
        break;
      }
    }
    lastCycleQuoteId = -1;  // Clear after using once
  }
   
  Serial.print("Unshown quotes remaining: ");
  Serial.println(unshown.size());
   
  // Select Random ID
  int randIdx = random(0, unshown.size());
  int selectedId = unshown[randIdx];
  
  // Fetch actual text from SD
  QuoteData quote;
  if (!getQuoteFromSD(selectedId, quote)) {
    Serial.print("Error: Could not fetch text for ID ");
    Serial.println(selectedId);
    return;
  }
   
  // Mark as shown in current cycle
  shownQuoteIds.push_back(quote.id);
  saveShownQuotes();
   
  // Store quote ID in RTC memory for audio playback on button press
  currentQuoteId = quote.id;
   
  // INCREMENT AND SAVE COUNTER
  totalDisplayCount++;
  preferences.putUInt("totalCnt", totalDisplayCount);
   
  // Save stats every 30 quotes
  if (totalDisplayCount % 30 == 0) {
    saveStatsToSD();
  }
   
  int charCount = quote.text.length();
  int wordCount = countWords(quote.text);
   
  Serial.println("-----------------------------");
  Serial.print("Display #: ");
  Serial.println(totalDisplayCount);
  Serial.print("Quote ID: ");
  Serial.println(quote.id);
  Serial.print("Text: ");
  Serial.println(quote.text);
  Serial.print("Author: ");
  Serial.println(quote.author);
  Serial.print("Characters: ");
  Serial.println(charCount);
  Serial.print("Words: ");
  Serial.println(wordCount);
   
  batteryVoltage = readBatteryVoltage();
  batteryPercent = calcBatteryPercent(batteryVoltage);
   
  if (batteryPercent >= 0) {
    Serial.print("Battery: ");
    Serial.print(batteryVoltage, 2);
    Serial.print("V (");
    Serial.print(batteryPercent);
    Serial.println("%)");
  } else {
    Serial.println("Battery: Not connected");
  }
   
  int16_t screenW = display.width();
  int16_t screenH = display.height();
  int16_t margin = 8;
  int16_t footerHeight = 20;
  int16_t textAreaH = screenH - footerHeight - margin * 2;
  int16_t textAreaW = screenW - margin * 2;
   
  String fontUsed = determineBestFont(quote.text, textAreaW, textAreaH);
   
  display.setFullWindow();
  display.firstPage();
   
  do {
    display.fillScreen(GxEPD_WHITE);
     
    drawQuoteText(quote.text, margin, margin, textAreaW, textAreaH);
     
    int16_t footerY = screenH - footerHeight;
    display.drawLine(margin, footerY, screenW - margin, footerY, GxEPD_BLACK);
     
    display.setFont(&FreeSans9pt7b);
    display.setCursor(margin, screenH - 5);
    display.print(quote.author);
     
    String counterStr = "#" + String(totalDisplayCount);
    int16_t tbx, tby;
    uint16_t tbw, tbh;
    display.getTextBounds(counterStr, 0, 0, &tbx, &tby, &tbw, &tbh);
    int16_t idX = screenW - margin - tbw;
     
    display.setCursor(idX, screenH - 5);
    display.print(counterStr);
     
    if (batteryPercent >= 0) {
        int16_t batW = 22;
        int16_t batH = 10;
        int16_t nubW = 3;
        int16_t nubH = 4;
        int16_t batGap = 8;
         
        int16_t batX = idX - batGap - batW - nubW;
        int16_t batY = screenH - 5 - batH;
         
        display.drawRect(batX, batY, batW, batH, GxEPD_BLACK);
        display.fillRect(batX + batW, batY + (batH - nubH)/2, nubW, nubH, GxEPD_BLACK);
         
        int numBars = 0;
        if (batteryPercent >= 10) numBars = 1;
        if (batteryPercent >= 35) numBars = 2;
        if (batteryPercent >= 60) numBars = 3;
        if (batteryPercent >= 85) numBars = 4;
         
        int16_t barW = 3;
        int16_t barGap = 2;
        int16_t innerH = batH - 4;
         
        for (int b = 0; b < numBars; b++) {
            int16_t barX = batX + 2 + (b * (barW + barGap));
            display.fillRect(barX, batY + 2, barW, innerH, GxEPD_BLACK);
        }
    }
     
  } while (display.nextPage());
   
  Serial.print("Font: ");
  Serial.println(fontUsed);
  Serial.print("Cycle progress: ");
  Serial.print(shownQuoteIds.size());
  Serial.print("/");
  Serial.print(quoteCount);
  Serial.println(" quotes");
  Serial.println("-----------------------------");
}

String determineBestFont(String text, int16_t maxW, int16_t maxH) {
  const GFXfont* fonts[] = {
    &FreeSansBold12pt7b, &FreeSans12pt7b, &FreeSansBold9pt7b,
    &FreeSans9pt7b, &FreeSerif9pt7b
  };
  const char* fontNames[] = {
    "FreeSansBold12pt", "FreeSans12pt", "FreeSansBold9pt",
    "FreeSans9pt", "FreeSerif9pt"
  };
  const int fontCount = 5;
  const int lineHeights[] = {22, 20, 16, 15, 15};
   
  for (int f = 0; f < fontCount; f++) {
    display.setFont(fonts[f]);
    int lineH = lineHeights[f];
     
    std::vector<String> lines;
    wrapText(text, maxW, lines);
     
    int totalHeight = lines.size() * lineH;
     
    if (totalHeight <= maxH || f == fontCount - 1) {
      return String(fontNames[f]);
    }
  }
  return "unknown";
}

void drawQuoteText(String text, int16_t x, int16_t y, int16_t maxW, int16_t maxH) {
  const GFXfont* fonts[] = {
    &FreeSansBold12pt7b, &FreeSans12pt7b, &FreeSansBold9pt7b,
    &FreeSans9pt7b, &FreeSerif9pt7b
  };
  const int fontCount = 5;
  const int lineHeights[] = {22, 20, 16, 15, 15};
   
  for (int f = 0; f < fontCount; f++) {
    display.setFont(fonts[f]);
    int lineH = lineHeights[f];
     
    std::vector<String> lines;
    wrapText(text, maxW, lines);
     
    int totalHeight = lines.size() * lineH;
     
    if (totalHeight <= maxH || f == fontCount - 1) {
      int16_t startY = y + lineH;
       
      if (totalHeight < maxH) {
        startY += (maxH - totalHeight) / 2;
      }
       
      for (size_t i = 0; i < lines.size(); i++) {
        display.setCursor(x, startY + i * lineH);
        display.print(lines[i]);
      }
      return;
    }
  }
}

void wrapText(String text, int16_t maxWidth, std::vector<String>& lines) {
  lines.clear();
  maxWidth -= 10;  // Small margin for safety
   
  String currentLine = "";
  int startIdx = 0;
   
  for (int i = 0; i <= (int)text.length(); i++) {
    char c = (i < (int)text.length()) ? text.charAt(i) : ' ';
     
    if (c == ' ' || i == (int)text.length()) {
      String word = text.substring(startIdx, i);
       
      if (word.length() == 0) {
        startIdx = i + 1;
        continue;
      }
       
      String testLine = currentLine;
      if (testLine.length() > 0) testLine += " ";
      testLine += word;
       
      int16_t tbx, tby;
      uint16_t tbw, tbh;
      display.getTextBounds(testLine.c_str(), 0, 0, &tbx, &tby, &tbw, &tbh);
       
      if ((int)tbw <= maxWidth) {
        currentLine = testLine;
      } else {
        if (currentLine.length() > 0) lines.push_back(currentLine);
         
        display.getTextBounds(word.c_str(), 0, 0, &tbx, &tby, &tbw, &tbh);
         
        if ((int)tbw <= maxWidth) {
          currentLine = word;
        } else {
          String part = "";
          for (unsigned int j = 0; j < word.length(); j++) {
            String testPart = part + word.charAt(j);
            display.getTextBounds(testPart.c_str(), 0, 0, &tbx, &tby, &tbw, &tbh);
            if ((int)tbw > maxWidth && part.length() > 0) {
              lines.push_back(part);
              part = String(word.charAt(j));
            } else {
              part = testPart;
            }
          }
          currentLine = part;
        }
      }
      startIdx = i + 1;
    }
  }
   
  if (currentLine.length() > 0) lines.push_back(currentLine);
}

void showError(const char* message) {
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setFont(&FreeSansBold12pt7b);
    display.setTextColor(GxEPD_BLACK);
     
    int16_t tbx, tby;
    uint16_t tbw, tbh;
    display.getTextBounds(message, 0, 0, &tbx, &tby, &tbw, &tbh);
     
    display.setCursor((display.width() - tbw) / 2, display.height() / 2);
    display.print(message);
  } while (display.nextPage());
}