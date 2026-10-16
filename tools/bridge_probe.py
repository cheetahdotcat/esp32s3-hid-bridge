#!/usr/bin/env python3
"""Bring-up tool for the ESP32-S3 libvirtualhid USB bridge.

Talks the bridge wire protocol directly (see
libvirtualhid/src/platform/esp32/esp32_bridge_protocol.h) so firmware can be
tested without Sunshine or libvirtualhid.

Examples:
    bridge_probe.py hello
    bridge_probe.py xinput --seconds 20     # one XInput pad; press A every second
    bridge_probe.py kbm                     # keyboard+mouse; types "hello" and draws a square
    bridge_probe.py reset                   # drop all slots
Requires pyserial (pip install pyserial).
"""

import argparse
import glob
import os
import struct
import sys
import time

import serial

PROTOCOL_VERSION = 1
SERIAL_PREFIX = "LVHB-"

MSG_HELLO = 0x01
MSG_LAYOUT_BEGIN = 0x02
MSG_LAYOUT_SLOT = 0x03
MSG_LAYOUT_COMMIT = 0x05
MSG_INPUT_REPORT = 0x10
MSG_PING = 0x11
MSG_HELLO_ACK = 0x81
MSG_ACK = 0x82
MSG_OUTPUT_REPORT = 0x90
MSG_STATUS = 0x91
MSG_PONG = 0x92
MSG_LOG = 0x93

SLOT_XINPUT = 1
SLOT_KEYBOARD_MOUSE = 3

# Must match keyboard_mouse_report_descriptor() in libvirtualhid/src/platform/esp32/keyboard_mouse_reports.cpp.
KEYBOARD_MOUSE_DESCRIPTOR = bytes.fromhex(
    "05010906a1018501050719e029e7150025017501950881027508950181010508190129057501950591027503950191010507190029e7150025e7750895068100c005010902a10185020901a1000509190129051500250175019505810275039501810105010930093116018026ff7f75109502810609381581257f750895018106050c0a38021581257f750895018106c0c005010902a10185030901a10005091901290515002501750195058102750395018101050109300931150026ff7f751095028102c0c0"
)


