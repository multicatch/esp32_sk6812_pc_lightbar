#include <SK6812.h>

#define LED_COUNT 27
#define LED_PIN 4

SK6812 LED(LED_COUNT);

#define MAX_LIGHT 24
#define MIN_LIGHT 0
const float lightLevelCurve = 1.2f;
bool breatheDown = false;
uint8_t target = MAX_LIGHT;

void setup() {
  Serial.begin(115200);
  LED.set_output(LED_PIN); 
  
  for (int i = 0; i < LED_COUNT; i++) {
    LED.set_rgbw(0, {0, 0, 0, 0}); // clear 
  }
}

void loop() {
  int center = (LED_COUNT / 2) - 1;
  for (uint8_t i = 0; i < LED_COUNT; i++) {
    int distance = abs((int) i - center);
    uint8_t level = (uint8_t) max(target - distance, 0);
    uint8_t adjustedLevel = calculateAdjustedBrightness(level, lightLevelCurve);
    Serial.printf("%d: %d %d\n", i, level, adjustedLevel);
    LED.set_rgbw(i, {0, 0, 0, adjustedLevel});
  }
  LED.sync(); 
  
  delay(100);

  if (breatheDown) {
    if (target > MIN_LIGHT) {
      target -= 1;
    } else {
      delay(3000);
      breatheDown = false;
    }
  } else {
    if (target < MAX_LIGHT) {
      target += 1;
    } else {
      delay(1000);
      breatheDown = true;
    }
  }
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

