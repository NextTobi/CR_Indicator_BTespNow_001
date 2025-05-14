/**
 * ESP32 ESP-NOW LED Indicator System - INDICATOR (RECEIVER) CODE
 * Version: 7.1 - Reliable Light Sleep Implementation
 * 
 * This implementation uses light sleep for maximum power efficiency
 * while maintaining reliable ESP-NOW communication.
 */

#include <Arduino.h>
#include <esp_now.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <Preferences.h>
#include <esp_sleep.h>

// Configuration constants
const int NUM_LEDS = 3;
const int LED_PINS[NUM_LEDS] = {25, 26, 27};  // GPIO pins for the LEDs (active LOW)
const int WIFI_CHANNEL = 6;                   // WiFi channel for ESP-NOW communication
const char* PREF_NAMESPACE = "espnow-leds";

// Sleep and timing control
const int AWAKE_TIME_MS = 300;        // 300ms awake time
const int SLEEP_DURATION_MS = 1700;   // 1.7 seconds sleep time
const int AWAKE_AFTER_COMMAND_MS = 3000;  // Stay awake for 3 seconds after command

// State tracking variables
unsigned long lastCommandTime = 0;
unsigned long lastStatusTime = 0;
unsigned long nextSleepTime = 0;  // Timestamp for when to enter next sleep cycle
int consecutiveSleepCycles = 0;
const int MAX_SLEEP_CYCLES = 10;  // Force a long awake period after this many sleep cycles
bool forceExtendedAwake = false;  // Flag to enforce extended awake period

// Message types for communication protocol
enum MessageType {
  LED_COMMAND = 1,
  ACKNOWLEDGMENT = 2,
  DISCOVERY = 3
};

// ESP-NOW message structure
typedef struct {
  uint8_t type;     // Message type
  uint8_t value;    // LED index or acknowledgment value
} message_t;

// Global variables
Preferences preferences;
int activeLedIndex = -1;
uint8_t lastSenderMac[6] = {0};
bool sendDiscoveryResponse = false;

// Function prototypes
void runLedTest();
void setupEspNow();
void reinitEspNowAfterSleep();
bool loadSavedAddresses();
void savePeerAddress(const uint8_t *addr);
void printMacAddress(const uint8_t *addr);
void onDataReceived(const uint8_t *macAddr, const uint8_t *data, int dataLen);
void handleLedCommand(uint8_t ledIndex, const uint8_t *senderAddr);
void sendAcknowledgment(const uint8_t *addr);
void handleDiscoveryResponse();
void prepareForSleep();
void handleWakeup();
void printStatusUpdate();

void setup() {
  Serial.begin(115200);
  delay(500); // Short delay for serial to initialize
  
  // Set CPU frequency to 80MHz for power efficiency
  setCpuFrequencyMhz(80);
  
  Serial.print("\n\n==== ESP32 ESP-NOW LED System ====\n");
  Serial.println("INDICATOR MODE (RECEIVER)");
  Serial.println("FW Version: 7.1 - Reliable Light Sleep Implementation");
  
  // Initialize LED pins
  for (int i = 0; i < NUM_LEDS; i++) {
    pinMode(LED_PINS[i], OUTPUT);
    digitalWrite(LED_PINS[i], HIGH);  // LEDs are OFF initially (active LOW)
  }
  
  // Run LED test
  runLedTest();
  
  // Initialize preferences
  preferences.begin(PREF_NAMESPACE, false);
  
  // Load saved addresses
  if (loadSavedAddresses()) {
    Serial.println("Loaded saved peer address:");
    printMacAddress(lastSenderMac);
  } else {
    Serial.println("No saved peer address found.");
  }
  
  // Setup ESP-NOW
  setupEspNow();
  
  Serial.println("Indicator ready - using optimized light sleep");
  Serial.printf("Sleep pattern: %dms awake, %dms sleep\n", 
                AWAKE_TIME_MS, SLEEP_DURATION_MS);
  
  lastStatusTime = millis();
  lastCommandTime = millis(); // Start with active state
  nextSleepTime = millis() + AWAKE_AFTER_COMMAND_MS; // Set initial sleep time
}

