# Changelog

## 0.2.0

- Host pairing (protocol version 2). The bridge only accepts input from paired
  hosts; pairing and every settings change are confirmed with the BOOT button.
  Up to four pairings; holding BOOT for 10 s removes all of them.
- The flasher site can list and remove pairings and turn the requirement off.
- New LED states for "waiting for BOOT" and "BOOT held".
- Hosts need libvirtualhid with protocol 2 (Sunshine `esp32-bridge` branch from
  this release on).

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
