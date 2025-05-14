/**
 * ESP32 ESP-NOW LED Indicator System - SENDER CODE
 * 
 * This file contains code specifically for the sender device.
 * It implements improved communication reliability, automatic
 * progression when acknowledgments are not received, and non-blocking operation.
 */

#include <Arduino.h>
#include <esp_now.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <Preferences.h>

// Configuration constants
const int NUM_LEDS = 3;
const int LED_PINS[NUM_LEDS] = {25, 26, 27};  // GPIO pins for the LEDs (reference only)
const int WIFI_CHANNEL = 6;  // Using channel 6 instead of 1 to reduce interference
const char* PREF_NAMESPACE = "espnow-leds";

// Timing constants - optimized for reliability
const int RETRY_INTERVAL_MS = 500;      // 0.5 seconds between retry attempts
const int NEXT_LED_DELAY_MS = 10000;    // 10 seconds before switching to next LED
const int MAX_RETRIES_BEFORE_WAIT = 12; // Maximum number of retries before waiting

// Setup state machine states
enum SetupState {
  SETUP_INIT,
  SETUP_SERIAL_WAIT,
  SETUP_ESPNOW_START,
  SETUP_WIFI_DISCONNECT_WAIT,
  SETUP_WIFI_CHANNEL_WAIT,
  SETUP_PEER_ATTEMPT,
  SETUP_PEER_WAIT,
  SETUP_COMPLETE
};

// ESP-NOW setup retry state
enum PeerSetupState {
  PEER_INIT,
  PEER_ATTEMPT,
  PEER_RETRY_WAIT,
  PEER_COMPLETE
};

// Message types for communication protocol
enum MessageType {
  LED_COMMAND = 1,
  ACKNOWLEDGMENT = 2,
  DISCOVERY = 3
};

// ESP-NOW message structure
typedef struct {
  uint8_t type;     // Message type (see MessageType enum)
  uint8_t value;    // LED index or acknowledgment value
} message_t;

// Global variables
Preferences preferences;

// IMPORTANT: Replace with the MAC address of your indicator device
uint8_t indicatorMac[6] = {0xE8, 0x31, 0xCD, 0xC6, 0xFE, 0x68};

// Sender state variables
int currentLedIndex = 0;
bool acknowledged = false;
unsigned long lastSendTime = 0;
unsigned long lastSuccessTime = 0;
int retryCount = 0;

// Setup state variables
SetupState setupState = SETUP_INIT;
PeerSetupState peerState = PEER_INIT;
unsigned long setupTimer = 0;
int peerAttemptCount = 0;

// Function prototypes
void setupEspNow();
bool setupPeer(bool isInitialSetup = false);
void printMacAddress(const uint8_t *addr);
void sendLedCommand();
void onDataSent(const uint8_t *macAddr, esp_now_send_status_t status);
void onDataReceived(const uint8_t *macAddr, const uint8_t *data, int dataLen);

void setup() {
  Serial.begin(115200);
  
  // Set CPU frequency to 80MHz for power efficiency
  setCpuFrequencyMhz(80);
  
  // Initialize setup state machine
  setupState = SETUP_SERIAL_WAIT;
  setupTimer = millis();
  
  // Initialize preferences for storing paired MAC addresses
  preferences.begin(PREF_NAMESPACE, false);
}

