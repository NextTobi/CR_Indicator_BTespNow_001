/**
 * ESP32 ESP-NOW LED Indicator System - SENDER CODE
 * 
 * This file contains code specifically for the sender device.
 * It implements improved communication reliability and automatic
 * progression when acknowledgments are not received.
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

// Function prototypes
void setupEspNow();
void setupPeer();
void printMacAddress(const uint8_t *addr);
void sendLedCommand();
void onDataSent(const uint8_t *macAddr, esp_now_send_status_t status);
void onDataReceived(const uint8_t *macAddr, const uint8_t *data, int dataLen);

void setup() {
  Serial.begin(115200);
  delay(500); // Short delay for serial to initialize
  
  // Set CPU frequency to 80MHz for power efficiency
  setCpuFrequencyMhz(80);
  
  Serial.print("\n\n==== ESP32 ESP-NOW LED System ====\n");
  Serial.println("SENDER MODE");
  Serial.println("FW Version: 2.0 - Reliable Communication");
  Serial.println("This device will send LED commands to the indicator");
  
  // Initialize preferences for storing paired MAC addresses
  preferences.begin(PREF_NAMESPACE, false);
  
  // Setup ESP-NOW
  setupEspNow();
  
  // Delay before starting transmission
  Serial.println("Sender ready, will begin sending LED commands");
  Serial.println("Target indicator MAC address:");
  printMacAddress(indicatorMac);
  Serial.print("Using enhanced retry logic: ");
  Serial.print(MAX_RETRIES_BEFORE_WAIT);
  Serial.print(" retries every ");
  Serial.print(RETRY_INTERVAL_MS);
  Serial.println("ms");
  
  delay(1000);  // Wait briefly before starting
}

void loop() {
  unsigned long currentTime = millis();
  
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
      setupPeer();
    }
  } else {
    // Not acknowledged yet, try sending again after interval
    if (currentTime - lastSendTime >= RETRY_INTERVAL_MS) {
      if (retryCount < MAX_RETRIES_BEFORE_WAIT) {
        sendLedCommand();
        lastSendTime = currentTime;
        retryCount++;
      } else {
        // Force progression after max retries
        Serial.println("Forcing progression after maximum retries");
        currentLedIndex = (currentLedIndex + 1) % NUM_LEDS;
        retryCount = 0;
        lastSendTime = currentTime;
        
        // Force peer re-registration
        esp_now_del_peer(indicatorMac);
        setupPeer();
      }
    }
  }
  
  delay(10);  // Short delay to prevent busy-waiting
}

void setupEspNow() {
  // Initialize WiFi in Station mode
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(300);  // Increased delay
  
  // Set WiFi channel
  esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
  delay(100);  // Added delay
  
  // Initialize ESP-NOW with error checking
  esp_err_t result = esp_now_init();
  if (result != ESP_OK) {
    Serial.print("Error initializing ESP-NOW, code: ");
    Serial.println(result);
    delay(3000);
    ESP.restart();  // Restart ESP32 if initialization fails
    return;
  }
  
  // Register callbacks
  esp_now_register_recv_cb(onDataReceived);
  esp_now_register_send_cb(onDataSent);
  
  // Setup peer
  setupPeer();
  
  Serial.print("Device MAC Address: ");
  Serial.println(WiFi.macAddress());
  Serial.print("Operating on WiFi channel: ");
  Serial.println(WIFI_CHANNEL);
  Serial.print("Target indicator MAC: ");
  printMacAddress(indicatorMac);
}

void setupPeer() {
  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, indicatorMac, 6);
  peerInfo.channel = WIFI_CHANNEL;
  peerInfo.encrypt = false;
  
  // Try multiple times to add the peer
  bool peerAdded = false;
  for (int i = 0; i < 3; i++) {
    if (esp_now_add_peer(&peerInfo) == ESP_OK) {
      peerAdded = true;
      Serial.println("Successfully added indicator as peer");
      break;
    }
    Serial.println("Failed to add peer, retrying...");
    delay(500);
  }
  
  if (!peerAdded) {
    Serial.println("ERROR: Failed to add peer after multiple attempts");
  }
  
  // Verify peer registration
  if (esp_now_is_peer_exist(indicatorMac)) {
    Serial.println("Peer verification: Successfully registered indicator");
  } else {
    Serial.println("ERROR: Peer verification failed - indicator not registered");
  }
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
      setupPeer();
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