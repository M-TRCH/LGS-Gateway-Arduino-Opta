#!/usr/bin/env python3
"""Bench test for the gateway-run module OTA (console verbs OTA START / STATUS / RESULT / ABORT).

The image must already be in a depot slot (test/depot_test.py or the Test Tool's Firmware page). Nothing here moves a byte
of firmware: the gateway reads it from its own depot, drives the bus, and this script only starts, watches and reads back.

Modes:
  run         start, poll STATUS until it finishes, print RESULT
  picks       like run, but a second connection drives coil writes (light on / off) and register reads through the bridge the
              whole time; every request is timed, and a baseline is taken first, so the cost of the background update shows
  status      one STATUS
  result      RESULT of the latest run (--n 1 for the one before)
  abort       OTA ABORT

Usage (needs pymodbus >= 3.9; the Test Tool's venv has it):
  python test/ota_runner_test.py --host 192.168.99.111 --mode run --ids 11-14
"""
import argparse
import statistics
import struct
import sys
import threading
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


def connect(host, timeout=6.0):
    c = ModbusTcpClient(host=host, port=502, timeout=timeout, retries=0)
    c.register(GwRawPDU)
    if not c.connect():
        sys.exit(f"cannot connect to {host}:502")
    return c


def console(c, line):
    rsp = c.execute(False, GwRawPDU(0x01, line.encode("ascii")))
    if rsp.isError():
        return "(exception)"
    total, _, ln = struct.unpack(">HHB", rsp.payload[:5])
    text = bytearray(rsp.payload[5:5 + ln])
    t0 = time.monotonic()
    while len(text) < total and time.monotonic() - t0 < 10:
        r2 = c.execute(False, GwRawPDU(0x02, struct.pack(">H", len(text))))
        _, _, ln = struct.unpack(">HHB", r2.payload[:5])
        if ln == 0:
            break
        text += r2.payload[5:5 + ln]
    return text.decode("ascii", "replace")


def set_clock(c):
    # the gateway keeps LOCAL seconds; the bench is UTC+7 like the site
    console(c, f"TIME {int(time.time()) + 7 * 3600}")


def start(c, ids, extra):
    console(c, "HELLO ota-test")
    line = "OTA START slot=0" + (f" ids={ids}" if ids else "") + (" " + extra if extra else "")
    out = console(c, line)
    print(out.strip())
    return "#OK OTA START" in out


def status_line(c):
    out = console(c, "OTA STATUS")
    kv = {}
    for part in out.replace("#DATA", "").split():
        if "=" in part:
            k, v = part.split("=", 1)
            kv[k] = v
    return kv, out


def wait_done(c, label="", every=10.0, max_s=4 * 3600):
    t0 = time.monotonic()
    last = ""
    while time.monotonic() - t0 < max_s:
        kv, _ = status_line(c)
        line = (f"{time.strftime('%H:%M:%S')} {kv.get('state')} phase={kv.get('phase')} ch={kv.get('ch')} group={kv.get('group')} "
                f"chunk={kv.get('chunk')} paused_s={kv.get('paused_s')} pending={kv.get('pending')} verified={kv.get('verified')} "
                f"applied={kv.get('applied')} failed={kv.get('failed')}")
        if line != last:
            print(label + line)
            last = line
        if kv.get("state") != "running":
            return kv
        time.sleep(every)
    return {"state": "timeout"}


class Picker(threading.Thread):
    """What the hospital server does: light a slot, wait, clear it, read a register, repeat."""

    def __init__(self, host, ids, gap_s):
        super().__init__(daemon=True)
        self.c = ModbusTcpClient(host=host, port=502, timeout=8.0, retries=0)
        self.c.connect()
        self.ids, self.gap = ids, gap_s
        self.stop = threading.Event()
        self.lat, self.fail = [], 0
        self.phase = "baseline"
        self.bucket = {}

    def run(self):
        i = 0
        while not self.stop.is_set():
            dev = self.ids[i % len(self.ids)]
            i += 1
            for kind in ("on", "off", "read"):
                t0 = time.monotonic()
                try:
                    if kind == "read":
                        r = self.c.read_holding_registers(0, count=3, device_id=dev)
                    else:
                        r = self.c.write_coil(1001, kind == "on", device_id=dev)
                    ok = r is not None and not r.isError()
                except Exception:      # noqa: BLE001
                    ok = False
                dt = (time.monotonic() - t0) * 1000
                b = self.bucket.setdefault(self.phase, {"lat": [], "fail": 0})
                if ok:
                    b["lat"].append(dt)
                else:
                    b["fail"] += 1
                time.sleep(0.4)
            time.sleep(self.gap)


