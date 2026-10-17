# Changelog

## 0.1.2

- Status LED. The default image drives a single-colour LED on GPIO21 (Seeed XIAO
  ESP32S3); the `esp32-s3-devkitc-1` image drives a WS2812 on GPIO48. The LED
  shows whether a host is connected and blinks on input traffic.
- Release images for both LED variants.

## 0.1.1

- The BOS descriptor always carries a USB 2.0 Extension capability. Windows did
  not enumerate the device without it.

## 0.1.0

- First firmware: CDC control channel plus two slots (XInput, HID gamepad, or
  keyboard+mouse), re-enumeration on layout change, host watchdog.
- `tools/bridge_probe.py` bring-up tool and the browser flasher site.
