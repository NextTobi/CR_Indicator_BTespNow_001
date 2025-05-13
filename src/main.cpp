#include <Arduino.h>
#include <esp_now.h>
#include <WiFi.h>
#include <esp_wifi.h>  // For WiFi power control
#include <esp_bt.h>    // For basic Bluetooth power control
#include <esp_sleep.h> // For sleep modes

// Pin and PWM configuration
const int NUM_LEDS = 3;
const int LED_PINS[NUM_LEDS] = {25, 26, 27};  // GPIO pins for the LEDs
const int PWM_CHANNELS[NUM_LEDS] = {0, 1, 2}; // Separate PWM channel for each LED
const int PWM_RESOLUTION = 12;                // 12-bit resolution (0-4095)
const int PWM_FREQUENCY = 1000;               // 5kHz frequency

// Timing configurations (in milliseconds)
const unsigned long FADE_IN_DURATION = 2000;   // Fade in over 2 seconds
const unsigned long FULL_BRIGHTNESS_DURATION = 800; // Hold full brightness for 800ms
const unsigned long FADE_OUT_DURATION = 1500;  // Fade out over 1.5 seconds
const unsigned long OFF_DURATION = 600;        // Hold off state for 600ms

// Test speed
const int TEST_SPEED_DEFAULT = 250;  // Default speed in milliseconds
int testSpeed = TEST_SPEED_DEFAULT;  // Adjustable speed that can be changed

// LED states
enum LedState {
  FADE_IN,
  FULL_BRIGHTNESS,
  FADE_OUT,
  OFF
};

// ESP-NOW Data Structure
typedef struct {
  char message[10]; // String message like "green", "yellow", "red"
  int ledNumber;    // Integer 0, 1, or 2
  bool useString;   // Flag to indicate if we're using string or int
} ESPNOWData;

// Global variables
LedState ledStates[NUM_LEDS] = {OFF, OFF, OFF}; // All LEDs start off
unsigned long stateStartTimes[NUM_LEDS] = {0, 0, 0};
unsigned long lastUpdateTime = 0;
const unsigned long UPDATE_INTERVAL = 10; // Update PWM every 10ms (was 5ms)
int activeLed = -1; // Tracks which LED is currently active
unsigned long lastActivityTime = 0; // Track last activity for power management

// Power management settings
const uint8_t WIFI_TX_POWER = 8;     // Lowest usable power level (8 = ~2dBm)
const bool SERIAL_ENABLED = false;    // Set to false to disable Serial after init
const int MIN_CPU_FREQ = 80;         // Minimum CPU frequency (was 40, but 80 is safer)
const int NORMAL_CPU_FREQ = 80;     // Normal running CPU frequency

// Most recent received data
ESPNOWData receivedData;
bool newDataReceived = false;

// Custom print function that checks if Serial should be used
void debugPrint(const String &message) {
  if (SERIAL_ENABLED) {
    Serial.println(message);
  }
}

// Function prototypes
void OnDataRecv(const uint8_t *mac, const uint8_t *incomingData, int len);
void setLedBrightness(int ledIndex, float brightness);
float calculateFadeInBrightness(float progress);
float calculateFadeOutBrightness(float progress);
void updateLedState(int ledIndex, unsigned long currentTime);
void runLedTest();
void setTestSpeed(int speedValue);
void randomLedBlink(int count, int speed);
void processReceivedData();

// ESP-NOW callback function to handle received data
void OnDataRecv(const uint8_t *mac, const uint8_t *incomingData, int len) {
  // Update activity time first thing
  lastActivityTime = millis();
  
  // We need to temporarily boost CPU frequency for processing
  setCpuFrequencyMhz(NORMAL_CPU_FREQ);
  
  if (SERIAL_ENABLED) {
    char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02x:%02x:%02x:%02x:%02x:%02x",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    
    Serial.print("ESP-NOW from: ");
    Serial.print(macStr);
    Serial.print(", len: ");
    Serial.println(len);
  }
  
  if (len == sizeof(ESPNOWData)) {
    memcpy(&receivedData, incomingData, sizeof(ESPNOWData));
    newDataReceived = true;
    
    if (SERIAL_ENABLED) {
      Serial.print("Data: ");
      if (receivedData.useString) {
        Serial.print("String: ");
        Serial.println(receivedData.message);
      } else {
        Serial.print("LED #");
        Serial.println(receivedData.ledNumber);
      }
    }
    
    // Process immediately
    processReceivedData();
  }
}


