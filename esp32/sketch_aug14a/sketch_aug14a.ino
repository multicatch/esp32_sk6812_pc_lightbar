#include <SK6812.h>
#include "driver/usb_serial_jtag.h"

/////// LED STRIP SETTINGS //////
// how many LEDs you have on your strip
#define LED_COUNT 27
// PIN of data line of SK6812
#define LED_PIN 4

/////// ESP32 BEHAVIOR //////
// quiet period after turning off (time when the PC connection will be ignored after transitioning to "OFF" state)
#define TURN_OFF_QUIET_PERIOD 30000
// time after the ESP32 will turn off the LED if the connection is lost
#define AUTO_OFF_TIME 20000
// time to wait for agent to connect (after Windows startup)
#define WAIT_FOR_CONNECTION_TIME 90000
// timeout of USB connection; sometimes Windows stop talking to the USB and resume polling after a delay, this timeout defines how long to wait for next polling 
#define USB_TIMEOUT 10000
// turn off LEDs if agent on PC didn't connect (if true, then ESP32 will wait the above time and turn off; if false, the LEDs will "breathe" as long as the PC is connected)
const bool turnOffAfterFailedAgentConnection = true;

////// SLEEP ANIMATION SEETTINGS //////
// minimum light level of sleep animation (breathing)
#define SLEEP_MIN_LEVEL 0
// maximum light level of sleep animation (breathing)
#define SLEEP_MAX_LEVEL 6

// sleep animation will make a pause between a sequence of "breaths", this is the duration of this pause
#define SLEEP_BREATH_INTERVAL 5000
// sleep animation will make a few "breaths" and make a pause, this is the number of said "breaths" (for example, if you want a 3 consecutive breaths before a longer pause, set this to 3)
#define SLEEP_BREATH_COUNT 1

// how long to "hold" the breath animation on max light level (ms)
#define SLEEP_BREATHE_UP_PAUSE_TIME 700
// how long to "hold" the breath animation on min light level (ms)
#define SLEEP_BREATHE_DOWN_PAUSE_TIME 700

////// PENDING CONNECTION ANIMATION SETTINGS ////
// minimum light level during connection pending breathing
#define PENDING_MIN_LEVEL 5
// maximum light level during connection pending breathing
#define PENDING_MAX_LEVEL 7

// how long to "hold" the pending animation on breathe up/down 
#define PENDING_BREATH_PAUSE_TIME 100

////// GENERAL GLOW SETTINGS //////
// brightness curve - how the light "blends" (higher number = wider light, lower number = narrow light); CANNOT BE 0.0f OR LESS
const float lightLevelCurve = 1.2f;
// remove this if you want an unlocked fps (may cause the animation to be unstable/variable speed) - default 300 Hz/fps
#define FPS_LOCK 300
//// LED setup
SK6812 LED(LED_COUNT);

typedef struct LightLevel {
  uint8_t level;
  uint8_t sublevel;
} LightLevel_t;

LightLevel_t currentLevel = { 0, 0 };

//// ANIMATION 
int lastFrameTime = 0;
int frameCount = 0;
#ifdef FPS_LOCK
uint32_t nextFrameTime = 0;
constexpr uint32_t targetRenderInterval = 1'000'000 / FPS_LOCK;
#endif

bool breatheDown = false;
bool duringAnimation = false;

//// LED STATE
int lastCommunicationTime = 0;
int lastSerialConnectionTime = 0;
bool connectionAttemptFailed = false;

typedef enum LedState {
  OFF,
  PENDING_CON,
  SLEEP,
  WAKE,
} LedState_t;

LedState_t currentState = LedState::OFF;
LedState_t targetState = LedState::OFF;


//// MAIN PROGRAM
void setup() {
  Serial.begin(115200);
  LED.set_output(LED_PIN); 
  
  turnOff(); // clear
  
  lastCommunicationTime = millis();
  setState(LedState::OFF);
}

void loop() {
  bool commandReceived = false;
  if (Serial.available()) {
    int command = Serial.read();
    if (command != -1 && command != '\n') {
      connectionAttemptFailed = false;
      commandReceived = true;
      process_command(command);
    }
  }

  if (!commandReceived) { // no command received, assuming dead connection
    if (currentState == LedState::WAKE && (elapsedSinceCommand() > AUTO_OFF_TIME)) {
      setState(LedState::OFF);
    }
    if (currentState == LedState::PENDING_CON && (elapsedSinceCommand() > WAIT_FOR_CONNECTION_TIME) && turnOffAfterFailedAgentConnection) {
      connectionAttemptFailed = true;
      setState(LedState::OFF);
    }
  }

  bool pcConnected = usb_serial_jtag_is_connected();
  bool turnOffQuietPeriodEnded = elapsedSinceCommand() > TURN_OFF_QUIET_PERIOD;
  bool lightsTurnedOff = currentState == LedState::OFF && targetState == LedState::OFF;

  if (pcConnected) {
    lastSerialConnectionTime = millis();
  }

  if (!connectionAttemptFailed && pcConnected && turnOffQuietPeriodEnded && lightsTurnedOff) {
    // we connected to the USB data (probably Windows started USB polling), thus the PC is ON
    setState(LedState::PENDING_CON);
  } else if (!pcConnected && currentState == LedState::PENDING_CON && (millis() - lastSerialConnectionTime > USB_TIMEOUT)) {
    // we sensed USB connection (PENDING_CON) and then lost it (assuming the PC is turned off)
    if (turnOffAfterFailedAgentConnection) {
      connectionAttemptFailed = true;
    }
    setState(LedState::OFF);
  }

  if ((millis() - lastFrameTime) >= 10) { // we need to calculate frames every 10ms or more because we are dithering LEDs in between them
    nextFrame();
    lastFrameTime = millis();

    if (!duringAnimation && targetState != currentState) { // we transition the animation/state only after the frames are rendered
      Serial.printf("Setting state: %d\n", targetState);
      currentState = targetState;
    }
  }

  #ifdef FPS_LOCK
  uint32_t now = micros();
  if (now < nextFrameTime) {
    return;
  }
  nextFrameTime += targetRenderInterval;
  if (nextFrameTime <= now) { // we are lagging behind
    nextFrameTime = now + targetRenderInterval;
  }
  #endif

  renderWhiteGlow(currentLevel);
}


