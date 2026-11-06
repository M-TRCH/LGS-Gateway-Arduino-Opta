"""Service AP bench test over the LAN only (docs/design-service-ap.md, "Test plan").

The PC running this must NOT join the AP (its own Wi-Fi is its route to the bench and to the internet), so everything
here is seen from the gateway's side and from the LAN: the AP's state as the gateway reports it, Modbus over the LAN
while the AP is up, the page NOT being served to the LAN, SWEEP, repeated ON/OFF, and a long run.

    python test/service_ap_test.py --host 192.168.99.111 --mode basic
    python test/service_ap_test.py --host 192.168.99.111 --mode cycles --n 50
    python test/service_ap_test.py --host 192.168.99.111 --mode soak --minutes 480

Needs the Test Tool's venv (it uses app.v2.gateway_api for the unit 255 console):
    ..\\LGS-Test-Tool\\.venv\\Scripts\\python.exe test\\service_ap_test.py ...
"""
from __future__ import annotations

import argparse
import os
import socket
import struct
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "LGS-Test-Tool"))
from app.v2.gateway_api import _Session  # noqa: E402

PASS = os.environ.get("LGS_AP_PASS", "lgs-bench-2026")


def ts() -> str:
    return time.strftime("%H:%M:%S")


def log(*a) -> None:
    print(ts(), *a, flush=True)


def info(link) -> dict:
    d: dict = {}
    for row in link.command("INFO").rows:
        d.update(row)
    return d


def ap_line(link) -> str:
    return " | ".join(link.command("AP").lines[:2])


class Modbus:
    """One persistent Modbus TCP connection, as a site server keeps one (a fresh socket per read churns the gateway's
    two client slots and measures the test, not the gateway)."""

    def __init__(self, host: str, timeout: float = 6.0) -> None:
        self.host, self.timeout, self.s, self.tid = host, timeout, None, 0

    def _open(self) -> None:
        self.s = socket.create_connection((self.host, 502), timeout=self.timeout)
        self.s.settimeout(self.timeout)

    def close(self) -> None:
        if self.s:
            try:
                self.s.close()
            except OSError:
                pass
        self.s = None

    def read(self, unit: int, reg: int = 0, n: int = 1):
        """FC03 -> (ok, ms, detail). Reconnects once on a dead socket."""
        t0 = time.time()
        for attempt in (1, 2):
            try:
                if self.s is None:
                    self._open()
                self.tid = (self.tid + 1) & 0xFFFF
                self.s.sendall(struct.pack(">HHHBBHH", self.tid, 0, 6, unit, 3, reg, n))
                data = b""
                while len(data) < 9:
                    chunk = self.s.recv(256)
                    if not chunk:
                        raise ConnectionError("closed by the gateway")
                    data += chunk
                ms = int((time.time() - t0) * 1000)
                if data[7] == 3:
                    return True, ms, "ok"
                return False, ms, f"exception fc=0x{data[7]:02X} code=0x{data[8]:02X}"
            except (OSError, ConnectionError) as exc:
                self.close()
                if attempt == 2:
                    return False, int((time.time() - t0) * 1000), f"{type(exc).__name__}: {exc}"
        return False, 0, "?"


def http_from_lan(host: str) -> str:
    """A GET on port 502 from the LAN must get nothing back (the page is for AP clients only)."""
    try:
        with socket.create_connection((host, 502), timeout=4) as s:
            s.settimeout(2.5)
            s.sendall(b"GET / HTTP/1.0\r\n\r\n")
            try:
                data = s.recv(200)
                return "ANSWERED: " + data[:60].decode("latin-1", "replace") if data else "closed with no data"
            except socket.timeout:
                return "no answer (dropped)"
    except OSError as exc:
        return f"{type(exc).__name__}: {exc}"


def arm(link, who: str = "ap-test") -> None:
    h = link.hello(who)
    if not h.ok:
        raise SystemExit(f"HELLO refused: {h.error_text}")


def wait_state(link, want: str, limit_s: float = 20.0) -> tuple:
    t0 = time.time()
    while time.time() - t0 < limit_s:
        line = link.command("AP").lines[0]
        if f"ap.state={want}" in line or "ap.state=failed" in line:
            return True, time.time() - t0, line
        time.sleep(0.5)
    return False, time.time() - t0, link.command("AP").lines[0]