// Safe power-saving implementation
void applyPowerSaving() {
  debugPrint("Applying power saving measures...");
  
  // 1. Disable Bluetooth (safe method)
  btStop();  // This safely disables BT on ESP32
  
  // 2. Set WiFi to minimum power mode
  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
  esp_wifi_set_max_tx_power(WIFI_TX_POWER);
  
  // 3. Reduce CPU frequency
  setCpuFrequencyMhz(MIN_CPU_FREQ);
  
  // 4. Configure unused GPIO pins to reduce power leakage
  for (int i = 0; i < 40; i++) {
    // Skip LED pins (25, 26, 27) and essential system pins
    if (i != 25 && i != 26 && i != 27 && i != 0 && i != 1 && i != 3) {
      pinMode(i, INPUT_PULLDOWN);
    }
  }
  
  // 5. Configure light sleep for delay()
  esp_sleep_enable_timer_wakeup(1000); // Wake every 1ms
  
  // 6. Disable WiFi auto-reconnect (saves power on connection loss)
  WiFi.setAutoReconnect(false);
  
  // Print current status
  debugPrint("Power saving mode active:");
  debugPrint("- Bluetooth: Disabled");
  debugPrint("- WiFi TX Power: Minimum");
  debugPrint("- CPU Frequency: " + String(getCpuFrequencyMhz()) + " MHz");
  debugPrint("- Light sleep: Enabled for delay()");
  
  // Optional: Disable Serial after setup if configured
  if (!SERIAL_ENABLED) {
    Serial.println("Serial will be disabled in 3 seconds to save power...");
    Serial.println("ESP-NOW reception will continue to work");
    delay(3000);
    Serial.end();
  }
}



// Process received ESP-NOW data
void processReceivedData() {
  if (!newDataReceived) return;
  
  newDataReceived = false;
  int targetLed = -1;
  
  // Determine which LED to activate based on received data
  if (receivedData.useString) {
    String message = String(receivedData.message);
    message.toLowerCase();
    
    if (message == "green") {
      targetLed = 0; // LED on pin 25
    } else if (message == "yellow") {
      targetLed = 1; // LED on pin 26
    } else if (message == "red") {
      targetLed = 2; // LED on pin 27
    }
  } else {
    // Using numerical value (0, 1, or 2)
    if (receivedData.ledNumber >= 0 && receivedData.ledNumber < NUM_LEDS) {
      targetLed = receivedData.ledNumber;
    }
  }
  
  // If valid LED target was determined, activate it
  if (targetLed >= 0) {
    // Turn off all LEDs first
    for (int i = 0; i < NUM_LEDS; i++) {
      if (i != targetLed) {
        ledStates[i] = OFF;
        setLedBrightness(i, 0);
      }
    }
    
    // Start the fade in sequence for the target LED
    unsigned long currentTime = millis();
    ledStates[targetLed] = FADE_IN;
    stateStartTimes[targetLed] = currentTime;
    activeLed = targetLed; // Track which LED is active
    
    Serial.print("Activating LED ");
    Serial.print(LED_PINS[targetLed]);
    Serial.println(" based on received data - continuous mode");
  }
}

// Set LED brightness (0.0 = off, 1.0 = full brightness)
void setLedBrightness(int ledIndex, float brightness) {
  // Clamp brightness between 0 and 1
  brightness = (brightness < 0) ? 0 : (brightness > 1) ? 1 : brightness;
  
  // For low-side switching, 4095 is OFF and 0 is fully ON
  int dutyCycle = 4095 - (int)(4095 * brightness);
  ledcWrite(PWM_CHANNELS[ledIndex], dutyCycle);
}

