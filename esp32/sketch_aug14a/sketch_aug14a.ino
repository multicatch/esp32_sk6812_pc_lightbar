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

////// PC OFF SETTINGS /////
// color of the glow of the LED strip when the PC is OFF (RGBW value)
RGBW offColor = { 0, 0, 0, 0 };
// brightness of the LED strip when the PC if OFF as % (min: 0, max: 100.0)
#define OFF_BRIGHTNESS 0.0f

////// PC WAKE SETTINGS //////
// color of the glow of the LED strip when the PC is ON (RGBW value)
RGBW wakeColor = { 0, 0, 0, 10 };
// brightness of the LED strip when the PC is ON as % (min: 0, max: 100.0)
#define WAKE_BRIGHTNESS 100.0f

////// SLEEP ANIMATION SEETTINGS //////
// color of sleep glow as RGBW value
RGBW sleepColor = { 0, 0, 0, 10 };
// minimum light level of sleep animation (breathing) as brightness %
#define SLEEP_MIN_LEVEL 0.0f
// maximum light level of sleep animation (breathing) as brightness %
#define SLEEP_MAX_LEVEL 70.0f

// sleep animation will make a pause between a sequence of "breaths", this is the duration of this pause
#define SLEEP_BREATH_INTERVAL 5000
// sleep animation will make a few "breaths" and make a pause, this is the number of said "breaths" (for example, if you want a 3 consecutive breaths before a longer pause, set this to 3)
#define SLEEP_BREATH_COUNT 1

// how long to "hold" the breath animation on max light level (ms)
#define SLEEP_BREATHE_UP_PAUSE_TIME 500
// how long to "hold" the breath animation on min light level (ms)
#define SLEEP_BREATHE_DOWN_PAUSE_TIME 700


////// PENDING CONNECTION ANIMATION SETTINGS ////
// color of pending connection glow as RGBW value
RGBW pendingConnectionColor = { 0, 0, 0, 10 };
// minimum light level during connection pending breathing
#define PENDING_MIN_LEVEL 0.0f
// maximum light level during connection pending breathing
#define PENDING_MAX_LEVEL 100.0f

// how long to "hold" the pending animation on breathe up/down 
#define PENDING_BREATH_PAUSE_TIME 100


////// GENERAL GLOW SETTINGS //////
// how fast the animation should change the color
const float lightAnimationStep = 1.0f;
// a difference in light level between two "pixels" when rendering the glow; this value describes how wide or narrow the glow should be (bigger number = narrower light) 
const float lightPixelGlowStep = 10.0f;
// brightness curve - how the light "blends" (higher number = wider and dimmer, lower number = narrower and brighter); CANNOT BE 0.0f OR LESS
const float lightLevelCurve = 1.2f;
// remove this if you want an unlocked fps (may cause the animation to be unstable/variable speed) - default 300 Hz/fps
#define FPS_LOCK 300


//// LED setup
SK6812 LED(LED_COUNT);

//// RGBW utils
struct HSVColor {
  float h, s, v;
};

struct DitheredRGB {
  float r, g, b;
};

RGBW currentColor = { 0, 0, 0, 0 };
float currentLevel = 0.0f; // percentage value, max: 100.0f

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

  renderGlow(currentColor, currentLevel);
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
    if (breatheDownTo(0)) {
      switchColor();
    }
    resetAnimationCounters(); // reset counters for sleep/pending animation
    return;
  }

  if (shouldHoldAnimation()) {
    // an animation delay was scheduled, so we delay it
    return;
  }

  switch (currentState) {
    case LedState::OFF:
      breatheUpTo(OFF_BRIGHTNESS);
      break;

    case LedState::WAKE:
      breatheUpTo(WAKE_BRIGHTNESS);
      break;

    case LedState::SLEEP:
      nextSleepBreathingFrame(frameCount, SLEEP_MIN_LEVEL, SLEEP_MAX_LEVEL, SLEEP_BREATH_COUNT, SLEEP_BREATH_INTERVAL, SLEEP_BREATHE_DOWN_PAUSE_TIME, SLEEP_BREATHE_UP_PAUSE_TIME);
      break;

    case LedState::PENDING_CON:
      nextSleepBreathingFrame(frameCount, PENDING_MIN_LEVEL, PENDING_MAX_LEVEL, 1, 0, PENDING_BREATH_PAUSE_TIME, PENDING_BREATH_PAUSE_TIME);
      break;
  }
}

