#include <Arduino.h>
#include <esp_now.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_sleep.h>

// Pin and PWM configuration
const int NUM_LEDS = 3;
const int LED_PINS[NUM_LEDS] = {25, 26, 27};  // GPIO pins for the LEDs
const int PWM_CHANNELS[NUM_LEDS] = {0, 1, 2}; // Separate PWM channel for each LED
const int PWM_RESOLUTION = 12;                // 12-bit resolution (0-4095)
const int PWM_FREQUENCY = 500;                // Lower frequency to save power

// Sleep & Wake configuration - EXTREME OPTIMIZATION
const unsigned long LISTEN_DURATION = 10000;  // Listen for 10 seconds
const unsigned long SLEEP_DURATION = 1000;    // Sleep for only 1 second
const unsigned long INITIAL_LISTEN = 60000;   // Listen for 60 seconds on first boot

// WiFi channel - MUST MATCH SENDER
const uint8_t WIFI_CHANNEL = 1;               // Fixed channel for better communication

// Skip sleep cycles to stay awake longer
const int INITIAL_NO_SLEEP_CYCLES = 3;        // Stay awake for multiple cycles initially
int noSleepCyclesLeft = INITIAL_NO_SLEEP_CYCLES;

// Current LED state
int currentLedIndex = -1;
int bootCount = 0;

// ESP-NOW Data Structure
typedef struct {
  char message[10]; // String message like "green", "yellow", "red"
  int ledNumber;    // Integer 0, 1, or 2
  bool useString;   // Flag to indicate if we're using string or int
} ESPNOWData;

// Debugging - set to true for testing
const bool SERIAL_ENABLED = true;

// Variables 
unsigned long listenStartTime = 0;
bool messageReceived = false;

// Function to print debug messages
void debugPrint(const String &message) {
  if (SERIAL_ENABLED) {
    Serial.println(message);
    Serial.flush(); // Make sure message is sent completely
  }
}

// Set LED brightness (0.0 = off, 1.0 = full brightness)
void setLedBrightness(int ledIndex, float brightness) {
  // Only process valid indices
  if (ledIndex < 0 || ledIndex >= NUM_LEDS) return;
  
  // Clamp brightness between 0 and 1
  brightness = (brightness < 0) ? 0 : (brightness > 1) ? 1 : brightness;
  
  // For low-side switching, 4095 is OFF and 0 is fully ON
  int dutyCycle = 4095 - (int)(4095 * brightness);
  ledcWrite(PWM_CHANNELS[ledIndex], dutyCycle);
}

// Turn off all LEDs except the specified one
void setActiveLed(int ledIndex) {
  for (int i = 0; i < NUM_LEDS; i++) {
    if (i == ledIndex) {
      setLedBrightness(i, 1.0); // Full brightness
    } else {
      setLedBrightness(i, 0.0); // Off
    }
  }
  
  // Remember this LED
  currentLedIndex = ledIndex;
  debugPrint("Active LED set to index " + String(ledIndex));
}

// Test all LEDs in sequence
void runLedTest() {
  debugPrint("=== STARTING LED TEST ===");
  
  // Turn off all LEDs initially
  for (int i = 0; i < NUM_LEDS; i++) {
    setLedBrightness(i, 0.0);
  }
  delay(300);
  
  // Sequence through each LED
  for (int i = 0; i < NUM_LEDS; i++) {
    debugPrint("Testing LED " + String(LED_PINS[i]));
    setLedBrightness(i, 1.0);
    delay(300);
    setLedBrightness(i, 0.0);
    delay(100);
  }
  
  // Flash all LEDs together
  for (int i = 0; i < 2; i++) {
    // All on
    for (int j = 0; j < NUM_LEDS; j++) {
      setLedBrightness(j, 1.0);
    }
    delay(200);
    
    // All off
    for (int j = 0; j < NUM_LEDS; j++) {
      setLedBrightness(j, 0.0);
    }
    delay(200);
  }
  
  debugPrint("=== LED TEST COMPLETE ===");
}

