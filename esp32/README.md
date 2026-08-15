# ESP32 SK6812 driver

This folder contains source code for ESP32 firmware that will control and play animations on your LED strip (SK6812).

To build this sketch, you need to add [AxlRocket/ESP32_SK6812](https://github.com/AxlRocket/ESP32_SK6812) as dependency.

1. Download [repository](https://github.com/AxlRocket/ESP32_SK6812/) as .zip file (or use zip from this folder).
3. In Arduino IDE: Sketch -> Include Library -> Add .ZIP Library
4. Include the library in your project using "#include <SK6812.h>" directive

Select `ESP32-C3 DevKit` as your board and **enable USB CDC on Boot**
