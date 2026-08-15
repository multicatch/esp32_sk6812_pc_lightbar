# An SK6812 LED bar for PC (controlled by an ESP32-C3)

This project contains the source code for an ESP32-C3 controller and a Windows background agent/controller (that sends "sleep"/"resume"/"shut down" events to the ESP32).

The project assumes your USB port powers the ESP32 even if your PC is shut down.

## Subprojects

* [esp32 - Arduino IDE source code for ESP32-C3](./esp32)
* [esp32_sk6812_lightbar_agent - Rust code for a Windows app](./esp32_sk6812_lightbar_agent)

## Pictures of my implementation

