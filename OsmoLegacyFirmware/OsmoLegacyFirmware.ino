/*
 * Osmo Notifier Firmware — E-ink Notification Pager
 * Authored by Ty Builds --- @gthhub on Github
 * @tybuilds_ on X 

 * Made for Waveshare ESP32-S3 1.54" e-Paper (200x200)
 *
 * Button A (single button):
 *   short press  = dismiss current notification (on device only)
 *   double press = dismiss all notifications (device only)
 *
 * BLE:
 *   - ANCS client for iPhone notifications
 *
*/

#include <SPI.h>
#include <Wire.h>
#include <NimBLEDevice.h>
#include <GxEPD2_BW.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <time.h>

// ============================================================
// PIN DEFINITIONS (Waveshare ESP32-S3-ePaper-1.54)
// ============================================================
#define EPAPER_CS    11
#define EPAPER_DC    10
#define EPAPER_RST   9
#define EPAPER_BUSY  8
#define EPAPER_CLK   12
#define EPAPER_DIN   13
#define EPAPER_PWR   6   // Active LOW: set LOW to power ON

#define BUTTON_A_PIN 0   // Single button: dismiss / toggle mode / dismiss all
#define PWR_BUTTON_PIN 18  // PWR button: long press to power off

#define BATT_ADC_PIN 4   // ADC1_CH3 — battery voltage via 1:2 divider

#define RTC_SDA 47
#define RTC_SCL 48
#define RTC_ADDR 0x51  // PCF85063 I2C address

#define BUTTON_DEBOUNCE_MS 50
#define LONG_PRESS_MS 500
#define MULTI_PRESS_WINDOW_MS 600  // Max gap between presses for multi-press

// ============================================================
// DISPLAY
// ============================================================
SPIClass epd_spi(HSPI);

// Waveshare 1.54" 200x200 - using GDEY0154D67 driver (SSD1681)
GxEPD2_BW<GxEPD2_154_GDEY0154D67, GxEPD2_154_GDEY0154D67::HEIGHT> display(
  GxEPD2_154_GDEY0154D67(EPAPER_CS, EPAPER_DC, EPAPER_RST, EPAPER_BUSY)
);

// ============================================================
// NOTIFICATION STORAGE
// ============================================================
struct Notification {
  uint32_t uid;
  char app[32];
  char title[100];
  char message[96];
  uint32_t timestamp;
  bool valid;
};

#define MAX_NOTIFICATIONS 30
Notification notifications[MAX_NOTIFICATIONS];
int notificationCount = 0;
int currentPage = 0;

// Queue for notifications arriving while display is busy
#define NOTIF_QUEUE_SIZE 10
struct QueuedNotif {
  uint32_t uid;
  char app[32];
  char title[100];
  char message[96];
  bool valid;
};
QueuedNotif notifQueue[NOTIF_QUEUE_SIZE];
int queueCount = 0;

// ============================================================
// STATE
// ============================================================
volatile bool needsDisplayUpdate = false;
bool isDisplayBusy = false;
int partialRefreshCount = 0;
#define FULL_REFRESH_INTERVAL 5

// Stats
uint32_t statsDismissed = 0;
uint32_t statsViewed = 0;

// Button state (single button with multi-press detection)
bool buttonAPressed = false;
unsigned long buttonAPressStart = 0;
bool buttonALongTriggered = false;
int buttonAPressCount = 0;           // Accumulated presses in current window
unsigned long buttonALastRelease = 0; // When last release happened

// PWR button state
bool pwrButtonPressed = false;
unsigned long pwrButtonPressStart = 0;
#define PWR_OFF_HOLD_MS 2000  // Hold PWR 2 seconds to shut down

// RTC time sync
bool rtcSynced = false;
unsigned long ancsSubscribedAt = 0;  // millis() when ANCS first subscribed
#define INITIAL_BURST_MS 5000        // ignore notif timestamps for 5s after subscribe

// BLE
NimBLEServer* pServer = nullptr;
NimBLEClient* pAncsClient = nullptr;
NimBLERemoteCharacteristic* pControlPointCharacteristic = nullptr;
bool bleConnected = false;
bool ancsSubscribed = false;
bool ancsDiscoveryNeeded = false;

// ============================================================
// BLE UUIDs
// ============================================================
// ANCS UUIDs (Apple standard)
static NimBLEUUID ancsServiceUUID("7905F431-B5CE-4E99-A40F-4B1E122D00D0");
static NimBLEUUID ancsNotifSourceUUID("9FBF120D-6301-42D9-8C58-25E699A21DBD");
static NimBLEUUID ancsControlPointUUID("69D1D8F3-45E1-49A8-9821-9BBDFDAAD9D9");
static NimBLEUUID ancsDataSourceUUID("22EAC6E9-24D6-4BB5-BE44-B36ACE7C7BFB");

// ============================================================
// FORWARD DECLARATIONS
// ============================================================
void displayInit();
void drawWaitingScreen();
void drawReadyScreen();
void drawDisconnectedScreen();
void drawNotifications();
void addNotification(uint32_t uid, const char* app, const char* title, const char* msg);
bool removeNotificationByUID(uint32_t uid);
void dismissCurrentNotification();
void dismissAllNotifications();
void sendStatsToApp();

// ============================================================
// BLE — ANCS (NimBLE, pServer->getClient pattern per h2zero's example)
// ============================================================
volatile bool pendingNotification = false;
uint8_t latestMessageID[4];

// Forward declarations for BLE callbacks
void ancsNotifCallback(NimBLERemoteCharacteristic* pChar, uint8_t* pData, size_t length, bool isNotify);
void ancsDataCallback(NimBLERemoteCharacteristic* pChar, uint8_t* pData, size_t length, bool isNotify);

