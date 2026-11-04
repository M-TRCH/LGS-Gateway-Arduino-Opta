#!/usr/bin/env python3
"""Bench test for the module-image depot (unit 255 / FC 0x41, subs 0x20-0x24).

The depot keeps module firmware images on the OTA partition so a laptop can leave one in an Opta and the Test Tool can
read it back later (docs/design-module-depot.md). Nothing here touches the RS485 bus, a module, or UPDATE.BIN.

Modes:
  roundtrip   deposit a 61 KB image, list it, read it back byte for byte, delete it     (the regression gate)
  negative    locked writes, bad names, size limits, sequence mismatch, a bad CRC leaves no slot
  list        print the four slots (use it after a gateway update to see whether the depot survived)
  speed       time a full read-back

Usage (needs pymodbus >= 3.9; the Test Tool's venv has it):
  python test/depot_test.py --host 192.168.99.111 --mode roundtrip
"""
import argparse
import os
import struct
import sys
import time
import zlib

from pymodbus.client import ModbusTcpClient
from pymodbus.pdu import ModbusPDU

GW_SELF_UNIT = 255
SUB_BEGIN, SUB_DATA, SUB_LIST, SUB_READ, SUB_DELETE = 0x20, 0x21, 0x22, 0x23, 0x24
FWS = {0: "ok", 1: "locked", 2: "storage", 3: "too big", 5: "seq mismatch", 6: "not started", 7: "crc mismatch",
       11: "full", 12: "bad name", 13: "busy", 14: "not found"}


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


def call(c, sub, payload=b""):
    return c.execute(False, GwRawPDU(sub, payload))


def console(c, line):
    rsp = call(c, 0x01, line.encode("ascii"))
    total, _, ln = struct.unpack(">HHB", rsp.payload[:5])
    return bytes(rsp.payload[5:5 + ln]).decode("ascii", "replace")


def expect(name, cond, results):
    results[name] = bool(cond)
    print(f"  {'PASS' if cond else 'FAIL'}  {name}")


def begin(c, name, size, crc):
    r = call(c, SUB_BEGIN, struct.pack(">II", size, crc) + name.encode())
    if r.isError():
        return ("exc", getattr(r, "exception_code", -1), None)
    return (FWS.get(r.payload[0], r.payload[0]), r.payload[0], r.payload[1])


def upload(c, image, name):
    crc = zlib.crc32(image) & 0xFFFFFFFF
    st, _, slot = begin(c, name, len(image), crc)
    if st != "ok":
        return st, None
    off, last = 0, None
    while off < len(image):
        chunk = image[off:off + 240]
        r = call(c, SUB_DATA, struct.pack(">I", off) + chunk)
        status = r.payload[0]
        off += len(chunk)
        last = FWS.get(status, status)
        if status != 0:
            return last, slot
    return last, slot


def list_slots(c):
    out = []
    for s in range(4):
        r = call(c, SUB_LIST, bytes([s]))
        if r.isError():
            out.append(None)
            continue
        slot, state, size, crc, nl = struct.unpack(">BBIIB", r.payload[:11])
        out.append({"slot": slot, "state": state, "size": size, "crc": crc, "name": r.payload[11:11 + nl].decode()})
    return out


def read_back(c, slot, size):
    data = bytearray()
    while len(data) < size:
        r = call(c, SUB_READ, struct.pack(">BI", slot, len(data)))
        if r.isError() or r.payload[0] != 0 or r.payload[1] == 0:
            break
        data += r.payload[2:2 + r.payload[1]]
    return bytes(data)


def clear_depot(c):
    console(c, "HELLO depot-test")
    for s in list_slots(c):
        if s and s["state"]:
            call(c, SUB_DELETE, bytes([s["slot"]]))