// Function to initialize WiFi and ESP-NOW
void setupWiFiAndESPNOW() {
  // Initialize WiFi with specific settings for better compatibility
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(10); // Ultra short delay
  
  WiFi.mode(WIFI_STA);
  
  // Disable Auto-Connect - important for reliability with ESP-NOW
  WiFi.setAutoConnect(false);
  WiFi.setAutoReconnect(false);
  delay(10); // Ultra short delay
  
  // Broadcast MAC for better discovery - uncomment if needed
  // uint8_t broadcastMac[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  // esp_wifi_set_mac(WIFI_IF_STA, broadcastMac);
  
  // Set WiFi channel explicitly (must match sender's channel)
  esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
  
  // Set WiFi to maximum power for better reception
  esp_wifi_set_max_tx_power(84); // Maximum power
  
  debugPrint("MAC Address: " + WiFi.macAddress());
  
  // Disable power saving for more reliable reception
  esp_wifi_set_ps(WIFI_PS_NONE);
  
  // Initialize ESP-NOW with faster retries
  for (int retry = 0; retry < 2; retry++) {
    esp_now_deinit(); // Clean start
    delay(10); // Ultra short delay
    if (esp_now_init() == ESP_OK) {
      debugPrint("ESP-NOW initialized successfully");
      return;
    }
  }
  
  debugPrint("ESP-NOW initialization failed after retries");
}

