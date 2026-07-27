#!/usr/bin/env python3
"""FC05 Write Single Coil test through the Opta USB-RS485 bridge.

The Opta must be running in USB-RS485 bridge mode (blue USER LED on).
Builds a raw Modbus RTU frame, sends it over the Opta COM port, and
verifies the slave's echo response.

Usage (defaults: auto-detect Opta port, slave 11, coil 1020, value ON):
    python test/usb_bridge_test.py
    python test/usb_bridge_test.py --port COM21 --id 11 --coil 1020 --value 0

Requires pyserial (bundled with PlatformIO's Python):
    C:/Users/mteer/.platformio/penv/Scripts/python.exe test/usb_bridge_test.py
"""
import argparse
import sys
import time

import serial
from serial.tools import list_ports

OPTA_VID = 0x2341
OPTA_PID = 0x0164

EXCEPTION_CODES = {
    0x01: "illegal function",
    0x02: "illegal data address",
    0x03: "illegal data value",
    0x04: "slave device failure",
}


def crc16(data):
    """CRC-16/Modbus (poly 0xA001), same as the firmware's crc16()."""
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            if crc & 1:
                crc = (crc >> 1) ^ 0xA001
            else:
                crc >>= 1
    return crc


def build_fc05(slave_id, coil, value):
    frame = bytes([
        slave_id, 0x05,
        (coil >> 8) & 0xFF, coil & 0xFF,
        0xFF if value else 0x00, 0x00,
    ])
    crc = crc16(frame)
    return frame + bytes([crc & 0xFF, (crc >> 8) & 0xFF])


def find_opta():
    for p in list_ports.comports():
        if p.vid == OPTA_VID and p.pid == OPTA_PID:
            return p.device
    return None


def main():
    ap = argparse.ArgumentParser(description="FC05 write-coil test via the Opta USB-RS485 bridge")
    ap.add_argument("--port", default=None, help="COM port (default: auto-detect the Opta)")
    ap.add_argument("--id", type=int, default=11, help="slave ID (default 11)")
    ap.add_argument("--coil", type=int, default=1020, help="coil address (default 1020)")
    ap.add_argument("--value", type=int, default=1, choices=[0, 1], help="1 = ON, 0 = OFF (default 1)")
    ap.add_argument("--timeout", type=float, default=1.0, help="response timeout in seconds (default 1.0)")
    args = ap.parse_args()

    port = args.port or find_opta()
    if not port:
        print("FAIL: no Opta found (VID:PID 2341:0164) - specify --port")
        return 2

    tx = build_fc05(args.id, args.coil, args.value == 1)
    print("Port : %s" % port)
    print("TX   : %s  (slave=%d FC05 coil=%d value=%s)"
          % (tx.hex(" ").upper(), args.id, args.coil, "ON" if args.value else "OFF"))

    try:
        ser = serial.Serial(port, 9600, timeout=args.timeout)
    except serial.SerialException as e:
        print("FAIL: cannot open %s (%s)" % (port, e))
        print("hint: close any serial monitor holding the port and retry")
        return 2

    with ser:
        ser.reset_input_buffer()
        ser.write(tx)
        ser.flush()

        first = ser.read(1)
        if not first:
            print("FAIL: timeout - no response within %.1fs" % args.timeout)
            print("hint: check the slave is powered and wired on the RS485 bus,")
            print("      and the blue USER LED is on (USB-RS485 bridge mode)")
            return 1
        time.sleep(0.05)  # let the rest of the frame arrive
        rx = first + ser.read(ser.in_waiting)

    print("RX   : %s" % rx.hex(" ").upper())

    if len(rx) < 4:
        print("FAIL: response too short (%d bytes)" % len(rx))
        return 1
    if crc16(rx[:-2]) != (rx[-2] | (rx[-1] << 8)):
        print("FAIL: response CRC mismatch")
        return 1
    if rx[0] != args.id:
        print("FAIL: response from wrong slave id (%d)" % rx[0])
        return 1
    if rx[1] == 0x85:
        code = rx[2] if len(rx) > 2 else -1
        print("FAIL: Modbus exception 0x%02X (%s)" % (code, EXCEPTION_CODES.get(code, "unknown")))
        return 1
    if rx[1] != 0x05 or rx[:6] != tx[:6]:
        print("FAIL: unexpected response")
        return 1

    print("PASS: Write Coil OK - slave echoed the request")
    return 0


if __name__ == "__main__":
    sys.exit(main())
