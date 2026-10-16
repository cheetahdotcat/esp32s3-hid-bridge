# ESP32-S3 libvirtualhid USB bridge

USB gamepads, keyboard, and mouse for Sunshine (or any libvirtualhid consumer), provided by an ESP32-S3.
An ESP32-S3 plugged into the streaming host enumerates as **real USB controllers**.
Windows binds its inbox drivers to them (`xusb22` for XInput, `hidclass` for HID).

**Flash a board from your browser:** https://cheetahdotcat.github.io/esp32s3-hid-bridge/

```
Moonlight client ──network──▶ Sunshine ──libvirtualhid (BackendKind::esp32_bridge)
                                              │ CDC-ACM serial, same USB cable
                                              ▼
                                   ESP32-S3 native USB (GPIO19/20)
                                   ├─ IF0-1  CDC control channel
                                   ├─ slot 0 XInput, HID pad, or keyboard+mouse
                                   └─ slot 1 XInput, HID pad, or keyboard+mouse
```

## Layout

| Path | What |
|---|---|
| `libvirtualhid/src/platform/esp32/esp32_bridge_protocol.h` | Wire protocol shared by host and firmware (COBS + CRC-16 frames) |
| `libvirtualhid/src/platform/esp32/bridge_client.*` | Host protocol client: handshake, layout transactions, reconnect after re-enumeration, keepalive |
| `libvirtualhid/src/platform/esp32/esp32_backend.*` | libvirtualhid `Backend`: profile→slot mapping, XUSB packing, output parsing, keyboard/mouse devices |
| `libvirtualhid/src/platform/esp32/keyboard_mouse_reports.*` | Keyboard/mouse HID descriptor, VK→HID usage table, report packing, absolute-pointer scaling |
| `libvirtualhid/src/platform/esp32/serial_port.*` | POSIX/Win32 serial + auto-detection by USB serial `LVHB-*` |
| `libvirtualhid/tests/unit/test_esp32_bridge.cpp` | Tests against an in-memory firmware simulator |
| `firmware/` | PlatformIO (`framework = espidf`, ESP-IDF 5.3, TinyUSB) firmware |
| `tools/bridge_probe.py` | Bring-up tool that speaks the protocol without Sunshine |
| `site/` | Browser flasher (ESP Web Tools) and buying guide, deployed to GitHub Pages |
| `libvirtualhid/` | Submodule: [cheetahdotcat/libvirtualhid](https://github.com/cheetahdotcat/libvirtualhid/tree/esp32-bridge) `esp32-bridge` |

## Using it from libvirtualhid

```cpp
lvh::RuntimeOptions options;
options.backend = lvh::BackendKind::esp32_bridge;
options.esp32_bridge_port = "";  // auto-detect; or "COM5" / "/dev/ttyACM0"
auto runtime = lvh::Runtime::create(options);
auto pad = runtime->create_gamepad(lvh::profiles::xbox_360());  // re-enumerates the bridge
```

### Other consumers: automatic selection

A runtime created with `BackendKind::platform_default` switches to the bridge
on its own, unless `RuntimeOptions::esp32_bridge_autodetect` is `false`:

| Condition (checked in order)                       | Backend used      |
|----------------------------------------------------|-------------------|
| `LVH_BACKEND=platform`                             | platform          |
| `LVH_BACKEND=esp32_bridge`                         | bridge            |
| `LVH_ESP32_PORT=COM5` (or `esp32_bridge_port`) set | bridge, that port |
| a bridge with USB serial `LVHB-*` is attached      | bridge            |
| otherwise                                          | platform          |

Detection happens once, when the runtime is created. `lvh::find_esp32_bridge_ports()`
lists attached bridges.

### Sunshine

The [`esp32-bridge` branch of the Sunshine fork](https://github.com/cheetahdotcat/Sunshine/tree/esp32-bridge)
adds **ESP32-S3 USB Bridge** as a Windows gamepad backend (`gamepad_driver = esp32_bridge`):

- On first start with a bridge attached and no backend chosen, Sunshine selects it automatically.
- It routes gamepads, keyboard, and mouse through the bridge, disables the ViGEmBus fallback,
  and does not show Virtual HID Broker notices.
- `esp32_bridge_port` sets the COM port; leave it empty to auto-detect.
- Its CI builds a Windows installer (Actions → `build-Windows-AMD64` artifact).

Plug the bridge in before starting Sunshine. Sunshine creates its keyboard and mouse at
startup, so they take one bridge slot and leave one for a gamepad.

`create_keyboard()` and `create_mouse()` share one **keyboard+mouse slot**: a single HID
interface with a keyboard (report id 1), a relative mouse (id 2), and an absolute
pointer (id 3). The first of them enumerates the slot, and it is released when both
are closed. Unlike gamepad slots, these reports are events: the firmware queues
them in order, merges consecutive pointer moves when the 1 kHz endpoint is busy,
and never replays them after a reconnect.

- Keys are Windows virtual-key codes mapped to HID usages (US positions,
  6-key rollover). Windows does its own key repeat for held keys.
- `type_text()` types US-ASCII as keystrokes. Other text goes to the platform
  keyboard (Win32 `SendInput` Unicode), because a HID keyboard can only press keys.
- Absolute motion uses 0..32767 across the virtual desktop. The
  `CreateMouseOptions` desktop/viewport mapping matches the `SendInput` path.
- Scrolling accumulates 120 units per wheel detent; horizontal scrolling uses AC Pan.
- If both slots are already taken by gamepads, keyboard and mouse fall back to the
  platform backend with a logged warning. Create the keyboard and mouse first
  so they get a bridge slot.

Touch, trackpad, and pen requests always use the platform backend.

Profile mapping:

| Requested profile | Bridge slot | USB identity |
|---|---|---|
| Xbox 360 / One / Series, generic | XInput (`XUSB10` compatible ID via MS OS 2.0) | bridge default `303A:40F0` |
| DualSense (USB or BT) | HID, `dualsense_usb()` descriptor + cached feature reports 0x05/0x09/0x20 | `054C:0CE6` |
| DualShock 4 | HID, `dualshock4_usb()` descriptor + 0x02/0x12/0xA3 | `054C:09CC` |
| Switch Pro | HID; subcommand replies generated host-side | `057E:2009` |
| Keyboard + mouse | Keyboard/mouse HID (one shared slot) | no requirement |

## Limits (by design)

- **Two slots at most, and only one XInput.** The S3 OTG core allows
  5 IN endpoints including EP0 and 6 non-EP0 endpoints in total. CDC takes 3,
  XInput takes 2 (IN + OUT), and HID pads and keyboard+mouse take 1 each
  (output arrives via SET_REPORT). Working layouts:

  | Slot A         | Slot B                     |
  |----------------|----------------------------|
  | keyboard+mouse | —                          |
  | keyboard+mouse | any one gamepad            |
  | Xbox           | PlayStation or Switch      |
  | PlayStation    | same PlayStation family    |

  **With keyboard and mouse on the bridge, only one gamepad fits.**
- **One USB identity per device.** PlayStation/Switch pads need their real
  VID/PID for SDL/Steam to recognize them, so you can't mix DualSense with
  DualShock 4 or Switch Pro.
- **Any slot change re-enumerates the whole bridge.** Other pads blip for about 0.5–1 s.
  `create_gamepad` blocks until the bridge is back. The first time a new
  layout appears, Windows driver setup can take several seconds.

## Safety behavior

- The host pings every second. If the host is silent for 3 s, the firmware
  switches every slot to its neutral report, so a crashed host can't leave a
  button held.
- After 15 s of silence, the firmware drops all slots and re-enumerates as
  CDC-only. When the host comes back, it restores its layout and replays the
  latest input per slot. The same happens if the bridge resets or reboots.

## Firmware

```bash
cd firmware
~/.platformio/penv/bin/pio run                       # build
~/.platformio/penv/bin/pio run -t upload --upload-port /dev/ttyUSB0
```

The **native** USB port is the bridge. Once the firmware runs, TinyUSB owns that
port, so the ROM's USB-Serial/JTAG (`303a:1001`) disappears. To reflash, hold
**BOOT** while resetting or replugging, or use the board's separate UART port if
it has one. Firmware logs go to UART0 (GPIO43/44) at 115200 baud; bridge errors
also reach the host as protocol LOG frames.

Bring-up without Sunshine:

```bash
pip install pyserial
python3 tools/bridge_probe.py hello
python3 tools/bridge_probe.py xinput --seconds 20   # watch joy.cpl / gamepad-tester.com
python3 tools/bridge_probe.py kbm                   # types "hello", moves the cursor in a square
python3 tools/bridge_probe.py reset
```

## Status and what still needs hardware

Verified here:
- The firmware builds warning-free. The host backend and its tests build with
  `-Wall -Wextra -Wpedantic -Werror`, and all 29 tests pass, including under
  ThreadSanitizer with no races. A test walks the keyboard/mouse descriptor to
  confirm it declares exactly the report sizes the code sends.
- Windows sources compile for `x86_64-windows-gnu` (zig/MinGW).
- The Python probe and the C protocol header produce byte-identical frames.

Verified on hardware (ESP32-S3 rev v0.2, Linux host, 2026-10-16):
- Enumeration as CDC + slots; handshake; layout changes re-enumerate in about
  0.8–1.0 s and the host client reconnects on its own.
- XInput slot descriptors (interface `FF/5D/01`, EP IN 32 B @ 1 ms, EP OUT 32 B @ 8 ms).
- DualSense slot through the public libvirtualhid API: Linux `hid-playstation`
  accepted the cached feature reports, registered gamepad, motion, touchpad, and
  headset devices, and reported button and stick input correctly. Its lightbar
  and rumble output reports arrived at the `GamepadOutput` callback.
- Keyboard+mouse slot: enumerates as a `hid-generic` keyboard and mouse, and
  typed text arrives correctly in the focused window (`bridge_probe.py kbm`).

Not yet verified:
1. `xusb22` binding to an XInput interface inside a **composite** device via
   the MS OS 2.0 `XUSB10` compatible ID. Both the binding and the ID are known
   to work, but not tested together with CDC on this firmware.
2. DualSense/DS4 recognition by SDL/Steam when the device also carries a CDC interface.
3. Re-enumeration timing on Windows, and whether `usbser.sys` reports surprise
   removal promptly. The client reopens the port proactively and also has a
   4 s silence timeout.
4. Real input latency end to end.
5. Where Windows maps the absolute pointer on multi-monitor desktops (virtual
   desktop vs. primary display). Relative motion, buttons, scroll, and keys don't
   depend on this.
6. The firmware's pointer merging under a real 1 kHz host poll.
7. The full libvirtualhid build/test on Linux. It needs `libevdev`, `libinput`,
   and `sdl2` dev packages (not installed here). The ESP32 tests ran in a
   reduced harness that compiles only core and ESP32 sources.

Before distributing hardware, request a PID from Espressif (the default
`303A:40F0` is a placeholder), and consider the trademark implications of
the Sony/Nintendo VIDs used for HID personalities.