// Discover ANCS service and subscribe — called from loop() after encryption
bool discoverAndSubscribeANCS() {
  if (!pAncsClient || !pAncsClient->isConnected()) {
    Serial.println("ANCS discovery: client not connected");
    return false;
  }

  Serial.println("Discovering ANCS service...");
  NimBLERemoteService* pAncsService = pAncsClient->getService(ancsServiceUUID);
  if (!pAncsService) {
    Serial.println("ANCS service not found!");
    return false;
  }
  Serial.println("ANCS service found!");

  NimBLERemoteCharacteristic* pNotifSourceChar = pAncsService->getCharacteristic(ancsNotifSourceUUID);
  pControlPointCharacteristic = pAncsService->getCharacteristic(ancsControlPointUUID);
  NimBLERemoteCharacteristic* pDataSourceChar = pAncsService->getCharacteristic(ancsDataSourceUUID);

  if (!pNotifSourceChar || !pControlPointCharacteristic || !pDataSourceChar) {
    Serial.println("Failed to find ANCS characteristics!");
    return false;
  }
  Serial.println("All ANCS characteristics found!");

  // Subscribe — Data Source first, then Notification Source (per Apple spec)
  bool ds = pDataSourceChar->subscribe(true, ancsDataCallback);
  Serial.printf("Data Source subscribe: %s\n", ds ? "OK" : "FAILED");

  bool ns = pNotifSourceChar->subscribe(true, ancsNotifCallback);
  Serial.printf("Notification Source subscribe: %s\n", ns ? "OK" : "FAILED");

  // Manual CCCD writes — NimBLE's subscribe() via getClient() misses the
  // NotifSource CCCD, so we write directly. Handle+1 = CCCD descriptor.
  uint16_t notifCccdHandle = pNotifSourceChar->getHandle() + 1;
  uint16_t dataCccdHandle = pDataSourceChar->getHandle() + 1;
  uint8_t enableNotif[] = {0x01, 0x00};

  int rc1 = ble_gattc_write_flat(pAncsClient->getConnHandle(), notifCccdHandle,
                                  enableNotif, 2, NULL, NULL);
  Serial.printf("Raw CCCD write NotifSource (handle %d): rc=%d\n", notifCccdHandle, rc1);

  int rc2 = ble_gattc_write_flat(pAncsClient->getConnHandle(), dataCccdHandle,
                                  enableNotif, 2, NULL, NULL);
  Serial.printf("Raw CCCD write DataSource (handle %d): rc=%d\n", dataCccdHandle, rc2);

  return rc1 == 0 && rc2 == 0;
}

// Server callbacks
class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* pSrv, NimBLEConnInfo& connInfo) override {
    Serial.printf("iPhone connected: %s\n", connInfo.getAddress().toString().c_str());
    bleConnected = true;
    if (!connInfo.isEncrypted()) {
      Serial.println("Requesting encryption...");
      NimBLEDevice::startSecurity(connInfo.getConnHandle());
    }
  }

  void onDisconnect(NimBLEServer* pSrv, NimBLEConnInfo& connInfo, int reason) override {
    Serial.printf("Disconnected (reason: %d)\n", reason);
    bleConnected = false;
    ancsSubscribed = false;
    ancsDiscoveryNeeded = false;
    pControlPointCharacteristic = nullptr;
    pAncsClient = nullptr;

    drawDisconnectedScreen();
  }

  void onAuthenticationComplete(NimBLEConnInfo& connInfo) override {
    if (connInfo.isEncrypted()) {
      Serial.println("Bonded! Getting client handle...");
      pAncsClient = pServer->getClient(connInfo);
      Serial.printf("Got client handle: %s\n", pAncsClient ? "OK" : "FAILED");
      if (pAncsClient) {
        ancsDiscoveryNeeded = true;
      }
    } else {
      Serial.println("Auth complete but NOT encrypted");
    }
  }
};