void loop() {
  unsigned long currentTime = millis();
  
  // Handle setup state machine
  if (setupState != SETUP_COMPLETE) {
    switch (setupState) {
      case SETUP_SERIAL_WAIT:
        // Wait for serial to initialize (non-blocking)
        if (currentTime - setupTimer >= 500) {
          Serial.print("\n\n==== ESP32 ESP-NOW LED System ====\n");
          Serial.println("SENDER MODE");
          Serial.println("FW Version: 2.0 - Reliable Communication (Non-blocking)");
          Serial.println("This device will send LED commands to the indicator");
          
          setupState = SETUP_ESPNOW_START;
        }
        break;
        
      case SETUP_ESPNOW_START:
        // Initialize WiFi in Station mode
        WiFi.mode(WIFI_STA);
        WiFi.disconnect();
        setupTimer = currentTime;
        setupState = SETUP_WIFI_DISCONNECT_WAIT;
        break;
        
      case SETUP_WIFI_DISCONNECT_WAIT:
        if (currentTime - setupTimer >= 300) {
          // Set WiFi channel
          esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
          setupTimer = currentTime;
          setupState = SETUP_WIFI_CHANNEL_WAIT;
        }
        break;
        
      case SETUP_WIFI_CHANNEL_WAIT:
        if (currentTime - setupTimer >= 100) {
          // Initialize ESP-NOW with error checking
          esp_err_t result = esp_now_init();
          if (result != ESP_OK) {
            Serial.print("Error initializing ESP-NOW, code: ");
            Serial.println(result);
            setupTimer = currentTime;
            // Wait 3 seconds before restart (we'll check in next iteration)
            setupState = SETUP_PEER_ATTEMPT;
            ESP.restart();  // Restart ESP32 if initialization fails
            return;
          }
          
          // Register callbacks
          esp_now_register_recv_cb(onDataReceived);
          esp_now_register_send_cb(onDataSent);
          
          Serial.print("Device MAC Address: ");
          Serial.println(WiFi.macAddress());
          Serial.print("Operating on WiFi channel: ");
          Serial.println(WIFI_CHANNEL);
          Serial.print("Target indicator MAC: ");
          printMacAddress(indicatorMac);
          
          peerState = PEER_INIT;
          setupState = SETUP_PEER_ATTEMPT;
        }
        break;
        
      case SETUP_PEER_ATTEMPT:
        if (setupPeer(true)) {
          setupState = SETUP_COMPLETE;
          setupTimer = currentTime;
          
          Serial.println("Sender ready, will begin sending LED commands");
          Serial.println("Target indicator MAC address:");
          printMacAddress(indicatorMac);
          Serial.print("Using enhanced retry logic: ");
          Serial.print(MAX_RETRIES_BEFORE_WAIT);
          Serial.print(" retries every ");
          Serial.print(RETRY_INTERVAL_MS);
          Serial.println("ms");
        }
        break;
        
      case SETUP_COMPLETE:
        // Wait 1 second before starting
        if (currentTime - setupTimer >= 1000) {
          lastSuccessTime = currentTime;
          setupState = SETUP_COMPLETE;
        }
        break;
    }
    
    return; // Don't process the rest of the loop until setup is complete
  }
  
  // Normal operation (after setup complete)
  if (acknowledged) {
    // If acknowledged, wait the delay time then proceed to next LED
    if (currentTime - lastSuccessTime >= NEXT_LED_DELAY_MS) {
      Serial.println("Moving to next LED");
      currentLedIndex = (currentLedIndex + 1) % NUM_LEDS;
      acknowledged = false;
      retryCount = 0;
      lastSendTime = 0;
      lastSuccessTime = currentTime;
      
      // Force peer re-registration periodically
      esp_now_del_peer(indicatorMac);
      peerState = PEER_INIT; // Reset peer setup state
    }
  } else {
    // Not acknowledged yet, try sending again after interval
    if (currentTime - lastSendTime >= RETRY_INTERVAL_MS) {
      if (retryCount < MAX_RETRIES_BEFORE_WAIT) {
        // Check if peer setup is complete before sending
        if (peerState == PEER_COMPLETE) {
          sendLedCommand();
          lastSendTime = currentTime;
          retryCount++;
        } else {
          // Try to set up peer if not complete
          setupPeer();
        }
      } else {
        // Force progression after max retries
        Serial.println("Forcing progression after maximum retries");
        currentLedIndex = (currentLedIndex + 1) % NUM_LEDS;
        retryCount = 0;
        lastSendTime = currentTime;
        
        // Force peer re-registration
        esp_now_del_peer(indicatorMac);
        peerState = PEER_INIT; // Reset peer setup state
      }
    }
  }
  
  // Handle peer setup state machine (non-blocking)
  if (peerState != PEER_COMPLETE) {
    setupPeer();
  }
}

void setupEspNow() {
  // This function is now handled by the setup state machine
}

