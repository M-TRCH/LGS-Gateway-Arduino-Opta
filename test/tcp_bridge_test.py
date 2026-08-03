#!/usr/bin/env python3
"""Exercise the Modbus TCP side of the gateway and prove it agrees with USB.

The two front ends share one RS485 bus and one synchronous transaction, so the
question worth answering is not "does TCP work" but "do TCP and USB return the
same thing, including when they are used at the same time".

Modes:
  read      read one register from each id over TCP
  compare   read over TCP and over USB, then diff        (the decisive test)
  soak      hammer TCP and USB concurrently from two threads
  port      check which TCP port answers

Usage (the Test Tool venv has pymodbus and pyserial):
  .venv/Scripts/python test/tcp_bridge_test.py --mode compare
"""
import argparse
import socket
import struct
import sys
import threading
import time

import serial
from serial.tools import list_ports

OPTA_VID, OPTA_PID = 0x2341, 0x0164


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


# ── Modbus TCP ─────────────────────────────────────────────────────────────
class TcpMaster:
    """A minimal MBAP client. pymodbus would work, but rolling the frame by
    hand keeps this test honest about what actually goes on the wire."""

    def __init__(self, host, port, timeout=2.0):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.settimeout(timeout)
        self.tid = 0

    def read_holding(self, unit, addr, count=1):
        self.tid = (self.tid + 1) & 0xFFFF
        pdu = struct.pack(">BBHH", unit, 0x03, addr, count)
        self.sock.sendall(struct.pack(">HHH", self.tid, 0, len(pdu)) + pdu)

        head = self._recv_exact(6)
        if head is None:
            return None
        tid, proto, length = struct.unpack(">HHH", head)
        body = self._recv_exact(length)
        if body is None or tid != self.tid or proto != 0:
            return None
        if body[1] & 0x80:                       # Modbus exception
            return ("exception", body[2])
        return body[3:]

    def _recv_exact(self, n):
        buf = b""
        while len(buf) < n:
            try:
                chunk = self.sock.recv(n - len(buf))
            except socket.timeout:
                return None
            if not chunk:
                return None
            buf += chunk
        return buf

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


# ── Modbus RTU over the USB bridge ─────────────────────────────────────────
def rtu_read_holding(ser, unit, addr, count=1, timeout=1.0):
    frame = struct.pack(">BBHH", unit, 0x03, addr, count)
    c = crc16(frame)
    ser.reset_input_buffer()
    ser.write(frame + bytes([c & 0xFF, (c >> 8) & 0xFF]))
    ser.flush()

    deadline = time.time() + timeout
    buf = b""
    while time.time() < deadline:
        chunk = ser.read(64)
        if chunk:
            buf += chunk
            if len(buf) >= 5 + buf[2]:
                break
        elif buf:
            break
    if len(buf) < 5 or crc16(buf) != 0 or buf[0] != unit:
        return None
    if buf[1] & 0x80:
        return ("exception", buf[2])
    return buf[3:-2]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="192.168.0.178")
    ap.add_argument("--port", type=int, default=502)
    ap.add_argument("--mode", default="compare",
                    choices=("read", "compare", "soak", "port"))
    ap.add_argument("--ids", default="11,12,13,14,15,16,17,18")
    ap.add_argument("--addr", type=int, default=0)
    ap.add_argument("--rounds", type=int, default=20)
    ap.add_argument("--seconds", type=float, default=60.0)
    args = ap.parse_args()
    ids = [int(x) for x in args.ids.split(",") if x.strip()]

    if args.mode == "port":
        for p in (502, 5020, args.port):
            try:
                s = socket.create_connection((args.host, p), timeout=2.0)
                s.close()
                print(f"  port {p}: OPEN")
            except OSError as exc:
                print(f"  port {p}: closed ({exc.__class__.__name__})")
        return 0

    if args.mode == "read":
        m = TcpMaster(args.host, args.port)
        bad = 0
        for i in ids:
            v = m.read_holding(i, args.addr)
            print(f"  id {i:3d} -> {v!r}")
            if v is None:
                bad += 1
        m.close()
        print(f"\n{len(ids) - bad}/{len(ids)} answered")
        return 0 if bad == 0 else 1

    if args.mode == "compare":
        port = find_opta()
        if not port:
            print("FAIL: no Opta USB port")
            return 2
        m = TcpMaster(args.host, args.port)
        ser = serial.Serial(port, 115200, timeout=0.05)
        mismatch = 0
        try:
            for i in ids:
                over_tcp = m.read_holding(i, args.addr)
                over_usb = rtu_read_holding(ser, i, args.addr)
                same = over_tcp == over_usb
                if not same:
                    mismatch += 1
                print(f"  id {i:3d}  tcp={over_tcp!r:20} usb={over_usb!r:20} "
                      f"{'ok' if same else 'MISMATCH'}")
        finally:
            ser.close()
            m.close()
        print(f"\n{len(ids) - mismatch}/{len(ids)} agree")
        print("RESULT:", "PASS" if mismatch == 0 else "FAIL")
        return 0 if mismatch == 0 else 1

    # soak: both front ends at once, for as long as asked
    port = find_opta()
    if not port:
        print("FAIL: no Opta USB port")
        return 2
    stats = {"tcp_ok": 0, "tcp_bad": 0, "usb_ok": 0, "usb_bad": 0}
    lock = threading.Lock()
    stop = threading.Event()

    def tcp_worker():
        m = TcpMaster(args.host, args.port)
        n = 0
        while not stop.is_set():
            v = m.read_holding(ids[n % len(ids)], args.addr)
            with lock:
                stats["tcp_ok" if v is not None else "tcp_bad"] += 1
            n += 1
        m.close()

    def usb_worker():
        ser = serial.Serial(port, 115200, timeout=0.05)
        n = 0
        while not stop.is_set():
            v = rtu_read_holding(ser, ids[n % len(ids)], args.addr)
            with lock:
                stats["usb_ok" if v is not None else "usb_bad"] += 1
            n += 1
        ser.close()

    threads = [threading.Thread(target=tcp_worker), threading.Thread(target=usb_worker)]
    for th in threads:
        th.start()
    end = time.time() + args.seconds
    while time.time() < end:
        time.sleep(2.0)
        with lock:
            print(f"  {stats}")
    stop.set()
    for th in threads:
        th.join(timeout=10)

    print(f"\n{stats}")
    good = stats["tcp_bad"] == 0 and stats["usb_bad"] == 0
    print("RESULT:", "PASS" if good else "CHECK FAILURES ABOVE")
    return 0 if good else 1


if __name__ == "__main__":
    sys.exit(main())
