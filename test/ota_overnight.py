#!/usr/bin/env python3
"""Overnight soak of the gateway-run module OTA: start a run every --interval seconds until --until, record each one.

Meant to run next to a Test Tool pharmacy-mode soak on the same gateway (realistic picks, so the runner has to find its gaps).
It holds a TCP slot only while a round is active, writes one JSON line per round plus a human log, and survives the gateway
rebooting (the 03:00 scheduled reset is deliberately allowed to hit one round: --collide HH:MM:SS starts an 8-module round
shortly before it).

Usage (the Test Tool's venv has pymodbus):
  python -u test/ota_overnight.py --host 192.168.99.111 --ids 11-14 --until 06:30 --max-rounds 16 --collide 02:58:00 --out DIR
"""
import argparse
import ctypes
import datetime as dt
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ota_runner_test as t  # noqa: E402

ES_CONTINUOUS, ES_SYSTEM_REQUIRED = 0x80000000, 0x00000001


def keep_awake():
    try:
        ctypes.windll.kernel32.SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED)
    except Exception:  # noqa: BLE001
        pass


def today_at(hhmmss, base=None):
    base = base or dt.datetime.now()
    h, m, *s = [int(x) for x in hhmmss.split(":")]
    return base.replace(hour=h, minute=m, second=(s[0] if s else 0), microsecond=0)


def kv_of(text):
    out = {}
    for part in text.replace("#DATA", " ").split():
        if "=" in part:
            k, v = part.split("=", 1)
            out[k] = v
    return out


def info_snapshot(c):
    kv = kv_of(t.console(c, "INFO"))
    keys = ("sys.up", "sys.reset", "sys.boots", "cnt.tcp_ok", "cnt.rs485_ok", "cnt.rs485_timeout", "rtu.retry_ok", "rtu.retry_fail",
            "rtu.bad_crc", "rtu.bad_other", "hub.cross", "log.n", "log.state", "ota.state", "time.now")
    return {k: kv.get(k) for k in keys}