// ============================================================
// SETUP
// ============================================================
void setup() {
  // Latch battery power ON — must be first thing in setup!
  // The PWR button only provides momentary power; GPIO 17 holds it.
  pinMode(GPIO_NUM_17, OUTPUT);
  digitalWrite(GPIO_NUM_17, HIGH);

  setCpuFrequencyMhz(80);

  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("========================================");
  Serial.println("Osmo E-ink Notification Pager");
  Serial.println("========================================");

  // Buttons
  pinMode(BUTTON_A_PIN, INPUT_PULLUP);
  pinMode(PWR_BUTTON_PIN, INPUT_PULLUP);

  // Battery ADC
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);

  // RTC
  rtcInit();
  Serial.println("RTC initialized");


  // Display
  Serial.println("Initializing display...");
  displayInit();
  drawWaitingScreen();
  Serial.printf("Display: %dx%d\n", display.width(), display.height());

  // BLE
  Serial.println("Initializing BLE...");
  NimBLEDevice::init("Osmo");
  NimBLEDevice::setSecurityAuth(true, true, true);   // bonding, MITM, SC
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
  NimBLEDevice::setPower(9);

  // Create BLE server
  pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());
  pServer->advertiseOnDisconnect(true);

  // Add a minimal HID service so iOS recognizes us as an accessory
  // This makes the device appear in iOS Settings > Bluetooth
  NimBLEService* pHidService = pServer->createService(NimBLEUUID((uint16_t)0x1812));
  // HID Information characteristic (mandatory for HID service)
  NimBLECharacteristic* pHidInfo = pHidService->createCharacteristic(
    NimBLEUUID((uint16_t)0x2A4A), NIMBLE_PROPERTY::READ
  );
  uint8_t hidInfo[] = {0x01, 0x11, 0x00, 0x02};  // HID v1.11, no country, normally connectable
  pHidInfo->setValue(hidInfo, 4);
  // Report Map characteristic (mandatory, minimal empty map)
  NimBLECharacteristic* pReportMap = pHidService->createCharacteristic(
    NimBLEUUID((uint16_t)0x2A4B), NIMBLE_PROPERTY::READ
  );
  uint8_t reportMap[] = {0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01, 0xC0};  // minimal consumer control
  pReportMap->setValue(reportMap, sizeof(reportMap));
  pHidService->start();

  // Device Information service (iOS often expects this with HID)
  NimBLEService* pDevInfoService = pServer->createService(NimBLEUUID((uint16_t)0x180A));
  pDevInfoService->createCharacteristic(
    NimBLEUUID((uint16_t)0x2A29), NIMBLE_PROPERTY::READ
  )->setValue("Osmo");
  pDevInfoService->start();

  // Advertising — split across adv packet and scan response
  NimBLEAdvertising* pAdvertising = pServer->getAdvertising();

  // Primary advertisement: flags + HID UUID + appearance + name
  NimBLEAdvertisementData advData;
  advData.setFlags(0x06);
  advData.setAppearance(0x03C0);                           // Generic HID
  advData.addServiceUUID(NimBLEUUID((uint16_t)0x1812));    // HID — iOS shows this in Settings
  advData.setName("Osmo");
  pAdvertising->setAdvertisementData(advData);

  // Scan response: ANCS as SOLICITATION (AD type 0x15), NOT regular service UUID
  NimBLEAdvertisementData scanData;
  uint8_t solicitData[] = {
    0x11,  // Length: 17 bytes (1 type + 16 UUID)
    0x15,  // AD type: List of 128-bit Service Solicitation UUIDs
    // ANCS UUID in little-endian:
    0xD0, 0x00, 0x2D, 0x12, 0x1E, 0x4B, 0x0F, 0xA4,
    0x99, 0x4E, 0xCE, 0xB5, 0x31, 0xF4, 0x05, 0x79
  };
  scanData.addData(solicitData, sizeof(solicitData));
  pAdvertising->setScanResponseData(scanData);

  pAdvertising->start();

  Serial.println("BLE advertising started");
  Serial.println("Setup complete. Connect via nRF Connect or Bluetooth settings.\n");
}

// ============================================================
// ANCS NOTIFICATION HANDLING
// ============================================================

// ANCS Notification Source callback
void ancsNotifCallback(NimBLERemoteCharacteristic* pChar, uint8_t* pData, size_t length, bool isNotify) {
  Serial.printf(">>> ANCS NotifSource callback! len=%d\n", length);
  if (length < 8) return;

  uint8_t eventID = pData[0];
  uint8_t eventFlags = pData[1];
  uint8_t categoryID = pData[2];
  uint8_t categoryCount = pData[3];
  uint32_t uid = *(uint32_t*)&pData[4];

  if (eventID == 2) {  // Notification Removed
    Serial.printf("ANCS: Removed UID %lu\n", uid);
    if (removeNotificationByUID(uid)) {
      needsDisplayUpdate = true;
    }
    return;
  }

  if (eventID != 0) return;  // Only process Added

  Serial.printf("ANCS: New notification UID=%lu cat=%d\n", uid, categoryID);

  latestMessageID[0] = pData[4];
  latestMessageID[1] = pData[5];
  latestMessageID[2] = pData[6];
  latestMessageID[3] = pData[7];
  pendingNotification = true;
}

// ANCS Data Source callback — receives attribute data
static uint8_t dataBuffer[512];
static int dataBufferLen = 0;

void ancsDataCallback(NimBLERemoteCharacteristic* pChar, uint8_t* pData, size_t length, bool isNotify) {
  Serial.printf(">>> ANCS DataSource callback! len=%d\n", length);
  // Accumulate data
  if (length + dataBufferLen > sizeof(dataBuffer)) {
    dataBufferLen = 0;
    return;
  }
  memcpy(&dataBuffer[dataBufferLen], pData, length);
  dataBufferLen += length;

  // Try to parse: CommandID(1) + UID(4) + attributes...
  if (dataBufferLen < 5) return;

  uint8_t cmdID = dataBuffer[0];
  if (cmdID != 0) { // Not GetNotificationAttributes response
    dataBufferLen = 0;
    return;
  }

  uint32_t uid = *(uint32_t*)&dataBuffer[1];
  int pos = 5;

  char appID[64] = {0};
  char title[48] = {0};
  char message[96] = {0};
  char subtitle[48] = {0};
  char dateStr[20] = {0};

  // Parse TLV attributes
  while (pos + 3 <= dataBufferLen) {
    uint8_t attrID = dataBuffer[pos];
    uint16_t attrLen = *(uint16_t*)&dataBuffer[pos + 1];
    pos += 3;

    if (pos + attrLen > dataBufferLen) {
      return;  // Need more data
    }

    switch (attrID) {
      case 0:  // App Identifier
        strncpy(appID, (char*)&dataBuffer[pos], min((int)attrLen, 63));
        break;
      case 1:  // Title
        strncpy(title, (char*)&dataBuffer[pos], min((int)attrLen, 47));
        break;
      case 2:  // Subtitle
        strncpy(subtitle, (char*)&dataBuffer[pos], min((int)attrLen, 47));
        break;
      case 3:  // Message
        strncpy(message, (char*)&dataBuffer[pos], min((int)attrLen, 95));
        break;
      case 5:  // Date (format: "yyyyMMddTHHmmss")
        strncpy(dateStr, (char*)&dataBuffer[pos], min((int)attrLen, 19));
        break;
    }
    pos += attrLen;
  }

  // We've parsed a complete notification
  dataBufferLen = 0;

  // Sync RTC from notification timestamp
  if (strlen(dateStr) >= 15) {
    syncRTCFromANCSDate(dateStr, strlen(dateStr));
  }

  // Get app display name from app ID
  char appName[32];
  const char* lastDot = strrchr(appID, '.');
  if (lastDot && strlen(lastDot + 1) > 0) {
    strncpy(appName, lastDot + 1, 31);
    appName[31] = '\0';
    if (appName[0] >= 'a' && appName[0] <= 'z') {
      appName[0] -= 32;
    }
  } else {
    strncpy(appName, appID, 31);
    appName[31] = '\0';
  }

  // Combine title + subtitle for emails
  if (strlen(subtitle) > 0 && strlen(title) > 0) {
    char combined[48];
    snprintf(combined, sizeof(combined), "%s: %s", title, subtitle);
    strncpy(title, combined, 47);
    title[47] = '\0';
  }

  // Skip empty notifications
  if (strlen(appName) == 0 && strlen(title) == 0 && strlen(message) == 0) {
    return;
  }

  Serial.println("---");
  Serial.printf("App:  %s\n", appName);
  Serial.printf("Title: %s\n", title);
  Serial.printf("Msg:  %s\n", message);
  Serial.println("---");

  addNotification(uid, appName, title, message);
  needsDisplayUpdate = true;
}