bool setupPeer(bool isInitialSetup) {
  static unsigned long peerTimer = 0;
  unsigned long currentTime = millis();
  
  switch (peerState) {
    case PEER_INIT:
      peerAttemptCount = 0;
      peerTimer = currentTime;
      peerState = PEER_ATTEMPT;
      return false;
      
    case PEER_ATTEMPT:
      {
        esp_now_peer_info_t peerInfo = {};
        memcpy(peerInfo.peer_addr, indicatorMac, 6);
        peerInfo.channel = WIFI_CHANNEL;
        peerInfo.encrypt = false;
        
        esp_err_t result = esp_now_add_peer(&peerInfo);
        if (result == ESP_OK) {
          Serial.println("Successfully added indicator as peer");
          
          // Verify peer registration
          if (esp_now_is_peer_exist(indicatorMac)) {
            Serial.println("Peer verification: Successfully registered indicator");
            peerState = PEER_COMPLETE;
            return true;
          } else {
            Serial.println("ERROR: Peer verification failed - indicator not registered");
          }
        } else {
          Serial.println("Failed to add peer, will retry...");
        }
        
        peerAttemptCount++;
        if (peerAttemptCount >= 3) {
          if (isInitialSetup) {
            // For initial setup, reset to retry
            peerAttemptCount = 0;
            Serial.println("ERROR: Failed to add peer after multiple attempts. Continuing to retry...");
          } else {
            Serial.println("ERROR: Failed to add peer after multiple attempts");
            peerState = PEER_COMPLETE; // Consider it complete even though it failed
            return false;
          }
        }
        
        peerTimer = currentTime;
        peerState = PEER_RETRY_WAIT;
        return false;
      }
      
    case PEER_RETRY_WAIT:
      if (currentTime - peerTimer >= 500) {
        peerState = PEER_ATTEMPT;
      }
      return false;
      
    case PEER_COMPLETE:
      return true;
  }
  
  return false;
}

void printMacAddress(const uint8_t *addr) {
  char macStr[18];
  snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
           addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
  Serial.println(macStr);
}

void sendLedCommand() {
  message_t message;
  message.type = LED_COMMAND;
  message.value = currentLedIndex;
  
  Serial.print("Sending command to activate LED index: ");
  Serial.print(currentLedIndex);
  Serial.print(" (pin: ");
  Serial.print(LED_PINS[currentLedIndex]);
  Serial.println(")");
  Serial.print("Target MAC: ");
  printMacAddress(indicatorMac);
  
  esp_err_t result = esp_now_send(indicatorMac, (uint8_t *)&message, sizeof(message));
  
  if (result != ESP_OK) {
    Serial.print("Error sending message, code: ");
    Serial.println(result);
    
    // Check if peer still exists, re-add if needed
    if (!esp_now_is_peer_exist(indicatorMac)) {
      Serial.println("Peer lost, attempting to re-add");
      peerState = PEER_INIT; // Reset peer setup state
    }
  } else {
    Serial.println("Message sent successfully to transport layer");
  }
}

void onDataSent(const uint8_t *macAddr, esp_now_send_status_t status) {
  Serial.print("Last packet send status: ");
  Serial.println(status == ESP_NOW_SEND_SUCCESS ? "Delivery Success" : "Delivery Fail");
  
  // Note: We only consider it acknowledged when we receive the actual
  // acknowledgment message, not just on delivery success
}

void onDataReceived(const uint8_t *macAddr, const uint8_t *data, int dataLen) {
  // Print who sent this data
  char macStr[18];
  snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
           macAddr[0], macAddr[1], macAddr[2], macAddr[3], macAddr[4], macAddr[5]);
  
  Serial.print("Received data from: ");
  Serial.println(macStr);
  
  // Process all incoming messages without strict MAC filtering for better reliability
  if (dataLen == sizeof(message_t)) {
    message_t *message = (message_t *)data;
    
    switch (message->type) {
      case ACKNOWLEDGMENT: {
        Serial.println("Received acknowledgment");
        Serial.print("Confirmed LED index: ");
        Serial.println(message->value);
        acknowledged = true;
        lastSuccessTime = millis();
        break;
      }
        
      case DISCOVERY: {
        Serial.println("Received discovery response");
        break;
      }
        
      default: {
        Serial.print("Unknown message type: ");
        Serial.println(message->type);
        break;
      }
    }
  }
}