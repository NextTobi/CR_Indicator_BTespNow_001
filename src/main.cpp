#include <Arduino.h>

// Pin and PWM configuration
const int NUM_LEDS = 3;
const int LED_PINS[NUM_LEDS] = {25, 26, 27};  // GPIO pins for the LEDs
const int PWM_CHANNELS[NUM_LEDS] = {0, 1, 2}; // Separate PWM channel for each LED
const int PWM_RESOLUTION = 12;                // 12-bit resolution (0-4095)
const int PWM_FREQUENCY = 5000;               // 5kHz frequency

// Timing configurations (in milliseconds)
const unsigned long FADE_IN_DURATION = 2000;   // Fade in over 2 seconds
const unsigned long FULL_BRIGHTNESS_DURATION = 800; // Hold full brightness for 800ms
const unsigned long FADE_OUT_DURATION = 1500;  // Fade out over 1.5 seconds
const unsigned long OFF_DURATION = 600;        // Hold off state for 600ms

// Very large offset to create distinct separation between LEDs
const unsigned long PHASE_OFFSET = 3000;       // 3 seconds between each LED

// Total cycle duration for a single LED
const unsigned long SINGLE_LED_CYCLE = FADE_IN_DURATION + FULL_BRIGHTNESS_DURATION + 
                                      FADE_OUT_DURATION + OFF_DURATION;

// Complete sequence duration for all LEDs with proper gaps
const unsigned long FULL_SEQUENCE_DURATION = SINGLE_LED_CYCLE + (2 * PHASE_OFFSET);


// Parameters for the running up and down test
const int TEST_SPEED_DEFAULT = 250;  // Default speed in milliseconds (smaller value = faster)
int testSpeed = TEST_SPEED_DEFAULT;  // Adjustable speed that can be changed

// LED states
enum LedState {
  FADE_IN,
  FULL_BRIGHTNESS,
  FADE_OUT,
  OFF,
  WAITING  // Wait state to explicitly control starting time
};

// Global variables
LedState ledStates[NUM_LEDS] = {FADE_IN, WAITING, WAITING}; // Only LED 25 starts active
unsigned long stateStartTimes[NUM_LEDS] = {0, 0, 0};
unsigned long ledStartTimes[NUM_LEDS] = {0, 0, 0};  // When each LED should begin its cycle
unsigned long lastUpdateTime = 0;
const unsigned long UPDATE_INTERVAL = 5; // Update PWM every 5ms for smooth transitions
unsigned long sequenceStartTime = 0;     // When the entire sequence started
int sequenceCount = 0;                   // Count of completed sequences
const int RANDOM_BLINK_COUNT = 8;  // Number of random blinks
const int RANDOM_BLINK_SPEED = TEST_SPEED_DEFAULT / 4;  // Double speed (half the delay time)

// Function prototypes
void resetSequence(unsigned long currentTime);
void setLedBrightness(int ledIndex, float brightness);
float calculateFadeInBrightness(float progress);
float calculateFadeOutBrightness(float progress);
void updateLedState(int ledIndex, unsigned long currentTime);
void runLedTest();
void scheduleNextCycle(int ledIndex, unsigned long currentTime);
void setTestSpeed(int speedValue);
void randomLedBlink(int count, int speed);

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

// Schedule the next activation for an LED to maintain proper sequence
void scheduleNextCycle(int ledIndex, unsigned long currentTime) {
  // Calculate when this LED should start its next cycle based on sequence position
  unsigned long nextStartTime;
  
  // Calculate the base sequence cycle time
  unsigned long baseSequenceTime = sequenceStartTime + (sequenceCount * FULL_SEQUENCE_DURATION);
  
  // Each LED's cycle start is offset from the base sequence time
  nextStartTime = baseSequenceTime + (ledIndex * PHASE_OFFSET);
  
  // If the calculated next start time is in the past, move to the next sequence
  if (nextStartTime <= currentTime) {
    sequenceCount++;
    baseSequenceTime = sequenceStartTime + (sequenceCount * FULL_SEQUENCE_DURATION);
    nextStartTime = baseSequenceTime + (ledIndex * PHASE_OFFSET);
  }
  
  // Set the LED to wait until its next scheduled time
  ledStartTimes[ledIndex] = nextStartTime;
  ledStates[ledIndex] = WAITING;
  
  // Turn LED off while waiting
  setLedBrightness(ledIndex, 0);
  
  Serial.print("LED ");
  Serial.print(LED_PINS[ledIndex]);
  Serial.print(" scheduled for next cycle at: ");
  Serial.print((nextStartTime - sequenceStartTime) / 1000.0);
  Serial.println(" seconds from sequence start");
}