void loop() {
  unsigned long currentTime = millis();
  
  // Handle any pending discovery responses
  if (sendDiscoveryResponse) {
    handleDiscoveryResponse();
  }
  
  // Print status update periodically
  if (currentTime - lastStatusTime >= 10000) {
    printStatusUpdate();
    lastStatusTime = currentTime;
  }
  
  // Ensure LED is correctly set
  if (activeLedIndex >= 0) {
    digitalWrite(LED_PINS[activeLedIndex], LOW);  // Ensure LED stays ON
  }
  
  // Determine if we should stay awake or enter sleep
  bool shouldSleep = false;
  
  // After receiving a command, stay awake for defined period
  if (currentTime - lastCommandTime < AWAKE_AFTER_COMMAND_MS) {
    // Actively scanning mode after receiving a command
    if (currentTime % 1000 < 10) { // Print only occasionally to reduce log spam
      Serial.println("Active scanning after command");
    }
    nextSleepTime = lastCommandTime + AWAKE_AFTER_COMMAND_MS;
    consecutiveSleepCycles = 0;
    
  } else if (forceExtendedAwake) {
    // We're in a forced extended awake period
    if (currentTime - lastStatusTime >= 5000) {
      Serial.println("Extended awake period to ensure communication");
      lastStatusTime = currentTime;
      
      // End extended awake period after 10 seconds
      if (currentTime - lastCommandTime >= 10000) {
        Serial.println("Ending extended awake period");
        forceExtendedAwake = false;
        consecutiveSleepCycles = 0;
        nextSleepTime = currentTime + 100; // Enter sleep soon
      }
    }
    
  } else if (currentTime >= nextSleepTime) {
    // Time to enter a sleep cycle
    shouldSleep = true;
  }
  
  // Enter sleep if conditions are met
  if (shouldSleep) {
    // Scan briefly before sleep
    Serial.println("Scanning briefly before sleep");
    delay(AWAKE_TIME_MS); // Brief scanning period
    
    // Enter sleep
    prepareForSleep();
    esp_light_sleep_start();
    
    // After wakeup
    handleWakeup();
    
    // Update sleep cycle tracking
    consecutiveSleepCycles++;
    
    // Check if we need to force an extended awake period
    if (consecutiveSleepCycles >= MAX_SLEEP_CYCLES) {
      Serial.println("Forcing extended awake period after multiple sleep cycles");
      forceExtendedAwake = true;
      consecutiveSleepCycles = 0;
    } else {
      // Schedule next sleep
      nextSleepTime = millis() + AWAKE_TIME_MS;
    }
  } else {
    // In active scanning mode, just a short delay
    delay(10);
  }
}

void prepareForSleep() {
  Serial.printf("Entering light sleep for %d ms\n", SLEEP_DURATION_MS);
  Serial.flush(); // Ensure all data is sent before sleep
  
  // Configure light sleep
  esp_sleep_enable_timer_wakeup(SLEEP_DURATION_MS * 1000); // Convert to microseconds
  
  // Hold GPIO state for LED
  if (activeLedIndex >= 0) {
    gpio_hold_en((gpio_num_t)LED_PINS[activeLedIndex]);
    gpio_deep_sleep_hold_en(); // Enable GPIO hold during sleep
  }
}

void handleWakeup() {
  Serial.println("Woke up from light sleep");
  
  // Disable GPIO hold
  gpio_hold_dis((gpio_num_t)LED_PINS[0]);
  gpio_hold_dis((gpio_num_t)LED_PINS[1]);
  gpio_hold_dis((gpio_num_t)LED_PINS[2]);
  gpio_deep_sleep_hold_dis();
  
  // Ensure LED state is maintained after wakeup
  if (activeLedIndex >= 0) {
    digitalWrite(LED_PINS[activeLedIndex], LOW);
  }
  
  // Reinitialize ESP-NOW after sleep
  reinitEspNowAfterSleep();
}

