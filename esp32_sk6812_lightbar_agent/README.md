# ESP32 SK6812 LED Agent (for Windows)

This project is used to control the [ESP32 firmware for SK6812](../esp32).

It listens to Windows events like "sleep", "resume" and "shut down" and then forwards the commands to ESP32.

## How to use it?

1. Build the project using Rust (`cargo build --release`).
2. Create a shortcut to `target/release/esp32_sk6812_lightbar_agent.exe`.
3. Move the shortcut to `%appdata%/Microsoft/Windows/Start Menu/Programs/Startup`.
4. Restart your PC.

## Resolving common issues

If this agent/controller doesn't connect to the ESP32, open main.rs and remove/comment the first line:

```rust
//#![windows_subsystem = "windows"]
```

Rebuild the project (`cargo build`). Run the exe file.

Now you a terminal window should open, and you should see errors (if there are any).