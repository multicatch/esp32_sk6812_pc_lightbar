#include <SK6812.h>
#include "driver/usb_serial_jtag.h"

#define LED_COUNT 27
#define LED_PIN 4

#define AUTO_OFF_TIME 30000
#define WAIT_FOR_CONNECTION_TIME 60000

#define SLEEP_MIN_LEVEL 0
#define SLEEP_MAX_LEVEL 8

SK6812 LED(LED_COUNT);

const float lightLevelCurve = 1.2f;

int currentLevel = 0;
int sublevel = 0;
int lastFrameTime = 0;
int frameCount = 0;

bool breatheDown = false;
bool duringAnimation = false;

int lastComm = 0;

typedef enum LedState {
  OFF,
  SLEEP,
  WAKE,
} LedState_t;

LedState_t currentState = LedState::OFF;
LedState_t targetState = LedState::OFF;

void setup() {
  Serial.begin(115200);
  LED.set_output(LED_PIN); 
  
  turnOff(); // clear
  
  lastComm = millis();
}

void loop() {
  if (Serial.available()) {
    int command = Serial.read();
    if (command != -1 && command != '\n') {
      lastComm = millis();
      process_command(command);
    }
  } else {
    if (currentState == LedState::WAKE && (elapsedSinceCommand() > AUTO_OFF_TIME)) {
      targetState = LedState::OFF;
    }
  }

  bool pcConnected = usb_serial_jtag_is_connected();
  if (pcConnected && elapsedSinceCommand() > WAIT_FOR_CONNECTION_TIME && currentState == LedState::OFF && targetState == LedState::OFF) {
    targetState = LedState::SLEEP;
    lastComm = millis();
  }

  if ((millis() - lastFrameTime) >= 10) {
    nextFrame();
    lastFrameTime = millis();
  }

  if (!duringAnimation && targetState != currentState) {
    Serial.printf("Setting state: %d\n", targetState);
    currentState = targetState;
  }

  renderWhiteGlow(currentLevel, sublevel);
}

void process_command(int command) {
  if (command == 'W') {
    targetState = LedState::WAKE;
  } else if (command == 'S') {
    targetState = LedState::SLEEP;
  } else if (command == 'Z') {
    targetState = LedState::OFF;
  } else if (command == 'H') {
    Serial.println("ESP32-SK6812-LIGHTBAR-V1");
  }
}

int elapsedSinceCommand() {
  return millis() - lastComm;
}

void nextFrame() {
  if (frameCount == INT_MAX) {
    frameCount = -1;
  }
  frameCount += 1;
  if (currentState != targetState) {
    breatheDownTo(0);
    return;
  }

  switch (currentState) {
    case LedState::OFF:
      breatheDownTo(0);
      break;

    case LedState::WAKE:
      breatheUpTo(12);
      break;

    case LedState::SLEEP:
      nextSleepBreathingFrame(frameCount);
      break;
  }
}

void nextSleepBreathingFrame(int frameCount) {
  const int minLevel = SLEEP_MIN_LEVEL;
  const int maxLevel = SLEEP_MAX_LEVEL;

  if (breatheDown) {
    if ((frameCount % 4) != 0) return; // breathe down is slower
    if (currentLevel == maxLevel) {
      delay(200);
      sublevel = 0;
    }
    breatheDownTo(minLevel);
    if (!breatheDown) {
      delay(500);
    }
  } else {
    if ((frameCount % 3) != 0) return; // slow down the animation 
    breatheUpTo(9);
    if (currentLevel >= maxLevel) {
      breatheDown = true;
    }
  }
}

void breatheDownTo(uint8_t target) {
  if (target >= currentLevel && sublevel == 0) {
    duringAnimation = false;
    breatheDown = false;
    return;
  } else {
    duringAnimation = true;
    breatheDown = true;
  }
  whiteDown();
}

void breatheUpTo(uint8_t target) {
  if (target <= currentLevel) {
    duringAnimation = false;
    breatheDown = false;
    return;
  } else {
    duringAnimation = true;
    breatheDown = false;
  }
  whiteUp();
} 

void turnOff() {
  for (int i = 0; i < LED_COUNT; i++) {
    LED.set_rgbw(i, {0, 0, 0, 0}); // clear 
  }
  LED.sync();
}

void whiteDown() {
  if (currentLevel <= 0 && sublevel <= 0) {
    currentLevel = 0;
    sublevel = 0;
    return;
  }
  if (sublevel <= 0) {
    currentLevel -= 1;
    sublevel = 9;
  } else {
    sublevel -= 1;
  }
}

void whiteUp() {
  if (currentLevel >= 255) {
    sublevel = 0;
    currentLevel = 255;
    return;
  }
  sublevel += 1;
  if (sublevel >= 10) {
    currentLevel += 1;
    sublevel = 0;
  }
}

void renderWhiteGlow(uint8_t targetLevel, uint8_t sublevel) {
  if (targetLevel == 255) {
    setWhiteGlowLevel(targetLevel);
    return;
  }
  // dithering
  uint8_t normalizedSub = min((uint8_t)10, sublevel);
  if (sublevel == 0 || (millis() % 10) > normalizedSub) {
    setWhiteGlowLevel(targetLevel);
  } else {
    setWhiteGlowLevel(targetLevel + 1);
  }
}

void setWhiteGlowLevel(uint8_t targetLevel) {
  int center = (LED_COUNT / 2) - 1;
  for (uint8_t i = 0; i < LED_COUNT; i++) {
    int distance = abs((int) i - center);
    uint8_t level = (uint8_t) max(targetLevel - distance, 0);
    uint8_t adjustedLevel = calculateAdjustedBrightness(level, lightLevelCurve);
    //Serial.printf("%d: %d %d\n", i, level, adjustedLevel);
    LED.set_rgbw(i, {0, 0, 0, adjustedLevel});
  }
  LED.sync(); 
}

uint8_t calculateAdjustedBrightness(const uint8_t brightness, const float dimCurve) {
  float xn = brightness / 255.0;
  float adjusted = 255.0 * pow(xn, dimCurve);
  uint8_t newBrightness = (uint8_t) adjusted;
  if (brightness > 0 && newBrightness == 0) {
    newBrightness = 1;
  }
  return newBrightness;
}