// ============================================================
// BATTERY
// ============================================================
int getBatteryPercent() {
  uint32_t raw = analogRead(BATT_ADC_PIN);
  float voltage = (raw / 4096.0) * 3.3 * 2.0;  // 1:2 voltage divider
  // LiPo range: 3.0V (empty) to 3.7V (full)
  int pct = (int)((voltage - 3.0) / (3.9 - 3.0) * 100.0);
  if (pct > 100) pct = 100;
  if (pct < 0) pct = 0;
  return pct;
}

// Draw battery icon with 4 levels (1/4, half, 3/4, full); show "LOW" at 1/4
void drawBatteryIndicator() {
  int pct = getBatteryPercent();

  // 4 discrete levels
  int level;
  if (pct <= 25)      level = 1;  // 1/4 — lowest, show LOW
  else if (pct <= 50) level = 2;  // half
  else if (pct <= 75) level = 3;  // 3/4
  else                level = 4;  // full

  // Battery outline: 24x12 at bottom-right, with 2px nub on right
  int bx = 168, by = 178;
  int bw = 24, bh = 12;
  display.drawRect(bx, by, bw, bh, GxEPD_BLACK);
  display.fillRect(bx + bw, by + 3, 2, 6, GxEPD_BLACK);

  // Fill segments (4 equal segments inside the battery)
  int segW = (bw - 4) / 4;
  for (int i = 0; i < level; i++) {
    display.fillRect(bx + 2 + i * segW, by + 2, segW - 1, bh - 4, GxEPD_BLACK);
  }

  // Show "LOW" to the left of the icon at the lowest level
  if (level == 1) {
    display.setFont(NULL);
    display.setTextColor(GxEPD_BLACK);
    display.setCursor(bx - 24, by + 2);
    display.print("LOW");
  }
}

// ============================================================
// RTC (PCF85063 over I2C)
// ============================================================
static uint8_t bcd2dec(uint8_t bcd) { return (bcd >> 4) * 10 + (bcd & 0x0F); }
static uint8_t dec2bcd(uint8_t dec) { return ((dec / 10) << 4) | (dec % 10); }

void rtcInit() {
  Wire.begin(RTC_SDA, RTC_SCL);
  // Reset the stop flag (register 0x00, bit 5) to ensure RTC is running
  Wire.beginTransmission(RTC_ADDR);
  Wire.write(0x00);
  Wire.write(0x00);  // Control_1: normal mode, RTC running
  Wire.endTransmission();
}

void rtcWrite(int year, int month, int day, int hour, int minute, int second) {
  Wire.beginTransmission(RTC_ADDR);
  Wire.write(0x04);  // start at Seconds register
  Wire.write(dec2bcd(second));
  Wire.write(dec2bcd(minute));
  Wire.write(dec2bcd(hour));
  Wire.write(dec2bcd(day));
  Wire.write(0x00);  // weekday (don't care)
  Wire.write(dec2bcd(month));
  Wire.write(dec2bcd(year % 100));
  Wire.endTransmission();
  Serial.printf("RTC set: %04d-%02d-%02d %02d:%02d:%02d\n",
                year, month, day, hour, minute, second);
}

bool rtcRead(int &hour, int &minute, int &second) {
  Wire.beginTransmission(RTC_ADDR);
  Wire.write(0x04);  // start at Seconds register
  if (Wire.endTransmission() != 0) return false;
  Wire.requestFrom((uint8_t)RTC_ADDR, (uint8_t)3);
  if (Wire.available() < 3) return false;
  second = bcd2dec(Wire.read() & 0x7F);  // mask OS flag
  minute = bcd2dec(Wire.read() & 0x7F);
  hour   = bcd2dec(Wire.read() & 0x3F);
  return true;
}