def report(picker):
    for phase, b in picker.bucket.items():
        lat = sorted(b["lat"])
        if not lat:
            print(f"  {phase}: no answers, {b['fail']} failed")
            continue
        p95 = lat[min(len(lat) - 1, int(len(lat) * 0.95))]
        print(f"  {phase:9s} n={len(lat):4d} failed={b['fail']:3d} median={statistics.median(lat):6.0f} ms "
              f"p95={p95:6.0f} ms max={lat[-1]:6.0f} ms")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", required=True)
    ap.add_argument("--mode", default="run", choices=["run", "picks", "storm", "status", "result", "abort"])
    ap.add_argument("--ids", default="", help="e.g. 11-14 (default: the whole cabinet)")
    ap.add_argument("--pick-ids", default="11-14,31-34", help="modules the picks mode drives through the bridge")
    ap.add_argument("--pick-gap", type=float, default=12.0, help="seconds of rest between two modules' pick cycles")
    ap.add_argument("--storm-s", type=float, default=130.0, help="storm mode: how long the bus is kept busy once streaming has begun")
    ap.add_argument("--extra", default="", help="extra START arguments, e.g. 'idle=3 group=4'")
    ap.add_argument("--n", type=int, default=0)
    ap.add_argument("--baseline-s", type=float, default=60.0)
    args = ap.parse_args()

    c = connect(args.host)
    if args.mode == "status":
        print(console(c, "OTA STATUS"))
    elif args.mode == "result":
        print(console(c, f"OTA RESULT {args.n}"))
    elif args.mode == "abort":
        console(c, "HELLO ota-test")
        print(console(c, "OTA ABORT"))
    else:
        set_clock(c)
        picker = None
        if args.mode == "picks":
            ids = []
            for part in args.pick_ids.split(","):
                a, _, b = part.partition("-")
                ids += list(range(int(a), int(b or a) + 1))
            picker = Picker(args.host, ids, args.pick_gap)
            picker.start()
            print(f"baseline for {args.baseline_s:.0f} s ...")
            time.sleep(args.baseline_s)
            picker.phase = "during"
        if not start(c, args.ids, args.extra):
            sys.exit(1)
        if args.mode == "storm":
            # wait until the image is being streamed, then keep the bus busy for longer than the module session timeout (90 s)
            while True:
                kv, _ = status_line(c)
                done_n = int((kv.get("chunk") or "0/0").split("/")[0])
                if kv.get("phase") == "stream" and done_n >= 100:
                    break
                time.sleep(2)
            ids = []
            for part in args.pick_ids.split(","):
                a, _, b = part.partition("-")
                ids += list(range(int(a), int(b or a) + 1))
            picker = Picker(args.host, ids, 0.05)
            picker.phase = "storm"
            picker.start()
            print(f"{time.strftime('%H:%M:%S')} streaming reached chunk {done_n}; keeping the bus busy for {args.storm_s:.0f} s")
            time.sleep(args.storm_s)
            picker.stop.set()           # the bus goes quiet; the runner has to notice that its modules timed out and go round again
            picker.join(10)
            storm = picker
            picker = None
        t0 = time.monotonic()
        final = wait_done(c)
        if picker:
            picker.stop.set()
            picker.join(5)
            print(f"update took {time.monotonic() - t0:.0f} s; picks through the bridge:")
            report(picker)
        if args.mode == "storm":
            print("picks through the bridge during the storm:")
            report(storm)
        print(console(c, "OTA RESULT 0"))
        print("FINAL:", final.get("state"))
    c.close()


if __name__ == "__main__":
    main()