def mode_roundtrip(c):
    results = {}
    expect("hello", "#OK HELLO" in console(c, "HELLO depot-roundtrip"), results)
    clear_depot(c)
    image = os.urandom(61304)                   # the size of module v3.5.5's OTA image
    t0 = time.monotonic()
    status, slot = upload(c, image, "module_v3.5.5_ota")
    print(f"  upload {len(image)} B in {time.monotonic() - t0:.1f} s")
    expect("upload ok", status == "ok", results)
    slots = list_slots(c)
    mine = next((s for s in slots if s and s["name"] == "module_v3.5.5_ota"), None)
    expect("listed complete", bool(mine) and mine["state"] == 2 and mine["size"] == len(image)
           and mine["crc"] == (zlib.crc32(image) & 0xFFFFFFFF), results)
    t0 = time.monotonic()
    back = read_back(c, mine["slot"], len(image)) if mine else b""
    print(f"  read back {len(back)} B in {time.monotonic() - t0:.1f} s")
    expect("read back identical", back == image, results)
    r = call(c, SUB_READ, struct.pack(">BI", mine["slot"], len(image) + 10))
    expect("read past the end is empty", r.payload[0] == 0 and r.payload[1] == 0, results)
    # a second image goes to a second slot, then replacing by name keeps one copy
    s2, slot2 = upload(c, os.urandom(2048), "other.bin")
    expect("second image in a second slot", s2 == "ok" and slot2 != mine["slot"], results)
    s3, slot3 = upload(c, os.urandom(4096), "other.bin")
    expect("same name replaces", s3 == "ok" and slot3 == slot2, results)
    r = call(c, SUB_DELETE, bytes([mine["slot"]]))
    expect("delete", r.payload[0] == 0, results)
    r = call(c, SUB_DELETE, bytes([mine["slot"]]))
    expect("delete twice -> not found", r.payload[0] == 14, results)
    call(c, SUB_DELETE, bytes([slot2]))
    expect("depot empty again", all(s and s["state"] == 0 for s in list_slots(c)), results)
    console(c, "BYE")
    return results


def mode_negative(c):
    results = {}
    console(c, "BYE")
    r = begin(c, "locked.bin", 4096, 0)
    expect("begin without HELLO is locked", r[1] == 1, results)
    r = call(c, SUB_DELETE, b"\x00")
    expect("delete without HELLO is locked", r.payload[0] == 1, results)
    console(c, "HELLO depot-negative")
    clear_depot(c)
    for bad in ("has space", "no/slash", "tab\tname", "x" * 25):
        r = begin(c, bad, 4096, 0)
        ok = r[0] == "exc" if len(bad) > 24 else r[1] == 12
        expect(f"bad name {bad!r}", ok, results)
    expect("too small", begin(c, "tiny", 100, 0)[1] == 3, results)
    expect("too big", begin(c, "huge", 200 * 1024, 0)[1] == 3, results)
    st, _, slot = begin(c, "seq.bin", 4096, 0)
    r = call(c, SUB_DATA, struct.pack(">I", 99) + b"x" * 16)
    expect("sequence mismatch names the resume offset", r.payload[0] == 5 and struct.unpack(">I", r.payload[1:5])[0] == 0, results)
    slots = list_slots(c)
    expect("a started slot lists as incomplete", slots[slot]["state"] == 1, results)
    r = call(c, SUB_READ, struct.pack(">BI", slot, 0))
    expect("an incomplete slot is not readable", r.payload[0] == 14, results)
    image = os.urandom(3000)
    st, slot = upload(c, image, "badcrc.bin")
    # now announce the wrong CRC
    st2, _, slot2 = begin(c, "wrongcrc.bin", len(image), 0x12345678)
    last = None
    for off in range(0, len(image), 240):
        r = call(c, SUB_DATA, struct.pack(">I", off) + image[off:off + 240])
        last = r.payload[0]
    expect("a wrong CRC is refused at the last chunk", last == 7, results)
    expect("and leaves no slot behind", list_slots(c)[slot2]["state"] == 0, results)
    clear_depot(c)
    r = call(c, SUB_LIST, b"\x09")
    expect("slot out of range is an exception", r.isError(), results)
    console(c, "BYE")
    return results


def mode_list(c):
    for s in list_slots(c):
        print("  ", s)
    return {"listed": True}


def mode_speed(c):
    results = {}
    slots = [s for s in list_slots(c) if s and s["state"] == 2]
    if not slots:
        print("  nothing deposited")
        return {"has an image": False}
    s = slots[0]
    t0 = time.monotonic()
    data = read_back(c, s["slot"], s["size"])
    dt = time.monotonic() - t0
    print(f"  {len(data)} B in {dt:.1f} s ({len(data) / dt / 1024:.0f} KB/s)")
    expect("crc matches the listing", (zlib.crc32(data) & 0xFFFFFFFF) == s["crc"], results)
    return results


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", required=True)
    ap.add_argument("--mode", default="roundtrip", choices=["roundtrip", "negative", "list", "speed"])
    args = ap.parse_args()
    c = connect(args.host)
    print(f"host {args.host} | mode {args.mode}")
    results = {"roundtrip": mode_roundtrip, "negative": mode_negative, "list": mode_list, "speed": mode_speed}[args.mode](c)
    c.close()
    ok = all(results.values())
    print(f"RESULT: {'PASS' if ok else 'FAIL'}")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