// Parse ANCS date string "yyyyMMddTHHmmss" and write to RTC
void syncRTCFromANCSDate(const char* dateStr, int len) {
  if (len < 15) return;
  // Skip if still in the initial notification burst
  if (ancsSubscribedAt > 0 && (millis() - ancsSubscribedAt < INITIAL_BURST_MS)) {
    Serial.println("RTC sync: skipping (initial burst)");
    return;
  }

  char buf[5];
  strncpy(buf, dateStr, 4); buf[4] = '\0';
  int year = atoi(buf);
  strncpy(buf, dateStr + 4, 2); buf[2] = '\0';
  int month = atoi(buf);
  strncpy(buf, dateStr + 6, 2); buf[2] = '\0';
  int day = atoi(buf);
  // dateStr[8] == 'T'
  strncpy(buf, dateStr + 9, 2); buf[2] = '\0';
  int hour = atoi(buf);
  strncpy(buf, dateStr + 11, 2); buf[2] = '\0';
  int minute = atoi(buf);
  strncpy(buf, dateStr + 13, 2); buf[2] = '\0';
  int second = atoi(buf);

  if (year < 2024 || month < 1 || month > 12) return;  // sanity check

  rtcWrite(year, month, day, hour, minute, second);
  rtcSynced = true;
}

// ============================================================
// DISPLAY FUNCTIONS
// ============================================================

void displayInit() {
  // Power on the e-paper display (active LOW)
  pinMode(EPAPER_PWR, OUTPUT);
  digitalWrite(EPAPER_PWR, LOW);
  delay(200);

  // Init SPI on HSPI bus with correct pins
  epd_spi.begin(EPAPER_CLK, -1, EPAPER_DIN, -1);

  // Tell GxEPD2 to use our SPI bus
  display.epd2.selectSPI(epd_spi, SPISettings(4000000, MSBFIRST, SPI_MODE0));

  display.init(115200, true, 2, false);
  display.setRotation(0);  // Portrait 200x200
  display.setTextColor(GxEPD_BLACK);
}

void drawWaitingScreen() {
  isDisplayBusy = true;
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);

    display.setFont(&FreeSansBold9pt7b);
    display.setTextColor(GxEPD_BLACK);
    display.setCursor(60, 70);
    display.print("Osmo");

    display.setFont(&FreeSans9pt7b);
    display.setCursor(20, 100);
    display.print("Waiting for iPhone...");

    display.setCursor(30, 130);
    display.print("Pair via Bluetooth");
  } while (display.nextPage());
  isDisplayBusy = false;
}

void drawReadyScreen() {
  isDisplayBusy = true;
  displayInit();
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);

    // Header
    display.fillRect(0, 0, 200, 22, GxEPD_BLACK);
    display.setFont(&FreeSansBold9pt7b);
    display.setTextColor(GxEPD_WHITE);
    display.setCursor(4, 16);
    display.print("Osmo");

    // "Osmosis" wave on ready screen header
    {
      int16_t ox, oy; uint16_t ow, oh;
      display.getTextBounds("Osmo", 0, 0, &ox, &oy, &ow, &oh);
      int wStart = 4 + ow + 6;
      int wEnd = 196;  // no page counter on ready screen
      int wMidY = 11;  // center of 22px header
      for (int ripple = 0; ripple < 3; ripple++) {
        int rx = wStart + ripple * 14;
        if (rx >= wEnd) break;
        int arcH = 6 - ripple;
        if (arcH < 2) arcH = 2;
        for (int dy = -arcH; dy <= arcH; dy++) {
          int py = wMidY + dy;
          if (py < 1 || py > 20) continue;
          int dx = (int)(0.5 + (float)(arcH * arcH - dy * dy) / (float)(arcH > 0 ? arcH : 1));
          int px = rx + dx;
          if (px >= wStart && px < wEnd) {
            display.drawPixel(px, py, GxEPD_WHITE);
            if (abs(dy) <= arcH / 2 && px + 1 < wEnd) {
              display.drawPixel(px + 1, py, GxEPD_WHITE);
            }
          }
        }
      }
    }

    // All clear message, centered
    display.setFont(&FreeSansBold9pt7b);
    display.setTextColor(GxEPD_BLACK);
    int16_t acx, acy; uint16_t acw, ach;
    display.getTextBounds("All caught up", 0, 0, &acx, &acy, &acw, &ach);
    display.setCursor((200 - acw) / 2, 90);
    display.print("All caught up");

    // Checkmark, centered (20px wide)
    int cx = (200 - 20) / 2, cy = 115;
    for (int i = 0; i < 3; i++) {
      display.drawLine(cx, cy + i, cx + 8, cy + 10 + i, GxEPD_BLACK);
      display.drawLine(cx + 8, cy + 10 + i, cx + 20, cy - 5 + i, GxEPD_BLACK);
    }

    // Mode indicator at bottom left
    display.setFont(&FreeSans9pt7b);
    display.setCursor(4, 190);
    display.print("Mode: All");

    // Battery indicator at bottom right
    drawBatteryIndicator();
  } while (display.nextPage());
  partialRefreshCount = 0;
  currentPage = 0;
  isDisplayBusy = false;
}

void drawDisconnectedScreen() {
  isDisplayBusy = true;
  displayInit();
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);

    display.setFont(&FreeSansBold9pt7b);
    display.setTextColor(GxEPD_BLACK);
    display.setCursor(30, 80);
    display.print("Disconnected");

    display.setFont(&FreeSans9pt7b);
    display.setCursor(25, 110);
    display.print("Reconnecting...");
  } while (display.nextPage());
  isDisplayBusy = false;
}

