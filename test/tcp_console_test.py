#!/usr/bin/env python3
"""Bench test for the gateway-self path: unit 255 / FC 0x41 on port 502.

Covers the console tunnel (EXEC + READ paging, session arming, locked
writes) and the firmware-staging state machine WITHOUT applying anything —
the dry run uploads a few chunks and aborts. The real update is driven from
the Test Tool (gw_net_update) or its scratch runner.

Modes:
  matrix     the console verb matrix over TCP           (the regression gate)
  fw-dry     BEGIN + a few DATA chunks + STATUS + ABORT (no APPLY, harmless)
  two-client Modbus polling on one socket while a second runs the console

Usage (needs pymodbus >= 3.9):
  python test/tcp_console_test.py --host 192.168.0.201 --mode matrix
"""
import argparse
import struct
import sys
import time

from pymodbus.client import ModbusTcpClient
from pymodbus.pdu import ModbusPDU

GW_SELF_UNIT = 255


class GwRawPDU(ModbusPDU):
    function_code = 0x41

    def __init__(self, sub=0, payload=b"", dev_id=GW_SELF_UNIT, transaction_id=0):
        super().__init__(dev_id=dev_id, transaction_id=transaction_id)
        self.sub = sub
        self.payload = bytes(payload)

    def encode(self):
        return bytes([self.sub]) + self.payload

    def decode(self, data):
        self.sub = data[0] if data else 0
        self.payload = bytes(data[1:])


def connect(host):
    c = ModbusTcpClient(host=host, port=502, timeout=3.5, retries=0)
    c.register(GwRawPDU)
    if not c.connect():
        sys.exit(f"cannot connect to {host}:502")
    return c


def console(c, line, timeout_s=5.0):
    rsp = c.execute(False, GwRawPDU(0x01, line.encode("ascii")))
    if rsp.isError():
        return None
    total, _, ln = struct.unpack(">HHB", rsp.payload[:5])
    text = bytearray(rsp.payload[5:5 + ln])
    deadline = time.monotonic() + timeout_s
    while len(text) < total and time.monotonic() < deadline:
        r2 = c.execute(False, GwRawPDU(0x02, struct.pack(">H", len(text))))
        _, _, ln = struct.unpack(">HHB", r2.payload[:5])
        if ln == 0:
            break
        text += r2.payload[5:5 + ln]
    return text.decode("ascii", "replace")


def expect(name, cond, results):
    results[name] = bool(cond)
    print(f"  {'PASS' if cond else 'FAIL'}  {name}")


def mode_matrix(c):
    results = {}
    expect("ping", "#OK PING" in (console(c, "PING") or ""), results)
    info = console(c, "INFO") or ""
    expect("info terminal", "#OK INFO" in info, results)
    expect("info multiline", info.count("#DATA") >= 10, results)
    got = console(c, "GET") or ""
    expect("get paged", "#OK GET" in got and len(got) > 1000, results)
    expect("set locked", "err=locked" in (console(c, "SET sys.name=x") or ""),
           results)
    expect("hello", "#OK HELLO" in (console(c, "HELLO tcp-test") or ""), results)
    expect("set staged", "#OK SET" in (console(c, "SET panel.bright=13") or ""),
           results)
    expect("staged visible",
           "staged=13" in (console(c, "GET panel.bright") or ""), results)
    expect("discard", "#OK DISCARD" in (console(c, "DISCARD") or ""), results)
    log = console(c, "LOG 30") or ""
    expect("log paged", "#OK LOG" in log, results)
    expect("bye", "#OK BYE" in (console(c, "BYE") or ""), results)
    return results


def mode_fw_dry(c):
    results = {}
    expect("hello", "#OK HELLO" in (console(c, "HELLO fw-dry") or ""), results)
    rsp = c.execute(False, GwRawPDU(0x12))
    state, received, size, capable, boot_ver = struct.unpack(
        ">BIIBB", rsp.payload[:11])
    print(f"  status: state={state} capable={capable} boot_ver={boot_ver}")
    fake = bytes(range(256)) * 300              # 76.8 KB of nothing
    rsp = c.execute(False, GwRawPDU(0x10, struct.pack(">II", len(fake), 0)))
    expect("begin ok", rsp.payload[0] == 0, results)
    off = 0
    for _ in range(4):
        chunk = fake[off:off + 240]
        rsp = c.execute(False, GwRawPDU(0x11, struct.pack(">I", off) + chunk))
        off += len(chunk)
    expect("data accepted", rsp.payload[0] == 0, results)
    # out-of-order chunk must be refused with the resume offset
    rsp = c.execute(False, GwRawPDU(0x11, struct.pack(">I", 99999) + b"x" * 16))
    expect("seq mismatch", rsp.payload[0] == 5
           and struct.unpack(">I", rsp.payload[1:5])[0] == off, results)
    rsp = c.execute(False, GwRawPDU(0x12))
    expect("receiving state", rsp.payload[0] == 1, results)
    rsp = c.execute(False, GwRawPDU(0x14))
    expect("abort", rsp.payload[0] == 0, results)
    rsp = c.execute(False, GwRawPDU(0x12))
    expect("idle after abort", rsp.payload[0] == 0, results)
    console(c, "BYE")
    return results


def mode_two_client(c, host, ids, rounds):
    results = {}
    c2 = connect(host)
    bad = cross = 0
    for i in range(rounds):
        dev = ids[i % len(ids)]
        r = c.read_holding_registers(0, count=3, device_id=dev)
        if r is None or r.isError():
            bad += 1
        text = console(c2, "PING") or ""
        if "#OK PING" not in text:
            cross += 1
    print(f"  {rounds} rounds: modbus_bad={bad} console_bad={cross}")
    expect("modbus clean", bad == 0, results)
    expect("console clean", cross == 0, results)
    c2.close()
    return results


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="192.168.0.201")
    ap.add_argument("--mode", default="matrix",
                    choices=["matrix", "fw-dry", "two-client"])
    ap.add_argument("--ids", default="11,12,13,14")
    ap.add_argument("--rounds", type=int, default=30)
    args = ap.parse_args()

    c = connect(args.host)
    print(f"host {args.host} | mode {args.mode}")
    if args.mode == "matrix":
        results = mode_matrix(c)
    elif args.mode == "fw-dry":
        results = mode_fw_dry(c)
    else:
        results = mode_two_client(c, args.host,
                                  [int(x) for x in args.ids.split(",")],
                                  args.rounds)
    c.close()
    ok = all(results.values())
    print(f"RESULT: {'PASS' if ok else 'FAIL'}")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
