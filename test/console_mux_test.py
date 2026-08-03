#!/usr/bin/env python3
"""Prove the $LGS console and the Modbus bridge cannot interfere with each other.

The console is fed only with bytes the bridge rejected as Modbus, so a valid
RTU frame must never reach the parser and a console reply must never appear
inside a Modbus response. This exercises that on real hardware.

Modes:
  modbus   read a register from each id, repeatedly  (regression gate)
  console  send $LGS PING repeatedly
  mux      alternate the two, checking for cross-talk (the decisive test)
  garbage  flood bad-CRC frames; the gateway must emit nothing at all

Usage (PlatformIO's Python has pyserial):
  C:/Users/mteer/.platformio/penv/Scripts/python.exe test/console_mux_test.py --mode mux --rounds 200
"""
import argparse
import random
import sys
import time

import serial
from serial.tools import list_ports

OPTA_VID, OPTA_PID = 0x2341, 0x0164
CONSOLE_BAUD = 115200


def find_opta():
    for p in list_ports.comports():
        if p.vid == OPTA_VID and p.pid == OPTA_PID:
            return p.device
    return None


def crc16(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def read_holding(slave: int, addr: int, count: int = 1) -> bytes:
    frame = bytes([slave, 0x03, addr >> 8, addr & 0xFF, count >> 8, count & 0xFF])
    c = crc16(frame)
    return frame + bytes([c & 0xFF, (c >> 8) & 0xFF])


def modbus_txn(ser, slave, addr=0, timeout=1.0):
    """Returns (ok, raw_reply)."""
    ser.reset_input_buffer()
    ser.write(read_holding(slave, addr))
    ser.flush()
    deadline = time.time() + timeout
    buf = b""
    while time.time() < deadline:
        chunk = ser.read(64)
        if chunk:
            buf += chunk
            if len(buf) >= 5 and time.time() > deadline - timeout + 0.15:
                break
        elif buf:
            break
    ok = len(buf) >= 5 and buf[0] == slave and buf[1] == 0x03 and crc16(buf) == 0
    return ok, buf


def console_txn(ser, line="PING", timeout=1.0):
    """Returns (ok, lines)."""
    ser.reset_input_buffer()
    ser.write(f"$LGS {line}\r\n".encode("ascii"))
    ser.flush()
    deadline = time.time() + timeout
    buf = b""
    while time.time() < deadline:
        chunk = ser.read(256)
        if chunk:
            buf += chunk
            if b"#OK" in buf or b"#ERR" in buf:
                if buf.rstrip().endswith(b"\r") or b"\n" in buf.split(b"#OK")[-1] \
                        or b"\n" in buf.split(b"#ERR")[-1]:
                    break
    text = buf.decode("ascii", "replace")
    lines = [x.strip() for x in text.splitlines() if x.strip()]
    ok = any(x.startswith("#OK") for x in lines)
    return ok, lines


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=None)
    ap.add_argument("--mode", default="mux",
                    choices=("modbus", "console", "mux", "garbage"))
    ap.add_argument("--cmd", default=None,
                    help="send one console command (e.g. INFO) and print the reply")
    ap.add_argument("--rounds", type=int, default=200)
    ap.add_argument("--ids", default="11,12,13,14")
    ap.add_argument("--seconds", type=float, default=120.0, help="garbage mode duration")
    args = ap.parse_args()

    port = args.port or find_opta()
    if not port:
        print("FAIL: no Opta found — pass --port")
        return 2
    ids = [int(x) for x in args.ids.split(",") if x.strip()]
    print(f"port {port} | mode {args.mode} | ids {ids}")

    ser = serial.Serial(port, CONSOLE_BAUD, timeout=0.05)
    stats = {"modbus_ok": 0, "modbus_bad": 0, "console_ok": 0, "console_bad": 0,
             "crosstalk": 0}

    try:
        if args.cmd:
            ok, lines = console_txn(ser, args.cmd, timeout=3.0)
            for line in lines:
                print(" ", line)
            print("RESULT:", "OK" if ok else "no #OK line")
            return 0 if ok else 1

        if args.mode == "garbage":
            # Bad-CRC frames must produce ZERO bytes back.
            end = time.time() + args.seconds
            sent = 0
            emitted = 0
            while time.time() < end:
                junk = bytes(random.randrange(256) for _ in range(8))
                ser.reset_input_buffer()
                ser.write(junk)
                ser.flush()
                sent += 1
                time.sleep(0.02)
                n = ser.in_waiting
                if n:
                    emitted += n
                    print(f"  UNEXPECTED {n} byte(s) after junk frame {sent}")
                    ser.reset_input_buffer()
            print(f"\nsent {sent} bad frames, gateway emitted {emitted} bytes")
            print("RESULT:", "PASS" if emitted == 0 else "FAIL")
            return 0 if emitted == 0 else 1

        for i in range(args.rounds):
            if args.mode in ("modbus", "mux"):
                slave = ids[i % len(ids)]
                ok, raw = modbus_txn(ser, slave)
                if ok:
                    stats["modbus_ok"] += 1
                else:
                    stats["modbus_bad"] += 1
                if b"#" in raw:                    # console text inside a Modbus reply
                    stats["crosstalk"] += 1
                    print(f"  CROSSTALK in modbus reply: {raw!r}")

            if args.mode in ("console", "mux"):
                ok, lines = console_txn(ser)
                if ok:
                    stats["console_ok"] += 1
                else:
                    stats["console_bad"] += 1
                    print(f"  console miss: {lines}")
                for line in lines:                 # binary inside a console reply
                    if not line.startswith(("#OK", "#ERR", "#DATA")):
                        stats["crosstalk"] += 1
                        print(f"  CROSSTALK in console reply: {line!r}")

            if (i + 1) % 25 == 0:
                print(f"  {i + 1}/{args.rounds}  {stats}")

        print(f"\n{stats}")
        good = stats["modbus_bad"] == 0 and stats["console_bad"] == 0 \
            and stats["crosstalk"] == 0
        print("RESULT:", "PASS" if good else "CHECK FAILURES ABOVE")
        return 0 if good else 1
    finally:
        ser.close()


if __name__ == "__main__":
    sys.exit(main())