def connect_retry(host, max_s):
    t0 = time.monotonic()
    while True:
        try:
            c = t.ModbusTcpClient(host=host, port=502, timeout=6.0, retries=0)
            c.register(t.GwRawPDU)
            if c.connect():
                t.console(c, "PING")
                return c
        except Exception:  # noqa: BLE001
            pass
        if time.monotonic() - t0 > max_s:
            return None
        time.sleep(5)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", required=True)
    ap.add_argument("--ids", default="11-14")
    ap.add_argument("--collide-ids", default="11-18")
    ap.add_argument("--interval", type=float, default=1800.0)
    ap.add_argument("--until", default="06:30", help="no new round starts after this time (next morning when earlier than now)")
    ap.add_argument("--max-rounds", type=int, default=16)
    ap.add_argument("--collide", default="", help="HH:MM:SS: start one extra round with --collide-ids at this time")
    ap.add_argument("--extra", default="", help="extra OTA START arguments")
    ap.add_argument("--round-max-s", type=float, default=1800.0, help="give a round this long before it is aborted")
    ap.add_argument("--out", required=True)
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    keep_awake()

    now = dt.datetime.now()
    until = today_at(args.until, now)
    if until <= now:
        until += dt.timedelta(days=1)
    collide_at = None
    if args.collide:
        collide_at = today_at(args.collide, now)
        if collide_at <= now:
            collide_at += dt.timedelta(days=1)
    logf = open(os.path.join(args.out, "overnight.log"), "a", buffering=1, encoding="utf-8")
    jf = open(os.path.join(args.out, "rounds.jsonl"), "a", buffering=1, encoding="utf-8")

    def log(msg):
        line = f"{dt.datetime.now():%Y-%m-%d %H:%M:%S} {msg}"
        print(line, flush=True)
        logf.write(line + "\n")

    log(f"overnight start: host={args.host} ids={args.ids} interval={args.interval:.0f}s until={until:%m-%d %H:%M} "
        f"max_rounds={args.max_rounds} collide={collide_at}")
    n, bad_streak = 0, 0
    next_at = dt.datetime.now()
    collide_done = collide_at is None
    while n < args.max_rounds + (0 if collide_done else 1):
        keep_awake()
        now = dt.datetime.now()
        is_collide = False
        if not collide_done and collide_at <= now + dt.timedelta(seconds=args.interval) and collide_at >= now - dt.timedelta(seconds=1):
            # the collision round is its own thing: wait for its moment, whatever the regular cadence says
            wait = (collide_at - now).total_seconds()
            if wait > 0:
                log(f"waiting {wait:.0f} s for the collision round")
                time.sleep(wait)
            is_collide, collide_done = True, True
        elif now < next_at:
            nxt = next_at
            if collide_at and not collide_done and collide_at < nxt:
                nxt = collide_at - dt.timedelta(seconds=1)
            time.sleep(max(1.0, min((nxt - now).total_seconds(), 60.0)))
            continue
        if dt.datetime.now() >= until:
            log("past the cut-off time: no more rounds")
            break

        n += 1
        ids = args.collide_ids if is_collide else args.ids
        rec = {"round": n, "kind": "collide" if is_collide else "regular", "ids": ids, "t_start": f"{dt.datetime.now():%H:%M:%S}"}
        log(f"--- round {n} ({rec['kind']}) ids={ids}")
        c = connect_retry(args.host, 600)
        if c is None:
            rec.update(state="no_connection")
            log("gateway unreachable for 10 min")
            jf.write(json.dumps(rec) + "\n")
            bad_streak += 1
            next_at = dt.datetime.now() + dt.timedelta(seconds=args.interval)
            if bad_streak >= 3:
                break
            continue
        try:
            rec["before"] = info_snapshot(c)
            st = kv_of(t.console(c, "OTA STATUS"))
            if st.get("state") == "running":
                log("a run is still active: waiting for it")
                t.wait_done(c, "  ", every=15.0, max_s=args.round_max_s)
            t0 = time.monotonic()
            t.console(c, "HELLO overnight")
            out = t.console(c, "OTA START slot=0" + f" ids={ids}" + (" " + args.extra if args.extra else ""))
            rec["start_reply"] = out.strip().replace("\n", " | ")
            if "#OK OTA START" not in out:
                rec["state"] = "start_refused"
                log("START refused: " + rec["start_reply"])
            else:
                final, last_ok = {"state": "running"}, time.monotonic()
                rebooted = False
                while time.monotonic() - t0 < args.round_max_s:
                    try:
                        kv = kv_of(t.console(c, "OTA STATUS"))
                        last_ok = time.monotonic()
                    except Exception:  # noqa: BLE001
                        # the gateway went away (the 03:00 reset, or a fault): reconnect and look at what it remembers
                        rebooted = True
                        try:
                            c.close()
                        except Exception:  # noqa: BLE001
                            pass
                        log("lost the gateway mid-round; reconnecting")
                        c = connect_retry(args.host, 600)
                        if c is None:
                            kv = {"state": "gateway_lost"}
                            break
                        kv = kv_of(t.console(c, "OTA STATUS"))
                    if kv.get("state") != "running":
                        final = kv
                        break
                    time.sleep(10)
                else:
                    log("round time limit: aborting")
                    t.console(c, "HELLO overnight")
                    t.console(c, "OTA ABORT")
                    final = {"state": "timeout_aborted"}
                rec.update(state=final.get("state"), seconds=round(time.monotonic() - t0), rebooted=rebooted)
                if c is not None:
                    rec["result"] = t.console(c, "OTA RESULT 0").strip().replace("\n", " | ")
                    rec["after"] = info_snapshot(c)
                    log(f"round {n}: {rec['state']} in {rec['seconds']} s" + (" (gateway rebooted)" if rebooted else ""))
        except Exception as exc:  # noqa: BLE001
            rec["state"] = "script_error"
            rec["error"] = repr(exc)
            log("script error: " + repr(exc))
        finally:
            try:
                if c is not None:
                    c.close()
            except Exception:  # noqa: BLE001
                pass
        jf.write(json.dumps(rec) + "\n")
        ok = rec.get("state") == "done_ok" or (is_collide and rec.get("state") in ("interrupted", "gateway_lost"))
        bad_streak = 0 if ok else bad_streak + 1
        if bad_streak >= 3:
            log("three bad rounds in a row: stopping so the cabinet is left alone")
            break
        if not is_collide:
            next_at = max(next_at + dt.timedelta(seconds=args.interval), dt.datetime.now() + dt.timedelta(seconds=60))
    log(f"overnight finished after {n} rounds")


if __name__ == "__main__":
    main()