void printStatusUpdate() {
  Serial.println("\n--- STATUS UPDATE ---");
  if (activeLedIndex >= 0) {
    Serial.printf("Current active LED: %d (pin: %d)\n", 
                  activeLedIndex, LED_PINS[activeLedIndex]);
  } else {
    Serial.println("No active LED");
  }
  
  Serial.printf("Time since last command: %.2f seconds\n", 
                (millis() - lastCommandTime) / 1000.0);
  Serial.printf("Consecutive sleep cycles: %d\n", consecutiveSleepCycles);
  Serial.printf("Current mode: %s\n", 
                forceExtendedAwake ? "Extended awake" : 
                ((millis() - lastCommandTime < AWAKE_AFTER_COMMAND_MS) ? 
                 "Post-command scanning" : "Normal sleep cycle"));
  Serial.printf("MAC Address: %s\n", WiFi.macAddress().c_str());
  Serial.printf("WiFi channel: %d\n", WIFI_CHANNEL);
  Serial.println("---------------------");
}

void runLedTest() {
  Serial.println("Running LED test sequence");
  
  // Turn on each LED in sequence
  for (int i = 0; i < NUM_LEDS; i++) {
    digitalWrite(LED_PINS[i], LOW);   // Turn ON (active LOW)
    delay(300);
    digitalWrite(LED_PINS[i], HIGH);  // Turn OFF
    delay(100);
  }
  
  // Flash all LEDs once to confirm end of test
  for (int i = 0; i < NUM_LEDS; i++) {
    digitalWrite(LED_PINS[i], LOW);   // All ON
  }
  delay(300);
  for (int i = 0; i < NUM_LEDS; i++) {
    digitalWrite(LED_PINS[i], HIGH);  // All OFF
  }
  
  Serial.println("LED test complete");
}

void setupEspNow() {
  // Initialize WiFi in Station mode
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(300);
  
  // Set WiFi channel
  esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
  delay(100);
  
  // Initialize ESP-NOW
  esp_err_t result = esp_now_init();
  if (result != ESP_OK) {
    Serial.printf("Error initializing ESP-NOW: %d\n", result);
    delay(3000);
    ESP.restart();
    return;
  }
  
  // Register receive callback
  esp_now_register_recv_cb(onDataReceived);
  
  Serial.printf("Device MAC Address: %s\n", WiFi.macAddress().c_str());
  Serial.printf("Operating on WiFi channel: %d\n", WIFI_CHANNEL);
}

void reinitEspNowAfterSleep() {
  // De-initialize ESP-NOW
  esp_now_deinit();
  
  // Wait a bit to ensure clean state
  delay(20);
  
  // Reinitialize WiFi
  WiFi.disconnect();
  WiFi.mode(WIFI_STA);
  delay(20);
  
  // Set WiFi channel
  esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
  delay(20);
  
  // Initialize ESP-NOW
  esp_err_t result = esp_now_init();
  if (result != ESP_OK) {
    Serial.printf("Error reinitializing ESP-NOW: %d\n", result);
    return;
  }
  
  // Register receive callback
  esp_now_register_recv_cb(onDataReceived);
  
  // Re-add peer if we have one
  if (lastSenderMac[0] != 0 || lastSenderMac[1] != 0) {
    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, lastSenderMac, 6);
    peerInfo.channel = WIFI_CHANNEL;
    peerInfo.encrypt = false;
    esp_now_add_peer(&peerInfo);
  }
  
  Serial.println("ESP-NOW reinitialized after sleep");
}

bool loadSavedAddresses() {
  size_t macLen = preferences.getBytesLength("last_sender");
  if (macLen == 6) {
    preferences.getBytes("last_sender", lastSenderMac, 6);
    return true;
  }
  return false;
}

void savePeerAddress(const uint8_t *addr) {
  preferences.putBytes("last_sender", addr, 6);
  memcpy(lastSenderMac, addr, 6);
  Serial.println("Saved peer MAC address");
}

void printMacAddress(const uint8_t *addr) {
  char macStr[18];
  snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
           addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
  Serial.println(macStr);
}

