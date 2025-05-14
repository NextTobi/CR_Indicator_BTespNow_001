/**
 * ESP32 ESP-NOW LED Indicator System - INDICATOR (RECEIVER) CODE
 * Version: 7.2 - Reliable Light Sleep Implementation with Non-Blocking Design
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

// State machine states
enum SetupState {
  SETUP_INIT,
  SETUP_SERIAL_WAIT,
  SETUP_LED_TEST,
  SETUP_WIFI_INIT,
  SETUP_WIFI_DISCONNECT_WAIT,
  SETUP_WIFI_CHANNEL_WAIT,
  SETUP_ESPNOW_INIT,
  SETUP_COMPLETE
};

enum LedTestState {
  LED_TEST_INIT,
  LED_TEST_SEQUENCE,
  LED_TEST_ALL_ON,
  LED_TEST_ALL_OFF,
  LED_TEST_COMPLETE
};

enum AckState {
  ACK_INIT,
  ACK_PEER_SETUP,
  ACK_SEND,
  ACK_WAIT,
  ACK_COMPLETE
};

enum DiscoveryState {
  DISCOVERY_INIT,
  DISCOVERY_PEER_SETUP,
  DISCOVERY_SEND,
  DISCOVERY_COMPLETE
};

enum SleepState {
  SLEEP_AWAKE,
  SLEEP_PREPARE,
  SLEEP_ENTER,
  SLEEP_WAKEUP,
  SLEEP_REINIT_START,
  SLEEP_WIFI_DISCONNECT,
  SLEEP_WIFI_SETUP,
  SLEEP_WIFI_WAIT,
  SLEEP_CHANNEL_SETUP,
  SLEEP_CHANNEL_WAIT,
  SLEEP_ESPNOW_INIT,
  SLEEP_ESPNOW_CALLBACK,
  SLEEP_PEER_SETUP,
  SLEEP_COMPLETE
};

// Global variables
Preferences preferences;
int activeLedIndex = -1;
uint8_t lastSenderMac[6] = {0};
bool sendDiscoveryResponse = false;

// State machine variables
SetupState setupState = SETUP_INIT;
LedTestState ledTestState = LED_TEST_INIT;
AckState ackState = ACK_INIT;
DiscoveryState discoveryState = DISCOVERY_INIT;
SleepState sleepState = SLEEP_AWAKE;

unsigned long stateTimer = 0;
int currentTestLed = 0;
int ackAttemptCount = 0;
const uint8_t *ackTargetAddr = NULL;
bool reinitRequired = false;

// Function prototypes
void processLedTest();
bool setupEspNow();
bool reinitEspNowAfterSleep();
bool loadSavedAddresses();
void savePeerAddress(const uint8_t *addr);
void printMacAddress(const uint8_t *addr);
void onDataReceived(const uint8_t *macAddr, const uint8_t *data, int dataLen);
void handleLedCommand(uint8_t ledIndex, const uint8_t *senderAddr);
void processAcknowledgment();
void processDiscoveryResponse();
void processSleepWakeup();
void printStatusUpdate();

void setup() {
  Serial.begin(115200);
  
  // Set CPU frequency to 80MHz for power efficiency
  setCpuFrequencyMhz(80);
  
  // Initialize setup state machine
  setupState = SETUP_SERIAL_WAIT;
  stateTimer = millis();
  
  // Initialize preferences
  preferences.begin(PREF_NAMESPACE, false);
}

void loop() {
  unsigned long currentTime = millis();
  
  // Handle setup state machine
  if (setupState != SETUP_COMPLETE) {
    switch (setupState) {
      case SETUP_SERIAL_WAIT:
        if (currentTime - stateTimer >= 500) {
          Serial.print("\n\n==== ESP32 ESP-NOW LED System ====\n");
          Serial.println("INDICATOR MODE (RECEIVER)");
          Serial.println("FW Version: 7.2 - Reliable Light Sleep Implementation with Non-Blocking Design");
          
          // Initialize LED pins
          for (int i = 0; i < NUM_LEDS; i++) {
            pinMode(LED_PINS[i], OUTPUT);
            digitalWrite(LED_PINS[i], HIGH);  // LEDs are OFF initially (active LOW)
          }
          
          ledTestState = LED_TEST_INIT;
          setupState = SETUP_LED_TEST;
        }
        break;
        
      case SETUP_LED_TEST:
        processLedTest();
        if (ledTestState == LED_TEST_COMPLETE) {
          // Load saved addresses
          if (loadSavedAddresses()) {
            Serial.println("Loaded saved peer address:");
            printMacAddress(lastSenderMac);
          } else {
            Serial.println("No saved peer address found.");
          }
          
          setupState = SETUP_WIFI_INIT;
        }
        break;
        
      case SETUP_WIFI_INIT:
        // Initialize WiFi in Station mode
        WiFi.mode(WIFI_STA);
        WiFi.disconnect();
        stateTimer = currentTime;
        setupState = SETUP_WIFI_DISCONNECT_WAIT;
        break;
        
      case SETUP_WIFI_DISCONNECT_WAIT:
        if (currentTime - stateTimer >= 300) {
          // Set WiFi channel
          esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
          stateTimer = currentTime;
          setupState = SETUP_WIFI_CHANNEL_WAIT;
        }
        break;
        
      case SETUP_WIFI_CHANNEL_WAIT:
        if (currentTime - stateTimer >= 100) {
          setupState = SETUP_ESPNOW_INIT;
        }
        break;
        
      case SETUP_ESPNOW_INIT:
        // Initialize ESP-NOW
        esp_err_t result = esp_now_init();
        if (result != ESP_OK) {
          Serial.printf("Error initializing ESP-NOW: %d\n", result);
          stateTimer = currentTime;
          ESP.restart(); // Restart ESP32 if initialization fails
          return;
        }
        
        // Register receive callback
        esp_now_register_recv_cb(onDataReceived);
        
        Serial.printf("Device MAC Address: %s\n", WiFi.macAddress().c_str());
        Serial.printf("Operating on WiFi channel: %d\n", WIFI_CHANNEL);
        
        Serial.println("Indicator ready - using optimized light sleep");
        Serial.printf("Sleep pattern: %dms awake, %dms sleep\n", 
                      AWAKE_TIME_MS, SLEEP_DURATION_MS);
        
        lastStatusTime = currentTime;
        lastCommandTime = currentTime; // Start with active state
        nextSleepTime = currentTime + AWAKE_AFTER_COMMAND_MS; // Set initial sleep time
        
        setupState = SETUP_COMPLETE;
        break;
    }
    
    return; // Don't process the rest of the loop until setup is complete
  }
  
  // Process acknowledgment if needed
  if (ackState != ACK_INIT && ackState != ACK_COMPLETE) {
    processAcknowledgment();
  }
  
  // Process discovery response if needed
  if (sendDiscoveryResponse && discoveryState == DISCOVERY_INIT) {
    discoveryState = DISCOVERY_PEER_SETUP;
    stateTimer = currentTime;
  }
  
  if (discoveryState != DISCOVERY_INIT && discoveryState != DISCOVERY_COMPLETE) {
    processDiscoveryResponse();
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
  bool shouldPrepareSleep = false;
  
  // After receiving a command, stay awake for defined period
  if (currentTime - lastCommandTime < AWAKE_AFTER_COMMAND_MS) {
    // Actively scanning mode after receiving a command
    if (currentTime % 1000 < 10) { // Print only occasionally to reduce log spam
      Serial.println("Active scanning after command");
    }
    nextSleepTime = lastCommandTime + AWAKE_AFTER_COMMAND_MS;
    consecutiveSleepCycles = 0;
    sleepState = SLEEP_AWAKE;
    
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
    sleepState = SLEEP_AWAKE;
    
  } else if (currentTime >= nextSleepTime && sleepState == SLEEP_AWAKE) {
    // Time to enter a sleep cycle
    shouldPrepareSleep = true;
  }
  
  // Process sleep/wakeup state machine
  if (shouldPrepareSleep) {
    sleepState = SLEEP_PREPARE;
    stateTimer = currentTime;
    Serial.println("Scanning briefly before sleep");
  }
  
  // Process sleep state machine if not in AWAKE state
  if (sleepState != SLEEP_AWAKE) {
    processSleepWakeup();
  }
}

void processLedTest() {
  static unsigned long ledTimer = 0;
  unsigned long currentTime = millis();
  
  switch (ledTestState) {
    case LED_TEST_INIT:
      Serial.println("Running LED test sequence");
      currentTestLed = 0;
      ledTimer = currentTime;
      ledTestState = LED_TEST_SEQUENCE;
      break;
      
    case LED_TEST_SEQUENCE:
      if (currentTime - ledTimer < 300) {
        // LED on for 300ms
        digitalWrite(LED_PINS[currentTestLed], LOW);
      } else if (currentTime - ledTimer < 400) {
        // LED off for 100ms
        digitalWrite(LED_PINS[currentTestLed], HIGH);
      } else {
        // Move to next LED
        currentTestLed++;
        if (currentTestLed >= NUM_LEDS) {
          ledTestState = LED_TEST_ALL_ON;
        } else {
          ledTimer = currentTime;
        }
      }
      break;
      
    case LED_TEST_ALL_ON:
      // Turn all LEDs on
      for (int i = 0; i < NUM_LEDS; i++) {
        digitalWrite(LED_PINS[i], LOW);
      }
      ledTimer = currentTime;
      ledTestState = LED_TEST_ALL_OFF;
      break;
      
    case LED_TEST_ALL_OFF:
      if (currentTime - ledTimer >= 300) {
        // Turn all LEDs off after 300ms
        for (int i = 0; i < NUM_LEDS; i++) {
          digitalWrite(LED_PINS[i], HIGH);
        }
        ledTestState = LED_TEST_COMPLETE;
        Serial.println("LED test complete");
      }
      break;
      
    case LED_TEST_COMPLETE:
      // Nothing to do, test is complete
      break;
  }
}

bool setupEspNow() {
  // This function is now handled by the setup state machine
  return true;
}

void processSleepWakeup() {
  unsigned long currentTime = millis();
  
  switch (sleepState) {
    case SLEEP_PREPARE:
      if (currentTime - stateTimer >= AWAKE_TIME_MS) {
        // After brief scanning period, enter sleep
        Serial.printf("Entering light sleep for %d ms\n", SLEEP_DURATION_MS);
        Serial.flush(); // Ensure all data is sent before sleep
        
        // Configure light sleep
        esp_sleep_enable_timer_wakeup(SLEEP_DURATION_MS * 1000); // Convert to microseconds
        
        // Hold GPIO state for LED
        if (activeLedIndex >= 0) {
          gpio_hold_en((gpio_num_t)LED_PINS[activeLedIndex]);
          gpio_deep_sleep_hold_en(); // Enable GPIO hold during sleep
        }
        
        sleepState = SLEEP_ENTER;
      }
      break;
      
    case SLEEP_ENTER:
      esp_light_sleep_start();
      // Code continues here after wakeup
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
      
      sleepState = SLEEP_REINIT_START;
      stateTimer = millis();
      break;
      
    case SLEEP_REINIT_START:
      // De-initialize ESP-NOW
      esp_now_deinit();
      sleepState = SLEEP_WIFI_DISCONNECT;
      stateTimer = millis();
      break;
      
    case SLEEP_WIFI_DISCONNECT:
      if (millis() - stateTimer >= 20) {
        // Reinitialize WiFi
        WiFi.disconnect();
        WiFi.mode(WIFI_STA);
        sleepState = SLEEP_WIFI_SETUP;
        stateTimer = millis();
      }
      break;
      
    case SLEEP_WIFI_SETUP:
      if (millis() - stateTimer >= 20) {
        // Set WiFi channel
        esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
        sleepState = SLEEP_CHANNEL_SETUP;
        stateTimer = millis();
      }
      break;
      
    case SLEEP_CHANNEL_SETUP:
      if (millis() - stateTimer >= 20) {
        // Initialize ESP-NOW
        esp_err_t result = esp_now_init();
        if (result != ESP_OK) {
          Serial.printf("Error reinitializing ESP-NOW: %d\n", result);
          sleepState = SLEEP_COMPLETE; // Skip to complete even on error
        } else {
          sleepState = SLEEP_ESPNOW_CALLBACK;
        }
        stateTimer = millis();
      }
      break;
      
    case SLEEP_ESPNOW_CALLBACK:
      // Register receive callback
      esp_now_register_recv_cb(onDataReceived);
      sleepState = SLEEP_PEER_SETUP;
      stateTimer = millis();
      break;
      
    case SLEEP_PEER_SETUP:
      // Re-add peer if we have one
      if (lastSenderMac[0] != 0 || lastSenderMac[1] != 0) {
        esp_now_peer_info_t peerInfo = {};
        memcpy(peerInfo.peer_addr, lastSenderMac, 6);
        peerInfo.channel = WIFI_CHANNEL;
        peerInfo.encrypt = false;
        esp_now_add_peer(&peerInfo);
      }
      
      Serial.println("ESP-NOW reinitialized after sleep");
      sleepState = SLEEP_COMPLETE;
      break;
      
    case SLEEP_COMPLETE:
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
      
      sleepState = SLEEP_AWAKE;
      break;
  }
}

bool reinitEspNowAfterSleep() {
  // This is now handled by the sleep state machine
  return true;
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
  
  // Start acknowledgment process
  ackTargetAddr = senderAddr;
  ackState = ACK_INIT;
  ackAttemptCount = 0;
  processAcknowledgment(); // Begin processing immediately
}

void processAcknowledgment() {
  static unsigned long ackTimer = 0;
  unsigned long currentTime = millis();
  
  switch (ackState) {
    case ACK_INIT:
      // Create peer info structure
      ackTimer = currentTime;
      ackState = ACK_PEER_SETUP;
      // Fall through to next state
      
    case ACK_PEER_SETUP:
      {
        if (ackTargetAddr == NULL) {
          ackState = ACK_COMPLETE;
          break;
        }
        
        esp_now_peer_info_t peerInfo = {};
        memcpy(peerInfo.peer_addr, ackTargetAddr, 6);
        peerInfo.channel = WIFI_CHANNEL;
        peerInfo.encrypt = false;
        
        // More reliable peer management - check before deleting
        if (esp_now_is_peer_exist(ackTargetAddr)) {
          esp_now_del_peer(ackTargetAddr);
        }
        
        // Add peer
        esp_err_t result = esp_now_add_peer(&peerInfo);
        
        if (result != ESP_OK && currentTime - ackTimer < 10) {
          // Need to wait a bit before retrying
          break;
        } else if (result != ESP_OK) {
          // Retry once more if failed
          result = esp_now_add_peer(&peerInfo);
        }
        
        if (result != ESP_OK) {
          Serial.printf("Peer management error: %d\n", result);
          ackState = ACK_COMPLETE;
        } else {
          ackState = ACK_SEND;
          ackTimer = currentTime;
        }
      }
      break;
      
    case ACK_SEND:
      {
        message_t message;
        message.type = ACKNOWLEDGMENT;
        message.value = activeLedIndex;
        
        esp_err_t result = esp_now_send(ackTargetAddr, (uint8_t *)&message, sizeof(message));
        if (result == ESP_OK) {
          Serial.printf("Acknowledgment %d sent successfully\n", ackAttemptCount + 1);
        } else {
          Serial.printf("Error on attempt %d: %d\n", ackAttemptCount + 1, result);
        }
        
        ackAttemptCount++;
        ackTimer = currentTime;
        ackState = ACK_WAIT;
      }
      break;
      
    case ACK_WAIT:
      if (currentTime - ackTimer >= 20) {
        // Wait is complete, check if we need more attempts
        if (ackAttemptCount < 3) {
          ackState = ACK_SEND;
        } else {
          Serial.printf("Completed acknowledgments for LED index: %d\n", activeLedIndex);
          ackState = ACK_COMPLETE;
        }
      }
      break;
      
    case ACK_COMPLETE:
      // Reset for next time
      ackState = ACK_INIT;
      ackTargetAddr = NULL;
      break;
  }
}

void processDiscoveryResponse() {
  static unsigned long discoveryTimer = 0;
  unsigned long currentTime = millis();
  
  switch (discoveryState) {
    case DISCOVERY_PEER_SETUP:
      {
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
        
        if (result != ESP_OK && currentTime - stateTimer < 10) {
          // Need to wait a bit before retrying
          break;
        } else if (result != ESP_OK) {
          // Retry once more
          result = esp_now_add_peer(&peerInfo);
        }
        
        if (result != ESP_OK) {
          Serial.printf("Peer management error: %d\n", result);
          discoveryState = DISCOVERY_COMPLETE;
        } else {
          discoveryState = DISCOVERY_SEND;
        }
      }
      break;
      
    case DISCOVERY_SEND:
      {
        message_t message;
        message.type = DISCOVERY;
        message.value = 0;
        
        esp_err_t result = esp_now_send(lastSenderMac, (uint8_t *)&message, sizeof(message));
        Serial.printf("Discovery response status: %s\n", 
                      (result == ESP_OK) ? "Success" : "Failed");
        
        sendDiscoveryResponse = false;
        discoveryState = DISCOVERY_COMPLETE;
      }
      break;
      
    case DISCOVERY_COMPLETE:
      // Reset for next time
      discoveryState = DISCOVERY_INIT;
      break;
  }
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