def mode_basic(host: str) -> int:
    bad = 0
    with _Session(host, 502) as link:
        d = info(link)
        log("before:", {k: d.get(k) for k in ("fw", "sys.up", "sys.reset", "hw.wifi", "wifi.fw", "ap.state", "ntp.state", "heap.used")})
        arm(link)
        log("AP PASS:", link.command(f"AP PASS {PASS}").lines)
        log("AP ON:", link.command("AP ON min=20").lines)
        ok, s, line = wait_state(link, "on")
        log(f"state after {s:.1f} s:", line)
        bad += 0 if ok and "ap.state=on" in line else 1
        log("AP:", ap_line(link))
        d = info(link)
        log("during:", {k: d.get(k) for k in ("ap.state", "ap.start_ms", "ntp.state", "heap.used", "net.client")})
        bad += 0 if d.get("ntp.state") == "paused" else 1
    # LAN Modbus while the AP is up (the site server's path), on one persistent connection
    mb = Modbus(host)
    reads = [mb.read(uid) for uid in (11, 12, 13, 14, 21, 22, 23, 24, 31, 32, 33, 34, 41, 42, 43, 44)]
    mb.close()
    log("LAN Modbus during AP:", sum(r[0] for r in reads), "/", len(reads), "ok, ms", [r[1] for r in reads],
        [r[2] for r in reads if not r[0]][:3])
    bad += sum(0 if r[0] else 1 for r in reads)
    log("GET / from the LAN:", http_from_lan(host))
    with _Session(host, 502) as link:
        log("SWEEP START:", link.command("SWEEP START").lines)
        t0 = time.time()
        while time.time() - t0 < 120:
            time.sleep(2)
            head = link.command("SWEEP STATUS").lines[0]
            if "state=done" in head:
                break
        log("SWEEP:", head, f"({time.time() - t0:.0f} s)")
        bad += 0 if "silent=0" in head else 1
        arm(link)
        log("AP OFF:", link.command("AP OFF").lines)
        ok, s, line = wait_state(link, "off")
        log(f"state after {s:.1f} s:", line)
        time.sleep(2)
        d = info(link)
        log("after:", {k: d.get(k) for k in ("ap.state", "ntp.state", "heap.used", "sys.up", "sys.reset")})
        bad += 0 if d.get("ntp.state") != "paused" else 1
    mb = Modbus(host)
    reads = [mb.read(uid) for uid in (11, 12, 13, 14, 21, 22, 23, 24, 31, 32, 33, 34, 41, 42, 43, 44)]
    mb.close()
    log("LAN Modbus with the AP off:", sum(r[0] for r in reads), "/", len(reads), "ok, ms", [r[1] for r in reads])
    log("RESULT basic:", "PASS" if bad == 0 else f"{bad} problem(s)")
    return bad


def mode_cycles(host: str, n: int) -> int:
    bad = 0
    with _Session(host, 502) as link:
        up0 = int(info(link).get("sys.up", 0))
        for i in range(1, n + 1):
            arm(link)
            link.command("AP ON min=5")
            ok_on, t_on, line_on = wait_state(link, "on")
            arm(link)
            link.command("AP OFF")
            ok_off, t_off, line_off = wait_state(link, "off")
            d = info(link)
            good = ok_on and "state=on" in line_on and ok_off and "state=off" in line_off
            bad += 0 if good else 1
            log(f"cycle {i}/{n}: on {t_on:.1f}s off {t_off:.1f}s start_ms={d.get('ap.start_ms')} heap={d.get('heap.used')} "
                f"starts={d.get('ap.starts')} fails={d.get('ap.fails')} {'ok' if good else 'BAD ' + line_on}")
        up1 = int(info(link).get("sys.up", 0))
        if up1 < up0:
            log("GATEWAY REBOOTED during the cycles"); bad += 1
    log("RESULT cycles:", "PASS" if bad == 0 else f"{bad} problem(s)")
    return bad


def mode_soak(host: str, minutes: int) -> int:
    with _Session(host, 502) as link:
        arm(link)
        log("AP ON:", link.command(f"AP ON min={min(minutes + 10, 480)}").lines)
        log(wait_state(link, "on"))
        up0 = int(info(link).get("sys.up", 0))
    t_end = time.time() + minutes * 60
    reads = fails = 0
    worst = 0
    last_report = 0.0
    up_prev = up0
    ids = [11, 12, 13, 14, 21, 22, 23, 24, 31, 32, 33, 34, 41, 42, 43, 44]
    mb = Modbus(host)
    while time.time() < t_end:
        for uid in ids:
            ok, ms, det = mb.read(uid)
            reads += 1
            worst = max(worst, ms)
            if not ok:
                fails += 1
                log(f"read fail unit {uid}: {det} ({ms} ms)")
            time.sleep(0.25)
        if time.time() - last_report > 600:
            last_report = time.time()
            try:
                with _Session(host, 502) as link:
                    d = info(link)
                up = int(d.get("sys.up", 0))
                if up < up_prev:
                    log("GATEWAY REBOOTED: sys.reset=", d.get("sys.reset"))
                up_prev = up
                log(f"soak: reads={reads} fails={fails} worst={worst} ms ap={d.get('ap.state')} left={d.get('ap.left_s')} "
                    f"heap={d.get('heap.used')} up={up} reset={d.get('sys.reset')}")
            except Exception as exc:  # noqa: BLE001
                log("status read failed:", exc)
    mb.close()
    with _Session(host, 502) as link:
        d = info(link)
        arm(link)
        link.command("AP OFF")
    log(f"RESULT soak: reads={reads} fails={fails} worst={worst} ms reboots={'yes' if int(d.get('sys.up', 0)) < up0 else 'no'}")
    return fails


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="192.168.99.111")
    ap.add_argument("--mode", choices=("basic", "cycles", "soak"), default="basic")
    ap.add_argument("--n", type=int, default=50)
    ap.add_argument("--minutes", type=int, default=480)
    a = ap.parse_args()
    if a.mode == "basic":
        return mode_basic(a.host)
    if a.mode == "cycles":
        return mode_cycles(a.host, a.n)
    return mode_soak(a.host, a.minutes)


if __name__ == "__main__":
    sys.exit(1 if main() else 0)