void onDataReceived(const uint8_t *macAddr, const uint8_t *data, int dataLen) {
  // Print who sent this data
  char macStr[18];
  snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
           macAddr[0], macAddr[1], macAddr[2], macAddr[3], macAddr[4], macAddr[5]);
  
  Serial.print("Received data from: ");
  Serial.println(macStr);
  
  // Process only if data length matches our message structure
  if (dataLen == sizeof(message_t)) {
    message_t *message = (message_t *)data;
    
    // Save the sender's address for potential responses
    memcpy(lastSenderMac, macAddr, 6);
    
    switch (message->type) {
      case LED_COMMAND: {
        Serial.printf("Received LED command: %d\n", message->value);
        // Update last command time and reset counter
        lastCommandTime = millis();
        consecutiveSleepCycles = 0;
        forceExtendedAwake = false;  // Cancel any forced awake period
        handleLedCommand(message->value, macAddr);
        break;
      }
        
      case DISCOVERY: {
        Serial.println("Received discovery request");
        savePeerAddress(macAddr);
        sendDiscoveryResponse = true;
        lastCommandTime = millis();
        consecutiveSleepCycles = 0;
        forceExtendedAwake = false;  // Cancel any forced awake period
        break;
      }
        
      default: {
        Serial.printf("Unknown message type: %d\n", message->type);
        break;
      }
    }
  }
}

void handleLedCommand(uint8_t ledIndex, const uint8_t *senderAddr) {
  // Validate LED index
  if (ledIndex >= NUM_LEDS) {
    Serial.println("Invalid LED index received");
    return;
  }
  
  // Turn off current active LED if any
  if (activeLedIndex >= 0) {
    digitalWrite(LED_PINS[activeLedIndex], HIGH);  // Turn OFF
  }
  
  // Turn on new LED
  digitalWrite(LED_PINS[ledIndex], LOW);  // Turn ON (active LOW)
  activeLedIndex = ledIndex;
  
  Serial.printf("Activated LED on pin: %d\n", LED_PINS[ledIndex]);
  
  // Send acknowledgment back to sender
  sendAcknowledgment(senderAddr);
}

void handleDiscoveryResponse() {
  message_t message;
  message.type = DISCOVERY;
  message.value = 0;
  
  // Create or update the peer
  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, lastSenderMac, 6);
  peerInfo.channel = WIFI_CHANNEL;
  peerInfo.encrypt = false;
  
  // More reliable peer management - check before deleting
  if (esp_now_is_peer_exist(lastSenderMac)) {
    esp_now_del_peer(lastSenderMac);
  }
  
  // Add with error checking
  esp_err_t result = esp_now_add_peer(&peerInfo);
  if (result != ESP_OK) {
    // Retry once more if failed
    delay(10);
    result = esp_now_add_peer(&peerInfo);
  }
  
  if (result != ESP_OK) {
    Serial.printf("Peer management error: %d\n", result);
  } else {
    // Send discovery response
    result = esp_now_send(lastSenderMac, (uint8_t *)&message, sizeof(message));
    Serial.printf("Discovery response status: %s\n", 
                  (result == ESP_OK) ? "Success" : "Failed");
  }
  
  sendDiscoveryResponse = false;
}

void sendAcknowledgment(const uint8_t *addr) {
  // Create peer info structure
  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, addr, 6);
  peerInfo.channel = WIFI_CHANNEL;
  peerInfo.encrypt = false;
  
  // More reliable peer management - check before deleting
  if (esp_now_is_peer_exist(addr)) {
    esp_now_del_peer(addr);
  }
  
  // Add with error checking
  esp_err_t result = esp_now_add_peer(&peerInfo);
  if (result != ESP_OK) {
    // Retry once more if failed
    delay(10);
    result = esp_now_add_peer(&peerInfo);
  }
  
  if (result != ESP_OK) {
    Serial.printf("Peer management error: %d\n", result);
    return;
  }
  
  // Send multiple acknowledgments for redundancy
  bool success = false;
  for (int i = 0; i < 3; i++) {  // Reduced to 3 for efficiency
    message_t message;
    message.type = ACKNOWLEDGMENT;
    message.value = activeLedIndex;
    
    result = esp_now_send(addr, (uint8_t *)&message, sizeof(message));
    if (result == ESP_OK) {
      Serial.printf("Acknowledgment %d sent successfully\n", i + 1);
      success = true;
    } else {
      Serial.printf("Error on attempt %d: %d\n", i + 1, result);
    }
    delay(20);  // Reduced delay for faster acknowledgment
  }
  
  Serial.printf("Completed acknowledgments for LED index: %d\n", activeLedIndex);
}