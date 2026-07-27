#!/usr/bin/env python3
"""Coil-toggle test through the Opta USB-RS485 bridge.

The Opta must be running in USB-RS485 bridge mode (blue USER LED on).
Alternates two FC05 Write Single Coil commands on the target slave:

    coil 1021 = ON   ->  wait interval  ->  coil 1001 = OFF  ->  repeat

every 2000 ms by default, printing one line per transaction. Stop with
Ctrl+C (a summary is printed), or limit the rounds with --count.

Usage (defaults: auto-detect Opta port, slave 11, 1021/ON <-> 1001/OFF, 2000 ms):
    python test/usb_bridge_test.py
    python test/usb_bridge_test.py --count 10
    python test/usb_bridge_test.py --port COM21 --id 11 --coil-on 1021 --coil-off 1001 --interval-ms 2000

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


def transact(ser, tx):
    """Send one RTU frame, return the response bytes or None on timeout."""
    ser.reset_input_buffer()
    ser.write(tx)
    ser.flush()
    first = ser.read(1)
    if not first:
        return None
    time.sleep(0.05)  # let the rest of the frame arrive
    return first + ser.read(ser.in_waiting)


def check_response(tx, rx, slave_id):
    """Return 'OK' or a short error description."""
    if len(rx) < 4:
        return "response too short (%d bytes)" % len(rx)
    if crc16(rx[:-2]) != (rx[-2] | (rx[-1] << 8)):
        return "response CRC mismatch"
    if rx[0] != slave_id:
        return "response from wrong slave id (%d)" % rx[0]
    if rx[1] == 0x85:
        code = rx[2] if len(rx) > 2 else -1
        return "Modbus exception 0x%02X (%s)" % (code, EXCEPTION_CODES.get(code, "unknown"))
    if rx[1] != 0x05 or rx[:6] != tx[:6]:
        return "unexpected response"
    return "OK"


def main():
    ap = argparse.ArgumentParser(description="FC05 coil-toggle test via the Opta USB-RS485 bridge")
    ap.add_argument("--port", default=None, help="COM port (default: auto-detect the Opta)")
    ap.add_argument("--id", type=int, default=11, help="slave ID (default 11)")
    ap.add_argument("--coil-on", type=int, default=1021, help="coil written ON (default 1021)")
    ap.add_argument("--coil-off", type=int, default=1001, help="coil written OFF (default 1001)")
    ap.add_argument("--interval-ms", type=int, default=2000, help="ms between commands (default 2000)")
    ap.add_argument("--count", type=int, default=0, help="number of commands to send (0 = run until Ctrl+C)")
    ap.add_argument("--timeout", type=float, default=1.0, help="response timeout in seconds (default 1.0)")
    args = ap.parse_args()

    port = args.port or find_opta()
    if not port:
        print("FAIL: no Opta found (VID:PID 2341:0164) - specify --port")
        return 2

    try:
        ser = serial.Serial(port, 9600, timeout=args.timeout)
    except serial.SerialException as e:
        print("FAIL: cannot open %s (%s)" % (port, e))
        print("hint: close any serial monitor holding the port and retry")
        return 2

    print("Port %s | slave %d | coil %d=ON <-> coil %d=OFF | every %d ms | %s"
          % (port, args.id, args.coil_on, args.coil_off, args.interval_ms,
             ("%d commands" % args.count) if args.count else "until Ctrl+C"))

    sent = ok = timeouts = errors = 0
    try:
        with ser:
            while args.count == 0 or sent < args.count:
                t0 = time.time()
                if sent % 2 == 0:
                    coil, value = args.coil_on, True
                else:
                    coil, value = args.coil_off, False
                tx = build_fc05(args.id, coil, value)
                rx = transact(ser, tx)
                sent += 1

                stamp = time.strftime("%H:%M:%S")
                label = "coil %4d = %-3s" % (coil, "ON" if value else "OFF")
                if rx is None:
                    timeouts += 1
                    print("[%s] #%03d  %s  TX %s  -> TIMEOUT" % (stamp, sent, label, tx.hex(" ").upper()))
                else:
                    verdict = check_response(tx, rx, args.id)
                    if verdict == "OK":
                        ok += 1
                        print("[%s] #%03d  %s  RX %s  -> OK" % (stamp, sent, label, rx.hex(" ").upper()))
                    else:
                        errors += 1
                        print("[%s] #%03d  %s  RX %s  -> FAIL: %s"
                              % (stamp, sent, label, rx.hex(" ").upper(), verdict))

                elapsed = time.time() - t0
                time.sleep(max(0.0, args.interval_ms / 1000.0 - elapsed))
    except KeyboardInterrupt:
        print()

    print("Summary: sent=%d ok=%d timeout=%d error=%d" % (sent, ok, timeouts, errors))
    return 0 if sent > 0 and ok == sent else 1


if __name__ == "__main__":
    sys.exit(main())