void drawNotifications() {
  if (notificationCount == 0) return;

  isDisplayBusy = true;

  // Always do a full refresh to prevent ghosting between notifications
  // (partial refresh was leaving too many afterimages from previous notification content)
  // bool doFullRefresh = (partialRefreshCount >= FULL_REFRESH_INTERVAL);
  // if (doFullRefresh) {
  //   partialRefreshCount = 0;
  //   display.setFullWindow();
  // } else {
  //   partialRefreshCount++;
  //   display.setPartialWindow(0, 0, 200, 200);
  // }
  display.setFullWindow();

  if (currentPage >= notificationCount) currentPage = notificationCount - 1;
  Notification* n = &notifications[currentPage];

  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setTextWrap(false);  // we handle wrapping manually

    // === Top bar ===
    display.fillRect(0, 0, 200, 20, GxEPD_BLACK);
    display.setFont(&FreeSans9pt7b);
    display.setTextColor(GxEPD_WHITE);
    display.setCursor(4, 14);
    display.print("Osmo");

    // Measure "Osmo" width to know where wave can start
    int16_t ox, oy; uint16_t ow, oh;
    display.getTextBounds("Osmo", 0, 0, &ox, &oy, &ow, &oh);
    int waveStart = 4 + ow + 6; // 6px gap after text

    // Notification count (right side)
    char countStr[12];
    snprintf(countStr, sizeof(countStr), "%d/%d", currentPage + 1, notificationCount);
    int16_t tbx, tby; uint16_t tbw, tbh;
    display.getTextBounds(countStr, 0, 0, &tbx, &tby, &tbw, &tbh);
    display.setCursor(196 - tbw, 14);
    display.print(countStr);

    // === Osmosis wave emanating from "Osmo" ===
    int waveEnd = 196 - tbw - 6; // 6px gap before page counter
    int waveMidY = 10;            // vertical center of 20px header
    // Draw 3 concentric ripple arcs that fade out (fewer pixels) to the right
    for (int ripple = 0; ripple < 3; ripple++) {
      int rx = waveStart + ripple * 14; // spacing between ripples
      if (rx >= waveEnd) break;
      int arcH = 6 - ripple;            // arcs get smaller as they travel
      if (arcH < 2) arcH = 2;
      // Draw a small vertical arc (like a ")" shape)
      for (int dy = -arcH; dy <= arcH; dy++) {
        int py = waveMidY + dy;
        if (py < 1 || py > 18) continue; // stay inside header
        // x offset curves outward at center, creating ")" shape
        int dx = (int)(0.5 + (float)(arcH * arcH - dy * dy) / (float)(arcH > 0 ? arcH : 1));
        int px = rx + dx;
        if (px >= waveStart && px < waveEnd) {
          display.drawPixel(px, py, GxEPD_WHITE);
          // thicken the center of the arc for visibility
          if (abs(dy) <= arcH / 2 && px + 1 < waveEnd) {
            display.drawPixel(px + 1, py, GxEPD_WHITE);
          }
        }
      }
    }

    // === App name bar ===
    display.fillRect(0, 22, 200, 18, GxEPD_BLACK);
    display.setFont(&FreeSansBold9pt7b);
    display.setTextColor(GxEPD_WHITE);
    display.setCursor(4, 36);
    char appTrunc[22];
    strncpy(appTrunc, n->app, 21);
    appTrunc[21] = '\0';
    display.print(appTrunc);

    // === Word-wrap: sanitize text, then draw lines that fit maxWidth ===
    int maxWidth = 192;  // 200 - 4px margin each side

    // Sanitize + wrap a string onto the display. Returns lines drawn.
    // Adds "..." on the last line if text is truncated.
    auto drawWrapped = [&](const char* src, int startY, int lineH, int maxL) -> int {
      // Copy and replace control chars with spaces
      char buf[100];
      strncpy(buf, src, 99);
      buf[99] = '\0';
      for (int i = 0; buf[i]; i++) {
        if (buf[i] < ' ') buf[i] = ' ';  // replace all control chars
      }
      int tLen = strlen(buf);
      int pos = 0;
      int drawn = 0;

      for (int line = 0; line < maxL && pos < tLen; line++) {
        // Don't draw if this line's Y would go past safe bottom (180px)
        int curY = startY + (line * lineH);
        if (curY > 180) break;

        // Measure chars one-by-one to find how many fit
        int fitEnd = pos;  // exclusive: buf[pos..fitEnd) fits
        int lastSpace = -1;
        for (int c = pos; c < tLen; c++) {
          char tmp[100];
          int len = c - pos + 1;
          if (len >= 99) break;
          memcpy(tmp, &buf[pos], len);
          tmp[len] = '\0';
          int16_t bx1, by1; uint16_t bw1, bh1;
          display.getTextBounds(tmp, 0, 0, &bx1, &by1, &bw1, &bh1);
          if ((int)bw1 > maxWidth) break;
          fitEnd = c + 1;
          if (buf[c] == ' ') lastSpace = c;
        }
        // Decide where to break
        int lineEnd;
        if (fitEnd >= tLen) {
          lineEnd = tLen;                              // everything fits
        } else if (lastSpace > pos) {
          lineEnd = lastSpace;                         // break at word boundary
        } else {
          lineEnd = fitEnd > pos ? fitEnd : pos + 1;   // force progress
        }

        // Check if there's remaining text after this line
        int nextPos = lineEnd;
        while (nextPos < tLen && buf[nextPos] == ' ') nextPos++;
        bool moreText = (nextPos < tLen);
        bool isLastLine = (line == maxL - 1) || (curY + lineH > 180);
        bool needEllipsis = isLastLine && moreText;

        // Build the line string
        char lineBuf[104];
        int len = lineEnd - pos;
        memcpy(lineBuf, &buf[pos], len);
        lineBuf[len] = '\0';

        // Add "..." if this is the last line and text was truncated
        if (needEllipsis) {
          while (len > 0) {
            char test[104];
            memcpy(test, &buf[pos], len);
            memcpy(test + len, "...", 4);  // includes null terminator
            int16_t tx, ty; uint16_t tw, th;
            display.getTextBounds(test, 0, 0, &tx, &ty, &tw, &th);
            if ((int)tw <= maxWidth) break;
            len--;
          }
          memcpy(lineBuf, &buf[pos], len);
          memcpy(lineBuf + len, "...", 4);
        }

        display.setCursor(4, curY);
        display.print(lineBuf);
        drawn++;
        // Advance past the break + any trailing spaces
        pos = lineEnd;
        while (pos < tLen && buf[pos] == ' ') pos++;
      }
      return drawn;
    };

    // === Title (bold, max 2 lines, 18px spacing) ===
    display.setFont(&FreeSansBold9pt7b);
    display.setTextColor(GxEPD_BLACK);
    int titleLines = drawWrapped(n->title, 58, 18, 2);

    // === Message (regular, fill remaining space, 18px spacing) ===
    display.setFont(&FreeSans9pt7b);
    // Push message start down based on how many title lines were drawn
    int msgStartY = 58 + (titleLines * 18) + 4;  // 4px gap after last title line
    int maxLines = (184 - msgStartY) / 18;
    if (maxLines < 1) maxLines = 1;
    if (maxLines > 4) maxLines = 4;
    drawWrapped(n->message, msgStartY, 18, maxLines);

  } while (display.nextPage());

  statsViewed++;
  isDisplayBusy = false;
}

