#!/usr/bin/env python3
"""Does the gateway-run OTA reboot a polled module right AFTER its poll turn instead of just before the next one?

A reader polls the cabinet like a monitoring master (one pass over --poll-ids every --pass-gap s, reads only) while the gateway
runs --rounds module updates back to back. Every read slower than --slow-ms is listed with the time; the count is the number of
requests that landed in a module's reboot window. Before the rhythm-aware apply (v3) about 6 % of module reboots were hit.

Usage (Test Tool venv):  python test/ota_poll_apply_test.py --host 192.168.99.111 --rounds 6 --ids 11-14
"""
import argparse
import threading
import time

import ota_runner_test as t


def parse_ids(s):
    out = []
    for part in s.split(","):
        a, _, b = part.partition("-")
        out += list(range(int(a), int(b or a) + 1))
    return out


class Poller(threading.Thread):
    def __init__(self, host, ids, gap):
        super().__init__(daemon=True)
        self.c = t.ModbusTcpClient(host=host, port=502, timeout=8.0, retries=0)
        self.c.connect()
        self.ids, self.gap = ids, gap
        self.stop = threading.Event()
        self.n, self.fail, self.slow = 0, 0, []

    def run(self):
        while not self.stop.is_set():
            for dev in self.ids:
                t0 = time.monotonic()
                try:
                    r = self.c.read_holding_registers(0, count=3, device_id=dev)
                    ok = r is not None and not r.isError()
                except Exception:      # noqa: BLE001
                    ok = False
                ms = (time.monotonic() - t0) * 1000
                self.n += 1
                if not ok:
                    self.fail += 1
                    self.slow.append((time.strftime("%H:%M:%S"), dev, ms, "FAIL"))
                elif ms > self.args_slow:
                    self.slow.append((time.strftime("%H:%M:%S"), dev, ms, "slow"))
                if self.stop.is_set():
                    return
                time.sleep(0.05)
            self.stop.wait(self.gap)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", required=True)
    ap.add_argument("--ids", default="11-14")
    ap.add_argument("--poll-ids", default="11-14,21-24,31-34,41-44")
    ap.add_argument("--pass-gap", type=float, default=30.0)
    ap.add_argument("--slow-ms", type=float, default=1500.0)
    ap.add_argument("--rounds", type=int, default=6)
    ap.add_argument("--rest-s", type=float, default=60.0)
    ap.add_argument("--extra", default="")
    a = ap.parse_args()

    p = Poller(a.host, parse_ids(a.poll_ids), a.pass_gap)
    p.args_slow = a.slow_ms
    c = t.connect(a.host)
    p.start()
    time.sleep(a.pass_gap * 2 + 5)          # let the poller's rhythm be seen before the first run
    reboots = 0
    for rnd in range(1, a.rounds + 1):
        t0 = time.monotonic()
        if not t.start(c, a.ids, a.extra):
            break
        final = t.wait_done(c, label=f"[{rnd}] ", every=5.0)
        reboots += len(parse_ids(a.ids))
        print(f"round {rnd}: {final.get('state')} in {time.monotonic() - t0:.0f} s; slow reads so far: {len(p.slow)}", flush=True)
        time.sleep(a.rest_s)
    p.stop.set()
    p.join(15)
    print(f"reads={p.n} failed={p.fail} slow(>{a.slow_ms:.0f} ms)={len(p.slow)} module reboots={reboots}")
    for row in p.slow:
        print("  ", row)
    print("FINAL: done")
    c.close()


if __name__ == "__main__":
    main()