def crc16(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def cobs_encode(data: bytes) -> bytes:
    out = bytearray([0])
    code_index, code = 0, 1
    for byte in data:
        if byte == 0:
            out[code_index] = code
            code_index, code = len(out), 1
            out.append(0)
            continue
        out.append(byte)
        code += 1
        if code == 0xFF:
            out[code_index] = code
            code_index, code = len(out), 1
            out.append(0)
    out[code_index] = code
    return bytes(out) + b"\x00"


def cobs_decode(data: bytes) -> bytes:
    out, index = bytearray(), 0
    while index < len(data):
        code = data[index]
        index += 1
        if code == 0 or index + code - 1 > len(data):
            raise ValueError("bad COBS frame")
        out += data[index:index + code - 1]
        index += code - 1
        if code != 0xFF and index < len(data):
            out.append(0)
    return bytes(out)


def find_port() -> str:
    for tty in sorted(glob.glob("/sys/class/tty/ttyACM*")):
        serial_path = os.path.join(os.path.realpath(os.path.join(tty, "device")), "..", "serial")
        try:
            with open(serial_path, encoding="ascii") as handle:
                if handle.read().strip().startswith(SERIAL_PREFIX):
                    return "/dev/" + os.path.basename(tty)
        except OSError:
            continue
    try:
        from serial.tools import list_ports

        for port in list_ports.comports():
            if (port.serial_number or "").startswith(SERIAL_PREFIX):
                return port.device
    except ImportError:
        pass
    sys.exit("no bridge found; pass --port")


class Bridge:
    def __init__(self, port: str):
        self.port_name = port
        self.sequence = 0
        self.buffer = bytearray()
        self.open()

    def open(self, timeout: float = 15.0):
        deadline = time.monotonic() + timeout
        while True:
            try:
                self.port = serial.Serial(self.port_name, 115200, timeout=0.05)
                self.port.dtr = True
                self.buffer.clear()
                return
            except serial.SerialException:
                if time.monotonic() > deadline:
                    raise
                time.sleep(0.1)

    def send(self, message_type: int, payload: bytes = b"") -> int:
        sequence = self.sequence
        self.sequence = (self.sequence + 1) & 0xFF
        frame = bytes([message_type, sequence]) + payload
        frame += struct.pack("<H", crc16(frame))
        self.port.write(cobs_encode(frame))
        return sequence

    def frames(self, timeout: float):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                chunk = self.port.read(256)
            except serial.SerialException:
                return
            self.buffer += chunk
            while b"\x00" in self.buffer:
                encoded, _, rest = bytes(self.buffer).partition(b"\x00")
                self.buffer = bytearray(rest)
                if not encoded:
                    continue
                try:
                    frame = cobs_decode(encoded)
                except ValueError:
                    continue
                if len(frame) < 4 or crc16(frame[:-2]) != struct.unpack("<H", frame[-2:])[0]:
                    print("corrupt frame")
                    continue
                yield frame[0], frame[1], frame[2:-2]

    def request(self, message_type: int, payload: bytes, expect: int, timeout: float = 1.0) -> bytes:
        sequence = self.send(message_type, payload)
        for frame_type, _, body in self.frames(timeout):
            if frame_type == expect and (expect != MSG_ACK or (body[0] == message_type and body[1] == sequence)):
                return body
            self.show(frame_type, body)
        raise TimeoutError(f"no reply to message 0x{message_type:02x}")

    def hello(self) -> int:
        body = self.request(MSG_HELLO, struct.pack("<H", PROTOCOL_VERSION), MSG_HELLO_ACK)
        version, max_slots, mounted, generation, max_descriptor = struct.unpack("<HBBIH", body[:10])
        print(f"bridge {self.port_name}: protocol {version}, firmware {body[10:].decode(errors='replace')}, "
              f"{max_slots} slots, mounted={mounted}, generation={generation}, max descriptor {max_descriptor}")
        return generation

    def apply(self, slots, generation: int):
        name = b"ESP32-S3 HID Bridge"
        begin = struct.pack("<HHH", 0x303A, 0x40F0, 0x0100) + bytes([13]) + b"libvirtualhid" + bytes([len(name)]) + name
        self.check(self.request(MSG_LAYOUT_BEGIN, begin, MSG_ACK))
        for index, (kind, descriptor, neutral) in enumerate(slots):
            payload = bytes([index, kind, 1]) + struct.pack("<H", len(descriptor)) + descriptor + neutral
            self.check(self.request(MSG_LAYOUT_SLOT, payload, MSG_ACK))
        self.check(self.request(MSG_LAYOUT_COMMIT, struct.pack("<I", generation), MSG_ACK))
        print("committed; waiting for re-enumeration")
        self.port.close()
        time.sleep(0.5)
        self.open()
        if self.hello() != generation:
            sys.exit("bridge came back with a different layout generation")

    @staticmethod
    def check(ack: bytes):
        if ack[2] != 0:
            sys.exit(f"bridge rejected message 0x{ack[0]:02x} with status {ack[2]}")

    @staticmethod
    def show(frame_type: int, body: bytes):
        if frame_type == MSG_OUTPUT_REPORT:
            print(f"output slot {body[0]} kind {body[1]}: {body[2:].hex(' ')}")
        elif frame_type == MSG_LOG:
            print("bridge log:", body.decode(errors="replace"))
        elif frame_type == MSG_STATUS:
            mounted, suspended, generation = struct.unpack("<BBI", body[:6])
            print(f"status: mounted={mounted} suspended={suspended} generation={generation}")


def xinput_report(buttons_low: int = 0, buttons_high: int = 0) -> bytes:
    return bytes([0x00, 0x14, buttons_low, buttons_high]) + bytes(16)


def keyboard_report(modifiers: int = 0, key: int = 0) -> bytes:
    return bytes([1, modifiers, 0, key, 0, 0, 0, 0, 0])


def mouse_report(dx: int = 0, dy: int = 0) -> bytes:
    return bytes([2, 0]) + struct.pack("<hhbb", dx, dy, 0, 0)


def keyboard_mouse_demo(bridge: "Bridge", generation: int):
    neutral = bytes([9]) + keyboard_report() + bytes([8]) + mouse_report()
    bridge.apply([(SLOT_KEYBOARD_MOUSE, KEYBOARD_MOUSE_DESCRIPTOR, neutral)], generation)
    print("keyboard+mouse active: focus a text editor within 3 seconds")
    time.sleep(3)
    for character in "hello":
        bridge.send(MSG_INPUT_REPORT, bytes([0]) + keyboard_report(0, 0x04 + ord(character) - ord("a")))
        bridge.send(MSG_INPUT_REPORT, bytes([0]) + keyboard_report())
        time.sleep(0.05)
    for dx, dy in ((200, 0), (0, 200), (-200, 0), (0, -200)):
        for _ in range(20):
            bridge.send(MSG_INPUT_REPORT, bytes([0]) + mouse_report(dx // 20, dy // 20))
            time.sleep(0.01)
    for frame_type, _, body in bridge.frames(0.5):
        if frame_type != MSG_PONG:
            Bridge.show(frame_type, body)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("command", choices=["hello", "xinput", "kbm", "reset"])
    parser.add_argument("--port", help="serial port (default: auto-detect by USB serial " + SERIAL_PREFIX + "*)")
    parser.add_argument("--seconds", type=float, default=15.0)
    args = parser.parse_args()

    bridge = Bridge(args.port or find_port())
    generation = bridge.hello()
    next_generation = (generation + 1) & 0xFFFFFFFF or 1

    if args.command == "reset":
        bridge.apply([], next_generation)
    elif args.command == "kbm":
        keyboard_mouse_demo(bridge, next_generation)
    elif args.command == "xinput":
        bridge.apply([(SLOT_XINPUT, b"", xinput_report())], next_generation)
        print("XInput pad active: open joy.cpl or gamepad-tester.com; A toggles every second")
        end = time.monotonic() + args.seconds
        pressed = False
        while time.monotonic() < end:
            pressed = not pressed
            bridge.send(MSG_INPUT_REPORT, bytes([0]) + xinput_report(0, 0x10 if pressed else 0))
            bridge.send(MSG_PING, struct.pack("<I", int(time.monotonic())))
            for frame_type, _, body in bridge.frames(1.0):
                if frame_type != MSG_PONG:
                    Bridge.show(frame_type, body)
        bridge.send(MSG_INPUT_REPORT, bytes([0]) + xinput_report())


if __name__ == "__main__":
    main()