// ============================================================
// NOTIFICATION MANAGEMENT
// ============================================================

void addNotification(uint32_t uid, const char* app, const char* title, const char* msg) {
  if (isDisplayBusy) {
    if (queueCount < NOTIF_QUEUE_SIZE) {
      QueuedNotif* q = &notifQueue[queueCount++];
      q->uid = uid;
      strncpy(q->app, app, 31); q->app[31] = '\0';
      strncpy(q->title, title, 47); q->title[47] = '\0';
      strncpy(q->message, msg, 95); q->message[95] = '\0';
      q->valid = true;
    }
    return;
  }

  for (int i = MAX_NOTIFICATIONS - 1; i > 0; i--) {
    notifications[i] = notifications[i - 1];
  }

  Notification* n = &notifications[0];
  n->uid = uid;
  strncpy(n->app, app, 31); n->app[31] = '\0';
  strncpy(n->title, title, 47); n->title[47] = '\0';
  strncpy(n->message, msg, 95); n->message[95] = '\0';
  n->timestamp = millis();
  n->valid = true;

  if (notificationCount < MAX_NOTIFICATIONS) notificationCount++;
  currentPage = 0;

  Serial.printf("Stored %d/%d (UID: %lu)\n", notificationCount, MAX_NOTIFICATIONS, uid);
}

bool removeNotificationByUID(uint32_t uid) {
  bool found = false;
  for (int i = 0; i < MAX_NOTIFICATIONS; i++) {
    if (notifications[i].valid && notifications[i].uid == uid) {
      notifications[i].valid = false;
      found = true;
      break;
    }
  }
  if (!found) return false;

  int write = 0;
  for (int read = 0; read < MAX_NOTIFICATIONS; read++) {
    if (notifications[read].valid) {
      if (write != read) notifications[write] = notifications[read];
      write++;
    }
  }
  for (int i = write; i < MAX_NOTIFICATIONS; i++) {
    notifications[i].valid = false;
  }
  notificationCount = write;

  if (currentPage >= notificationCount && notificationCount > 0) {
    currentPage = notificationCount - 1;
  } else if (notificationCount == 0) {
    currentPage = 0;
  }

  return true;
}

void dismissCurrentNotification() {
  if (currentPage < notificationCount && notifications[currentPage].valid) {
    notifications[currentPage].valid = false;
    statsDismissed++;

    int write = 0;
    for (int i = 0; i < MAX_NOTIFICATIONS; i++) {
      if (notifications[i].valid) {
        if (write != i) notifications[write] = notifications[i];
        write++;
      }
    }
    for (int i = write; i < MAX_NOTIFICATIONS; i++) {
      notifications[i].valid = false;
    }
    notificationCount = write;
  }

  if (currentPage >= notificationCount && notificationCount > 0) {
    currentPage = notificationCount - 1;
  } else if (notificationCount == 0) {
    currentPage = 0;
  }

  needsDisplayUpdate = true;
}

void dismissAllNotifications() {
  for (int i = 0; i < MAX_NOTIFICATIONS; i++) {
    if (notifications[i].valid) statsDismissed++;
    notifications[i].valid = false;
  }
  notificationCount = 0;
  currentPage = 0;

  needsDisplayUpdate = true;
}

// ============================================================
// MAIN LOOP
// ============================================================