// Calculate brightness during fade-in (cubic curve for ultra-soft start)
float calculateFadeInBrightness(float progress) {
  // Cubic curve makes the initial part of the transition much more gradual
  return progress * progress * progress;
}

// Calculate brightness during fade-out (quadratic curve)
float calculateFadeOutBrightness(float progress) {
  // Invert progress for fade-out (1.0 → 0.0)
  float invProgress = 1.0 - progress;
  // Quadratic curve for smooth fade-out
  return invProgress * invProgress;
}

// Function to make the test speed adjustable
void setTestSpeed(int speedValue) {
  // speedValue is in milliseconds, smaller = faster
  // Constrain between 50ms (very fast) and 500ms (very slow)
  testSpeed = constrain(speedValue, 50, 500);
  Serial.print("LED test speed set to: ");
  Serial.println(testSpeed);
}

// Random LED blink for testing
void randomLedBlink(int count, int speed) {
  Serial.println("Random LED blinking at double speed...");
  
  // Turn off all LEDs initially
  for (int i = 0; i < NUM_LEDS; i++) {
    setLedBrightness(i, 0);
  }
  
  // Perform random blinks
  for (int i = 0; i < count; i++) {
    // Select a random LED
    int ledIndex = random(NUM_LEDS);
    
    // Turn it on
    setLedBrightness(ledIndex, 1.0);
    delay(speed);
    
    // Turn it off
    setLedBrightness(ledIndex, 0.0);
    delay(speed / 2);  // Shorter off time for more dynamic effect
  }
  
  Serial.println("Random blinking complete.");
}

// Update state for a specific LED
void updateLedState(int ledIndex, unsigned long currentTime) {
  // If LED is in OFF state, just keep it off
  if (ledStates[ledIndex] == OFF) {
    setLedBrightness(ledIndex, 0);
    return;
  }

  // Normal state machine for active LEDs
  unsigned long elapsedTime = currentTime - stateStartTimes[ledIndex];
  float progress;
  
  switch (ledStates[ledIndex]) {
    case FADE_IN:
      if (elapsedTime < FADE_IN_DURATION) {
        progress = (float)elapsedTime / FADE_IN_DURATION;
        setLedBrightness(ledIndex, calculateFadeInBrightness(progress));
      } else {
        ledStates[ledIndex] = FULL_BRIGHTNESS;
        stateStartTimes[ledIndex] = currentTime;
        setLedBrightness(ledIndex, 1.0); // Full brightness
      }
      break;
      
    case FULL_BRIGHTNESS:
      if (elapsedTime >= FULL_BRIGHTNESS_DURATION) {
        ledStates[ledIndex] = FADE_OUT;
        stateStartTimes[ledIndex] = currentTime;
      }
      break;
      
    case FADE_OUT:
      if (elapsedTime < FADE_OUT_DURATION) {
        progress = (float)elapsedTime / FADE_OUT_DURATION;
        setLedBrightness(ledIndex, calculateFadeOutBrightness(progress));
      } else {
        // Instead of going to OFF state, start the FADE_IN again to create a continuous cycle
        ledStates[ledIndex] = FADE_IN;
        stateStartTimes[ledIndex] = currentTime;
      }
      break;
  }
}

