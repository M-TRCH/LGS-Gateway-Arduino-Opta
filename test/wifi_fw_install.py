"""Install the Opta WiFi radio firmware file over the network (unit 255 / FC 0x41 subs 0x30-0x32).

Writes /wlan/4343WA1.BIN on QSPI partition 1 (as 4343WA1.TMP, CRC-checked on read-back, then renamed), so a gateway whose
service AP says `wifi_fw_not_installed` can be fixed without a site visit or USB. The image is the one linked into the
Arduino core: this script pulls it out of libmbed.a with the toolchain's objcopy (or take --file).

    python test/wifi_fw_install.py --host 192.168.99.111            # extract + install + check
    python test/wifi_fw_install.py --host 192.168.99.111 --file 4343WA1.BIN
    python test/wifi_fw_install.py --extract out.bin                # just extract, no network

Needs the Test Tool's venv (gateway_api._Session for the unit 255 link):
    ..\\LGS-Test-Tool\\.venv\\Scripts\\python.exe test\\wifi_fw_install.py ...
The AP must be off (the gateway refuses while it is up); this script turns it off first. Exit 0 = installed and reads ok.
"""
from __future__ import annotations

import argparse
import glob
import os
import struct
import subprocess
import sys
import tempfile
import time
import zlib

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "LGS-Test-Tool"))

SUB_BEGIN, SUB_DATA, SUB_STATUS = 0x30, 0x31, 0x32
CHUNK = 240
EXPECT_SIZE = 421098
STATUS = {0: "ok", 1: "locked", 2: "busy", 3: "storage", 4: "bad_param", 5: "seq_mismatch", 6: "crc"}


def pio_pkg(*parts: str) -> str:
    return os.path.join(os.path.expanduser("~"), ".platformio", "packages", *parts)


def extract(dest: str) -> bytes:
    """4343WA1_bin.o out of libmbed.a, its image section out of that."""
    libs = glob.glob(pio_pkg("framework-arduino-mbed", "variants", "OPTA", "libs", "libmbed.a"))
    if not libs:
        raise SystemExit("libmbed.a for the OPTA variant not found (build the project once)")
    bindir = os.path.dirname(pio_pkg("toolchain-gccarmnoneeabi", "bin", "x"))
    ar = os.path.join(bindir, "arm-none-eabi-ar.exe" if os.name == "nt" else "arm-none-eabi-ar")
    oc = os.path.join(bindir, "arm-none-eabi-objcopy.exe" if os.name == "nt" else "arm-none-eabi-objcopy")
    with tempfile.TemporaryDirectory() as td:
        subprocess.run([ar, "x", libs[0], "4343WA1_bin.o"], cwd=td, check=True)
        subprocess.run([oc, "-O", "binary", "-j", ".rodata.wifi_firmware_image_data", "4343WA1_bin.o", dest], cwd=td, check=True)
    return open(dest, "rb").read()


def log(*a) -> None:
    print(time.strftime("%H:%M:%S"), *a, flush=True)


def info(link) -> dict:
    d: dict = {}
    for row in link.command("INFO").rows:
        d.update(row)
    return d


def status(link) -> tuple:
    p = link._execute(SUB_STATUS).payload
    receiving = p[0]
    got = struct.unpack(">I", p[1:5])[0]
    state = p[6:6 + p[5]].decode("ascii", "replace")
    return receiving, got, state


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host")
    ap.add_argument("--file")
    ap.add_argument("--extract", help="only write the image here and exit")
    a = ap.parse_args()

    if a.file:
        data = open(a.file, "rb").read()
    else:
        out = a.extract or os.path.join(tempfile.gettempdir(), "4343WA1.BIN")
        extract(os.path.abspath(out))
        data = open(out, "rb").read()
    crc = zlib.crc32(data) & 0xFFFFFFFF
    log(f"image {len(data)} B crc32=0x{crc:08x}")
    if a.extract:
        return 0
    if len(data) != EXPECT_SIZE:
        log(f"FAIL: expected {EXPECT_SIZE} B")
        return 1
    if not a.host:
        ap.error("--host is required")

    from app.v2.gateway_api import _Session
    with _Session(a.host, 502, timeout_s=8.0) as link:
        before = info(link)
        log("before: wifi.fw =", before.get("wifi.fw"), "hw.wifi =", before.get("hw.wifi"), "fw", before.get("fw"))
        if before.get("hw.wifi") != "1":
            log("FAIL: this unit has no Wi-Fi radio")
            return 1
        link.command("HELLO wifi-fw")
        link.command("AP OFF")
        time.sleep(2)
        r = link._execute(SUB_BEGIN, struct.pack(">II", len(data), crc)).payload[0]
        log("BEGIN ->", STATUS.get(r, r))
        if r != 0:
            return 1
        off, t0, last = 0, time.time(), 0
        while off < len(data):
            chunk = data[off:off + CHUNK]
            p = link._execute(SUB_DATA, struct.pack(">I", off) + chunk).payload
            st, got = p[0], struct.unpack(">I", p[1:5])[0]
            if st == 5:                       # the gateway says where it wants the stream
                off = got
                continue
            if st != 0:
                log(f"FAIL at offset {off}: {STATUS.get(st, st)}")
                return 1
            off = got
            if off - last >= 50_000:
                last = off
                log(f"  {off}/{len(data)}  ({off * 100 // len(data)}%)")
        log(f"uploaded in {time.time() - t0:.0f} s")
        for _ in range(30):
            rec, got, state = status(link)
            log(f"status: receiving={rec} received={got} state={state}")
            if not rec:
                break
            time.sleep(1)
        after = info(link)
        log("after: wifi.fw =", after.get("wifi.fw"))
        if after.get("wifi.fw") != "ok":
            return 1
        log("PASS: radio file installed")
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