void switchColor() {
  switch (targetState) {
    case LedState::OFF:
      currentColor = offColor;
      break;
    case LedState::WAKE:
      currentColor = wakeColor;
      break;
    case LedState::SLEEP:
      currentColor = sleepColor;
      break;
    case LedState::PENDING_CON:
      currentColor = pendingConnectionColor;
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
  const float minLevel, 
  const float maxLevel, 
  const int maxBreathCount,
  const int breathInterval,
  const int breatheDownPauseTime, 
  const int breatheUpPauseTime
) {
  int elapsedSinceLastBreath = millis() - lastSleepBreathTime;

  if (minLevel >= maxLevel) {
    if (currentLevel < minLevel) {
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
    if (breatheDownTo(minLevel)) {
      lastSleepBreathTime = millis();
      sleepBreathCounter -= 1;
      delayAnimation(breatheDownPauseTime);
    }
  } else {
    if ((frameCount % 2) == 0) return; // slow down the animation (50%)
    if (breatheUpTo(maxLevel)) {
      breatheDown = true;
      delayAnimation(breatheUpPauseTime);
    }
  }
}

bool breatheDownTo(float target) {
  if (target >= currentLevel) {
    duringAnimation = false;
    breatheDown = false;
    return true;
  } else {
    duringAnimation = true;
    breatheDown = true; 
  }
  
  currentLevel = max(0.0f, currentLevel - lightAnimationStep);
  return currentLevel == 0.0f || currentLevel <= target;
}

bool breatheUpTo(float target) {
  if (target <= currentLevel) {
    duringAnimation = false;
    breatheDown = false;
    return true;
  } else {
    duringAnimation = true;
    breatheDown = false;
  }
  
  currentLevel = min(100.0f, currentLevel + lightAnimationStep);
  return currentLevel == 100.0f || currentLevel >= target;
} 

void turnOff() {
  for (int i = 0; i < LED_COUNT; i++) {
    LED.set_rgbw(i, {0, 0, 0, 0}); // clear 
  }
  LED.sync();
}

///// RENDER UTILS ////

// dither step for each pixel (rgbw = 4 channels) 
int ditherStep[LED_COUNT][4] = {0};

void renderGlow(RGBW color, float targetLevel) {
  int center = (LED_COUNT / 2) - 1;
  for (uint8_t i = 0; i < LED_COUNT; i++) {
    int distance = abs((int) i - center);
    float pixelLevel = max(targetLevel - (distance * lightPixelGlowStep), 0.0f);

    HSVColor hsv = rgbToHsv(currentColor);
    hsv.v = normalizeLightLevel(hsv.v, pixelLevel);
    DitheredRGB adjustedRGB = hsvToRgb(hsv);
    uint8_t r = dither(i, 0, adjustedRGB.r);
    uint8_t g = dither(i, 1, adjustedRGB.g);
    uint8_t b = dither(i, 2, adjustedRGB.b);
    uint8_t w = dither(i, 3, normalizeLightLevel(currentColor.w, pixelLevel));

    LED.set_rgbw(i, {r, g, b, w});
    //Serial.printf("%d %d %d %d %d\n", i, r, g, b, w);
  }
  LED.sync(); 
}

HSVColor rgbToHsv(RGBW color) {
  float r = color.r / 255.0f;
  float g = color.g / 255.0f;
  float b = color.b / 255.0f;
  
  float maxVal = max(r, max(g, b));
  float minVal = min(r, min(g, b));
  float delta = maxVal - minVal;

  float s;
  if (maxVal <= 0.0f) {
    s = 0.0f;
  } else {
    s = delta / maxVal;
  }

  float h;
  if (delta == 0.0f) {
    h = 0.0f;
  } else if (maxVal == r) {
    h = 60.0f * fmod((g - b) / delta, 6.0f);
  } else if (maxVal == g) {
    h = 60.0f * (((b - r) / delta) + 2.0f);
  } else {
    h = 60.0f * (((r - g) / delta) + 4.0f);
  }
  if (h < 0.0f) {
    h += 360.0f;
  }
  return { h, s, maxVal };
}

DitheredRGB hsvToRgb(HSVColor hsv) {
  float C = hsv.v * hsv.s;
  float X = C * (1.0f - fabs(fmod(hsv.h / 60.0f, 2.0f) - 1.0f));
  float m = hsv.v - C;

  if (hsv.h < 60.0f) {
    return finishRGBConv(C, X, 0.0f, m);
  } else if (hsv.h < 120.0f) {
    return finishRGBConv(X, C, 0.0f, m);
  } else if (hsv.h < 180.0f) {
    return finishRGBConv(0.0f, C, X, m);
  } else if (hsv.h < 240.0f) {
    return finishRGBConv(0.0f, X, C, m);
  } else if (hsv.h < 300.0f) {
    return finishRGBConv(X, 0.0f, C, m);
  } else {
    return finishRGBConv(C, 0.0f, X, m);
  }
}

DitheredRGB finishRGBConv(float r, float g, float b, float m) {
  return { (r + m) * 255.0f, (g + m) * 255.0f, (b + m) * 255.0f };
}

uint8_t dither(int i, int colorIndex, float value) {
  uint8_t floorValue = (uint8_t) value;
  int subLevel = (int) ((value - floorValue) * 100.0f);
  ditherStep[i][colorIndex] += subLevel;

  if (ditherStep[i][colorIndex] >= 100) {
    ditherStep[i][colorIndex] -= 100;
    return floorValue += 1;
  } else {
    return floorValue;
  }
}

float normalizeLightLevel(float color, float pixelLevel) {
  return calculateAdjustedBrightness((pixelLevel * color) / 100.0f, lightLevelCurve);
}

float normalizeLightLevel(uint8_t color, float pixelLevel) {
  return calculateAdjustedBrightness((pixelLevel * color) / 100.0f, lightLevelCurve);
}

float calculateAdjustedBrightness(const float brightness, const float dimCurve) {
  float xn = brightness / 255.0;
  float adjusted = 255.0 * pow(xn, dimCurve);
  float newBrightness = adjusted;
  if (brightness > 0 && newBrightness <= 0) {
    newBrightness = 0.1f;
  }
  return min(max(newBrightness, 0.0f), 255.0f);
}