// ESP-NOW callback function
void OnDataRecv(const uint8_t *mac, const uint8_t *incomingData, int len) {
  char macStr[18];
  snprintf(macStr, sizeof(macStr), "%02x:%02x:%02x:%02x:%02x:%02x",
         mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  
  debugPrint("ESP-NOW message received from: " + String(macStr));
  debugPrint("Data length: " + String(len) + " bytes");
  
  messageReceived = true;
  
  if (len == sizeof(ESPNOWData)) {
    // Process the received message
    ESPNOWData receivedData;
    memcpy(&receivedData, incomingData, sizeof(ESPNOWData));
    
    int targetLed = -1;
    
    // Determine which LED to activate
    if (receivedData.useString) {
      String message = String(receivedData.message);
      message.toLowerCase();
      
      // Match color string to LED index
      if (message == "green") {
        targetLed = 0; // LED on pin 25
      } else if (message == "yellow") {
        targetLed = 1; // LED on pin 26
      } else if (message == "red") {
        targetLed = 2; // LED on pin 27
      }
      
      debugPrint("Received string command: " + message);
    } else {
      // Using numerical value (0, 1, or 2)
      if (receivedData.ledNumber >= 0 && receivedData.ledNumber < NUM_LEDS) {
        targetLed = receivedData.ledNumber;
      }
      
      debugPrint("Received numeric command: " + String(receivedData.ledNumber));
    }
    
    // Activate the appropriate LED
    if (targetLed >= 0 && targetLed < NUM_LEDS) {
      setActiveLed(targetLed);
    }
  } else {
    debugPrint("Warning: Incorrect data size received");
  }
  
    //  debugPrint("Going to SLEEP now");
    //  prepareForSleep();
    // After waking from light sleep, code continues here
  // Extend the listening period after receiving a message
  listenStartTime = millis();
}

// Prepare for light sleep while keeping LEDs on
void prepareForSleep() {
  debugPrint("Light sleep for " + String(SLEEP_DURATION / 1000) + "s");
  
  // No need to modify GPIO settings for light sleep
  // LEDs will stay in their current state during light sleep
  
  // Configure when to wake up
  esp_sleep_enable_timer_wakeup(SLEEP_DURATION * 1000); // Convert to microseconds
  
  // Disable ESP-NOW and WiFi to save power (but fast to reinitialize)
  esp_now_deinit();
  WiFi.disconnect(false); // Don't disable RF
  
  Serial.flush(); // Make sure all Serial data is sent before sleeping
  delay(10); // Brief delay to allow final operations to complete
  
  // Start light sleep
  esp_light_sleep_start();
  
  // Code continues here after waking up from light sleep
  debugPrint("Woke up!");
}

void setup() {
  // Initialize serial first
  if (SERIAL_ENABLED) {
    Serial.begin(115200);
    delay(100); // Shorter delay
    Serial.println("\n\n"); // Clear any garbage
  }
  
  // Increment boot count
  bootCount++;
  
  // Configure PWM for all LEDs
  for (int i = 0; i < NUM_LEDS; i++) {
    ledcSetup(PWM_CHANNELS[i], PWM_FREQUENCY, PWM_RESOLUTION);
    ledcAttachPin(LED_PINS[i], PWM_CHANNELS[i]);
    
    // Initially turn all LEDs off
    setLedBrightness(i, 0.0);
  }
  
  // First boot only
  if (bootCount == 1) {
    debugPrint("ESP32 ESP-NOW LED Receiver - Ultra-Fast Mode");
    debugPrint("------------------------------------------");
    
    // Set all LEDs to ON briefly to show we're alive
    for (int i = 0; i < NUM_LEDS; i++) {
      setLedBrightness(i, 1.0);
    }
    delay(300);
    for (int i = 0; i < NUM_LEDS; i++) {
      setLedBrightness(i, 0.0);
    }
    delay(100);
    
    debugPrint("WiFi Channel: " + String(WIFI_CHANNEL));
    debugPrint("Extended initial wake time: " + String(INITIAL_LISTEN / 1000) + "s");
    debugPrint("Will stay awake for " + String(INITIAL_NO_SLEEP_CYCLES) + " cycles after boot");
  } else {
    debugPrint("Wakeup #" + String(bootCount));
  }
  
  // Restore active LED state
  if (currentLedIndex >= 0 && currentLedIndex < NUM_LEDS) {
    debugPrint("Active LED: " + String(currentLedIndex));
    setActiveLed(currentLedIndex); // Full brightness
  }
  
  // Initialize WiFi and ESP-NOW
  setupWiFiAndESPNOW();
  
  // Register callback for when data is received
  esp_now_register_recv_cb(OnDataRecv);
  
  // Set CPU to higher frequency for faster response on first boot
  if (bootCount == 1) {
    setCpuFrequencyMhz(240); // Full speed during initial connection
  } else {
    setCpuFrequencyMhz(80);  // Lower speed for power saving
  }
  
  // Start the listen period
  listenStartTime = millis();
  messageReceived = false;
  
  // On first boot, listen longer to ensure pairing
  unsigned long currentListenTime = (bootCount == 1) ? INITIAL_LISTEN : LISTEN_DURATION;
  
  debugPrint("Listening for " + String(currentListenTime / 1000) + "s");
}

void loop() {
  unsigned long currentTime = millis();
  unsigned long currentListenDuration = (bootCount == 1) ? INITIAL_LISTEN : LISTEN_DURATION;
  
  // Print a periodic heartbeat for debugging - less frequent
  static unsigned long lastHeartbeat = 0;
  if (currentTime - lastHeartbeat > 4000) { // 4-second heartbeat
    debugPrint("Listening... (" + String((currentTime - listenStartTime) / 1000) + "s)");
    lastHeartbeat = currentTime;
  }
  
  // Check if listen period is over
  if (currentTime - listenStartTime >= currentListenDuration) {
    if (messageReceived) {
      debugPrint("Message received this cycle");
    } else {
      debugPrint("No messages received");
    }
    
    // Skip sleep on first boot or during no-sleep cycles
    if (bootCount == 1 || noSleepCyclesLeft > 0) {
      if (bootCount == 1) {
        debugPrint("First boot - continuing without sleep");
      } else {
        debugPrint("Skipping sleep - remaining cycles: " + String(noSleepCyclesLeft));
        noSleepCyclesLeft--;
      }
      
      // Reset for next listening period without sleeping
      listenStartTime = millis();
      messageReceived = false;
      
      // Reinitialize ESP-NOW just to be safe
      esp_now_unregister_recv_cb();
      setupWiFiAndESPNOW();
      esp_now_register_recv_cb(OnDataRecv);
      
      return; // Skip sleep
    }
    
    debugPrint("Going to SLEEP now");
    prepareForSleep();
    // After waking from light sleep, code continues here
    
    // Reset for next listening period
    listenStartTime = millis();
    messageReceived = false;
    
    // Re-initialize WiFi and ESP-NOW after waking up
    setupWiFiAndESPNOW();
    esp_now_register_recv_cb(OnDataRecv);
  }
  
  // Small delay to prevent busy waiting
  delay(1); // Minimal delay for maximum responsiveness
}