void loop() {
  // ANCS discovery after bonding
  if (ancsDiscoveryNeeded && pAncsClient) {
    ancsDiscoveryNeeded = false;
    delay(500);  // Give iOS a moment after bonding
    ancsSubscribed = discoverAndSubscribeANCS();
    if (ancsSubscribed) {
      ancsSubscribedAt = millis();
      Serial.println("ANCS ready! Waiting for notifications...");
      drawReadyScreen();
    } else {
      Serial.println("ANCS setup failed.");
    }
  }

  // Request notification details when flagged
  if (ancsSubscribed && pendingNotification && pControlPointCharacteristic) {
    Serial.println("Requesting notification details...");
    uint8_t cmd[] = {
      0x00,
      latestMessageID[0], latestMessageID[1],
      latestMessageID[2], latestMessageID[3],
      0x00, 0x3F, 0x00,   // Attr 0: App Identifier, max 63
      0x01, 0x2F, 0x00,   // Attr 1: Title, max 47
      0x02, 0x2F, 0x00,   // Attr 2: Subtitle, max 47
      0x03, 0x5F, 0x00,   // Attr 3: Message, max 95
      0x05                 // Attr 5: Date
    };
    pControlPointCharacteristic->writeValue(cmd, sizeof(cmd), true);
    pendingNotification = false;
  }

  if (needsDisplayUpdate && !isDisplayBusy && bleConnected) {
    needsDisplayUpdate = false;

    if (notificationCount == 0) {
      drawReadyScreen();
    } else {
      drawNotifications();
    }
  }

  if (!isDisplayBusy && queueCount > 0) {
    for (int i = 0; i < queueCount; i++) {
      if (notifQueue[i].valid) {
        addNotification(notifQueue[i].uid, notifQueue[i].app,
                        notifQueue[i].title, notifQueue[i].message);
        notifQueue[i].valid = false;
      }
    }
    queueCount = 0;
    needsDisplayUpdate = true;
  }

  // === Button A: single button with multi-press detection ===
  //   short press  = dismiss current notification
  //   long press   = Siri (reserved for future use)
  //   double press = dismiss all
  bool aPressed = (digitalRead(BUTTON_A_PIN) == LOW);

  if (aPressed && !buttonAPressed) {
    // Button just pressed down
    buttonAPressStart = millis();
    buttonAPressed = true;
    buttonALongTriggered = false;
  } else if (aPressed && buttonAPressed && !buttonALongTriggered) {
    // Button held — check for long press
    if (millis() - buttonAPressStart > LONG_PRESS_MS) {
      Serial.println("Button A long: Trigger Siri (reserved)");
      // TODO: HFP Siri trigger
      buttonALongTriggered = true;
      buttonAPressCount = 0;  // Long press cancels any multi-press sequence
    }
  } else if (!aPressed && buttonAPressed) {
    // Button just released
    if (!buttonALongTriggered && (millis() - buttonAPressStart > BUTTON_DEBOUNCE_MS)) {
      buttonAPressCount++;
      buttonALastRelease = millis();
      Serial.printf("Button release #%d\n", buttonAPressCount);
    }
    buttonAPressed = false;
  }

  // Execute multi-press action after the window expires
  if (buttonAPressCount > 0 && !buttonAPressed &&
      (millis() - buttonALastRelease > MULTI_PRESS_WINDOW_MS)) {
    switch (buttonAPressCount) {
      case 1:
        if (notificationCount > 0) {
          Serial.println("Button A x1: Dismiss current");
          dismissCurrentNotification();
        } else if (bleConnected) {
          Serial.println("Button A x1: Connection check");
          isDisplayBusy = true;
          display.setFullWindow();
          display.firstPage();
          do {
            display.fillScreen(GxEPD_WHITE);
            display.setFont(&FreeSansBold9pt7b);
            display.setTextColor(GxEPD_BLACK);

            if (rtcSynced) {
              // Show current time
              int h, m, s;
              if (rtcRead(h, m, s)) {
                char timeBuf[12];
                // 12-hour format
                int h12 = h % 12;
                if (h12 == 0) h12 = 12;
                const char* ampm = (h < 12) ? "AM" : "PM";
                snprintf(timeBuf, sizeof(timeBuf), "%d:%02d %s", h12, m, ampm);
                // Center the time string
                int16_t tx, ty; uint16_t tw, th;
                display.getTextBounds(timeBuf, 0, 0, &tx, &ty, &tw, &th);
                display.setCursor((200 - tw) / 2, 90);
                display.print(timeBuf);
              }
              // "Still connected" below, centered
              display.setFont(&FreeSans9pt7b);
              int16_t sx, sy; uint16_t sw, sh;
              display.getTextBounds("Still connected", 0, 0, &sx, &sy, &sw, &sh);
              display.setCursor((200 - sw) / 2, 120);
              display.print("Still connected");
            } else {
              int16_t sx2, sy2; uint16_t sw2, sh2;
              display.getTextBounds("Still connected!", 0, 0, &sx2, &sy2, &sw2, &sh2);
              display.setCursor((200 - sw2) / 2, 100);
              display.print("Still connected!");
            }
          } while (display.nextPage());
          isDisplayBusy = false;
          delay(2000);
          drawReadyScreen();
        }
        break;
      default:  // 2+
        Serial.println("Button A x2: Dismiss all");
        dismissAllNotifications();
        break;
    }
    buttonAPressCount = 0;
  }

  // === PWR button: long press (2s) to power off ===
  bool pwrPressed = (digitalRead(PWR_BUTTON_PIN) == LOW);
  if (pwrPressed && !pwrButtonPressed) {
    pwrButtonPressStart = millis();
    pwrButtonPressed = true;
  } else if (pwrPressed && pwrButtonPressed) {
    if (millis() - pwrButtonPressStart > PWR_OFF_HOLD_MS) {
      Serial.println("PWR long press: shutting down...");
      // Show shutdown message
      display.setFullWindow();
      display.firstPage();
      do {
        display.fillScreen(GxEPD_WHITE);
        display.setFont(&FreeSansBold9pt7b);
        display.setTextColor(GxEPD_BLACK);
        int16_t px, py; uint16_t pw, ph;
        display.getTextBounds("Power off", 0, 0, &px, &py, &pw, &ph);
        display.setCursor((200 - pw) / 2, 100);
        display.print("Power off");
      } while (display.nextPage());
      delay(500);
      // Cut power by releasing the latch
      digitalWrite(GPIO_NUM_17, LOW);
      // If still running (e.g. USB powered), enter deep sleep
      esp_deep_sleep_start();
    }
  } else if (!pwrPressed && pwrButtonPressed) {
    pwrButtonPressed = false;
  }

  delay(10);
}