//// UTILS

void setState(LedState_t state) {
  targetState = state;
  lastCommunicationTime = millis();
}

void process_command(int command) {
  if (command == 'W') {
    setState(LedState::WAKE);
  } else if (command == 'S') {
    setState(LedState::SLEEP);
  } else if (command == 'Z') {
    setState(LedState::OFF);
  } else if (command == 'H') {
    Serial.println("ESP32-SK6812-LIGHTBAR-V1");
    setState(LedState::WAKE);
  }
}

int elapsedSinceCommand() {
  return millis() - lastCommunicationTime;
}

void nextFrame() {
  if (frameCount == INT_MAX) {
    frameCount = -1;
  }
  frameCount += 1;
  if (currentState != targetState) {
    breatheDownTo(0);
    resetAnimationCounters(); // reset counters for sleep/pending animation
    return;
  }

  if (shouldHoldAnimation()) {
    // an animation delay was scheduled, so we delay it
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
      nextSleepBreathingFrame(frameCount, SLEEP_MIN_LEVEL, SLEEP_MAX_LEVEL, SLEEP_BREATH_COUNT, SLEEP_BREATH_INTERVAL, SLEEP_BREATHE_DOWN_PAUSE_TIME, SLEEP_BREATHE_UP_PAUSE_TIME);
      break;

    case LedState::PENDING_CON:
      nextSleepBreathingFrame(frameCount, PENDING_MIN_LEVEL, PENDING_MAX_LEVEL, 1, 0, PENDING_BREATH_PAUSE_TIME, PENDING_BREATH_PAUSE_TIME);
      break;
  }
}

int scheduledDelayEnd = 0;
int sleepBreathCounter = 0;
int lastSleepBreathTime = 0;

void resetAnimationCounters() {
  sleepBreathCounter = 0;
  lastSleepBreathTime = 0;
  scheduledDelayEnd = 0;
}

// returns false if a non-blocking delay was scheduled
bool shouldHoldAnimation() {
  return scheduledDelayEnd > millis();
}

// non-blocking delay
void delayAnimation(uint32_t delayMillis) {
  scheduledDelayEnd = millis() + delayMillis;
}

// calculate next frame for "breathing" animation, which is a slow fade up (up to maxLevel) and down (down to minLevel)
void nextSleepBreathingFrame(
  int frameCount, 
  const int minLevel, 
  const int maxLevel, 
  const int maxBreathCount,
  const int breathInterval,
  const int breatheDownPauseTime, 
  const int breatheUpPauseTime
) {
  int elapsedSinceLastBreath = millis() - lastSleepBreathTime;

  if (minLevel >= maxLevel) {
    if (currentLevel.level < minLevel) {
      breatheUpTo(minLevel);
    } else {
      breatheDownTo(minLevel);
    }
    return;
  }

  if (sleepBreathCounter == 0 && elapsedSinceLastBreath < breathInterval) {
    return;
  }
  if (sleepBreathCounter <= 0) {
    sleepBreathCounter = maxBreathCount;
  }

  if (breatheDown) {
    if ((frameCount % 4) != 0) return; // breathe down is slower (25%)
    breatheDownTo(minLevel);
    if (!breatheDown) {
      lastSleepBreathTime = millis();
      sleepBreathCounter -= 1;
      delayAnimation(breatheDownPauseTime);
    }
  } else {
    if ((frameCount % 2) == 0) return; // slow down the animation (50%)
    breatheUpTo(maxLevel);
    if (currentLevel.level >= maxLevel) {
      breatheDown = true;
      delayAnimation(breatheUpPauseTime);
    }
  }
}

void breatheDownTo(uint8_t target) {
  if (target >= currentLevel.level && currentLevel.sublevel == 0) {
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
  if (target <= currentLevel.level) {
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
  if (currentLevel.level <= 0 && currentLevel.sublevel <= 0) {
    currentLevel.level = 0;
    currentLevel.sublevel = 0;
    return;
  }
  if (currentLevel.sublevel <= 0) {
    currentLevel.level -= 1;
    currentLevel.sublevel = 15;
  } else {
    currentLevel.sublevel -= 1;
  }
}

void whiteUp() {
  if (currentLevel.level >= 255) {
    currentLevel.sublevel = 0;
    currentLevel.level = 255;
    return;
  }
  currentLevel.sublevel += 1;
  if (currentLevel.sublevel >= 16) {
    currentLevel.level += 1;
    currentLevel.sublevel = 0;
  }
}

int ditherStep = 0;

void renderWhiteGlow(LightLevel_t targetLevel) {
  if (targetLevel.level == 255) {
    setWhiteGlowLevel(targetLevel.level);
    return;
  }
  // temporal sigma-delta dithering
  uint8_t normalizedSub = min(targetLevel.sublevel, (uint8_t) 15);
  ditherStep += normalizedSub;

  uint8_t adjustedLevel = targetLevel.level;
  if (ditherStep >= 16) {
    ditherStep -= 16;
    adjustedLevel += 1;
  }
  setWhiteGlowLevel(adjustedLevel);
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

