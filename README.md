# An SK6812 LED bar for PC (controlled by an ESP32-C3)

This project contains the source code for an ESP32-C3 controller and a Windows background agent/controller (that sends "sleep"/"resume"/"shut down" events to the ESP32).

The project assumes you have ports USB powered even if the PC is turned off (thus, the ESP32 firmware expects it will be powered 24/7). But it should work even if the PC cuts power to the ESP32 when turned off.

## Subprojects

* [esp32 - Arduino IDE source code for ESP32-C3](./esp32)
* [esp32_sk6812_lightbar_agent - Rust code for a Windows app](./esp32_sk6812_lightbar_agent)

## Pictures of my implementation

#### Turning off the computer
![Working example](./pic/lightbar.gif)

#### Sleep mode animation
![Sleep mode light](./pic/sleepmac.gif)

#### LED Strip (5V SK6812 with cold white)

Each "pixel" of this LED Strip is individually addressable. I used a 5V variant with 144 LEDs/1m. I’ve cut a ~19cm section (27 LEDs). Unfortunately it was still not smooth enough for a seamless "breathing" animation, but I implemented temporal dithering and now it looks nice and premium.

![SK6812 LED strip](./pic/sk6812.jpeg)

#### ESP32 installed inside (connected to internal USB header)

![ESP32-C3](./pic/esp32inside.jpeg)

I’ve wrapped it in elective tape after taking this picture, I don’t want the pins touching the aluminum case.

#### LED Light Profile

Not my photo, it’s a picture found on the internet. I bought mine in Obi and cut 19cm to fit it inside my PC case (the Mac Pro case). It blends the LEDs really nicely

![LED light channel](./pic/ledchannel.jpg)