// Reset the entire LED sequence to create a fresh start
void resetSequence(unsigned long currentTime) {
  Serial.println("Resetting the entire LED sequence");
  
  // Turn all LEDs off
  for (int i = 0; i < NUM_LEDS; i++) {
    setLedBrightness(i, 0);
  }
  
  // Reset sequence start time and count
  sequenceStartTime = currentTime;
  sequenceCount = 0;
  
  // Set up the sequence again
  ledStartTimes[0] = currentTime;
  ledStates[0] = FADE_IN;
  stateStartTimes[0] = currentTime;
  
  // LED 26 waits for its time
  ledStartTimes[1] = currentTime + PHASE_OFFSET;
  ledStates[1] = WAITING;
  
  // LED 27 waits even longer
  ledStartTimes[2] = currentTime + (2 * PHASE_OFFSET);
  ledStates[2] = WAITING;
  
  Serial.println("New LED sequence scheduled:");
  Serial.print("LED 25 starts at: 0ms\n");
  Serial.print("LED 26 starts at: ");
  Serial.print(PHASE_OFFSET);
  Serial.println("ms");
  Serial.print("LED 27 starts at: ");
  Serial.print(2 * PHASE_OFFSET);
  Serial.println("ms");
}

// Update state for a specific LED
void updateLedState(int ledIndex, unsigned long currentTime) {
  // Check if this LED is in waiting state and needs to be activated
  if (ledStates[ledIndex] == WAITING) {
    if (currentTime >= ledStartTimes[ledIndex]) {
      // Time to start this LED's cycle
      ledStates[ledIndex] = FADE_IN;
      stateStartTimes[ledIndex] = currentTime;
      Serial.print("Starting LED ");
      Serial.print(LED_PINS[ledIndex]);
      Serial.print(" at time ");
      Serial.print((currentTime - sequenceStartTime) / 1000.0);
      Serial.println(" seconds");
    } else {
      // Not time yet, keep LED off
      setLedBrightness(ledIndex, 0);
      return;
    }
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
        ledStates[ledIndex] = OFF;
        stateStartTimes[ledIndex] = currentTime;
        setLedBrightness(ledIndex, 0); // Off
      }
      break;
      
    case OFF:
      if (elapsedTime >= OFF_DURATION) {
        // Schedule the next activation for this LED
        scheduleNextCycle(ledIndex, currentTime);
        
        // If we've been running for a long time, occasionally do a full reset
        // to prevent any timing drift (roughly every 10 minutes)
        if (sequenceCount > 0 && sequenceCount % 100 == 0 && ledIndex == 0) {
          resetSequence(currentTime);
        }
      }
      break;
      
    default:
      // Should not get here
      break;
  }
}

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
  randomLedBlink(RANDOM_BLINK_COUNT, RANDOM_BLINK_SPEED);
  
  Serial.println("=== TEST COMPLETE ===");
  delay(300);
}

void setup() {
  Serial.begin(115200);
  Serial.println("ESP32 Lily T7 v1.5 - Continuous Sequential LED Pulse");
  
  // Configure PWM for all LEDs
  for (int i = 0; i < NUM_LEDS; i++) {
    ledcSetup(PWM_CHANNELS[i], PWM_FREQUENCY, PWM_RESOLUTION);
    ledcAttachPin(LED_PINS[i], PWM_CHANNELS[i]);
    setLedBrightness(i, 0); // Start with all LEDs off
  }
  
  randomSeed(analogRead(0));  // Initialize random number generator
  // Run initial LED test with running up and down pattern
  runLedTest();
  
  // Initialize the sequence
  unsigned long currentTime = millis();
  sequenceStartTime = currentTime;  // Record when the sequence starts
  sequenceCount = 0;
  
  // Schedule each LED to start at specific future times with equal spacing
  resetSequence(currentTime);
  
  lastUpdateTime = currentTime;
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
  
  // Your other code can run here without being blocked
  // ...
}