// Test all LEDs in sequence
void runLedTest() {
  // First turn everything off
  for (int i = 0; i < NUM_LEDS; i++) {
    setLedBrightness(i, 0);
  }
  delay(300);
  
  Serial.println("=== STARTING LED TEST ===");
  Serial.println("Running building-up-and-down pattern followed by random blinks");
  
  // ======== BUILDING UP PATTERN ========
  Serial.println("Building UP: 25→25+26→25+26+27");
  
  // Start with all LEDs off
  for (int i = 0; i < NUM_LEDS; i++) {
    setLedBrightness(i, 0.0);
  }
  
  // Building up (progressively add LEDs)
  for (int i = 0; i < NUM_LEDS; i++) {
    Serial.print("Adding LED GPIO ");
    Serial.println(LED_PINS[i]);
    
    // Turn on current LED (keeping previous ones on)
    setLedBrightness(i, 1.0);
    delay(testSpeed);
  }
  
  // ======== BUILDING DOWN PATTERN ========
  Serial.println("Building DOWN: 25+26+27→25+26→25→off");
  
  // Building down (progressively remove LEDs)
  for (int i = NUM_LEDS - 1; i >= 0; i--) {
    Serial.print("Removing LED GPIO ");
    Serial.println(LED_PINS[i]);
    
    // Turn off current LED (keeping others on)
    setLedBrightness(i, 0.0);
    delay(testSpeed);
  }
  
  // Short pause after the pattern
  delay(300);
  
  // Random blinking
  Serial.println("Random blinking...");
  randomLedBlink(8, testSpeed / 4);
  
  Serial.println("=== TEST COMPLETE ===");
  delay(300);
}

void setup() {
  Serial.begin(115200);
  
  Serial.println("\n\nESP32 Lily T7 v1.5 - ESP-NOW LED Controller (Receiver) - Simple Version");
  Serial.println("--------------------------------------------------------------");
  
  // Configure PWM for all LEDs
  for (int i = 0; i < NUM_LEDS; i++) {
    ledcSetup(PWM_CHANNELS[i], PWM_FREQUENCY, PWM_RESOLUTION);
    ledcAttachPin(LED_PINS[i], PWM_CHANNELS[i]);
    setLedBrightness(i, 0); // Start with all LEDs off
  }
  
  // Initialize WiFi for ESP-NOW
  WiFi.mode(WIFI_STA);
  
  Serial.println("========================================");
  Serial.print("RECEIVER MAC Address: ");
  Serial.println(WiFi.macAddress());
  Serial.println("👆 COPY THIS MAC ADDRESS into your sender code! 👆");
  Serial.println("Format in code: ");
  
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char macStr[150];
  snprintf(macStr, sizeof(macStr), "uint8_t receiverMacAddress[] = {0x%02X, 0x%02X, 0x%02X, 0x%02X, 0x%02X, 0x%02X};",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  Serial.println(macStr);
  Serial.println("========================================");
  
  // Initialize ESP-NOW
  if (esp_now_init() != ESP_OK) {
    Serial.println("Error initializing ESP-NOW");
    return;
  }
  Serial.println("ESP-NOW initialized successfully");
  
  // Register callback function for received data
  esp_now_register_recv_cb(OnDataRecv);
  Serial.println("Ready to receive ESP-NOW messages");
  
  // Initialize the random number generator
  randomSeed(millis());
  
  // Run initial LED test with running up and down pattern
  runLedTest();
  
  // Very minimal power saving - just lower WiFi power slightly
  // This is safer and won't interfere with ESP-NOW
  esp_wifi_set_max_tx_power(40);  // Moderate power level (about 12dBm)
  
  Serial.println("Waiting for ESP-NOW commands...");
  Serial.println("LEDs will continuously fade in/out until new command is received");

  lastUpdateTime = millis();
}

void loop() {
  // Non-blocking LED update
  unsigned long currentTime = millis();
  
  // Only update at the specified interval for efficiency
  if (currentTime - lastUpdateTime >= UPDATE_INTERVAL) {
    // Update all LEDs
    for (int i = 0; i < NUM_LEDS; i++) {
      updateLedState(i, currentTime);
    }
    lastUpdateTime = currentTime;
  }
  
  // Print heartbeat every 10 seconds to show the device is still running
  static unsigned long lastHeartbeat = 0;
  if (currentTime - lastHeartbeat >= 10000) {
    Serial.println("Heartbeat - waiting for ESP-NOW commands...");
    lastHeartbeat = currentTime;
  }
  
  // Add a small delay to allow the ESP32 to yield to background tasks
  delay(1);
}