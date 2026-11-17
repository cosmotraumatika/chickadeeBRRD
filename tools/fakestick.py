#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Jamie Saker
"""A pretend chickadeeBRRD stick on a pseudo-terminal.

Speaks protocol v0 so the CLI (and later the Android app) can be developed
without hardware. Prints the port to use, then answers requests until Ctrl-C.

    python3 tools/fakestick.py
    chickchirp --port /dev/pts/N info

It offers the same signals as the real firmware, "led" and "bt4", and applies
the same checks to tx.start. Nothing is transmitted: it prints what a real
stick would be doing. To try the limits without waiting, shorten the default
session with FAKESTICK_SESSION_DEFAULT_MS=4000.
"""
import json
import os
import select
import sys
import time
import tty

FW = "0.4.0-fake"
PROTO = 0
MAX_LINE = 1024
MAX_TEXT = 64
LEASE_MS = 3000
# FAKESTICK_SESSION_MAX_MS is the older name for the default limit.
SESSION_DEFAULT_MS = int(os.environ.get("FAKESTICK_SESSION_DEFAULT_MS",
                                        os.environ.get("FAKESTICK_SESSION_MAX_MS", "300000")))
SESSION_HARD_MAX_MS = 3600000
POWER_MIN_DBM, POWER_DEFAULT_DBM, POWER_MAX_DBM, POWER_STEP_DB = -24, -24, 0, 3
UNIX_2019 = 1546300800
RID = {"id_type": "specific_session_id", "id": "CHICKADEEBRRD-TEST", "self_id": "chickadeeBRRD:TEST"}
SIGNALS = ["led", "bt4"]
started = time.monotonic()

# The transmit session, as the firmware's state machine keeps it.
session = {"active": False, "signal": None, "began": 0.0, "lease_until": 0.0, "last_stop": None,
           "max_ms": SESSION_DEFAULT_MS, "power_dbm": None, "lat": None, "lon": None}


def is_number(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool)


def now_ms():
    return time.monotonic() * 1000


def end_session(fd, reason):
    session["active"] = False
    session["last_stop"] = reason
    print(f"[{session['signal']}] off ({reason})", flush=True)
    reply(fd, {"ev": "state", "state": "idle", "reason": reason})


def tick(fd):
    """Stop the session when its limit or its lease runs out."""
    if not session["active"]:
        return
    if now_ms() - session["began"] >= session["max_ms"]:
        end_session(fd, "limit")
    elif now_ms() >= session["lease_until"]:
        end_session(fd, "lease_expired")


def lease_fields():
    remaining = max(0, session["max_ms"] - (now_ms() - session["began"]))
    return {"lease_ms": LEASE_MS, "session_remaining_ms": int(remaining)}


def reply(fd, obj):
    os.write(fd, (json.dumps(obj, separators=(",", ":")) + "\n").encode())


def handle(fd, raw):
    try:
        req = json.loads(raw)
        if not isinstance(req, dict):
            raise ValueError
    except ValueError:
        return reply(fd, {"id": 0, "ok": False, "err": "bad_json"})
    rid = req.get("id", 0)
    cmd = req.get("cmd")
    if not isinstance(rid, int) or not isinstance(cmd, str):
        return reply(fd, {"id": 0, "ok": False, "err": "bad_args", "msg": "need numeric id and string cmd"})
    if cmd == "hello":
        return reply(fd, {"id": rid, "ok": True, "device": "chickadeeBRRD", "board": "fakestick",
                          "fw": FW, "proto": PROTO, "caps": ["display.text", "tx"], "signals": SIGNALS,
                          "limits": {"lease_ms": LEASE_MS, "session_default_ms": SESSION_DEFAULT_MS,
                                     "session_max_ms": SESSION_HARD_MAX_MS, "power_min_dbm": POWER_MIN_DBM,
                                     "power_default_dbm": POWER_DEFAULT_DBM, "power_max_dbm": POWER_MAX_DBM,
                                     "power_step_db": POWER_STEP_DB, "text_max": MAX_TEXT},
                          "rid": RID})
    if cmd == "status":
        up = int((time.monotonic() - started) * 1000)
        out = {"id": rid, "ok": True, "state": "idle", "uptime_ms": up}
        if session["active"]:
            out.update(state="transmitting", signal=session["signal"],
                       session_remaining_ms=lease_fields()["session_remaining_ms"],
                       lease_remaining_ms=int(max(0, session["lease_until"] - now_ms())))
            if session["signal"] == "bt4":
                out.update(power_dbm=session["power_dbm"], lat=session["lat"], lon=session["lon"])
        if session["last_stop"]:
            out["last_stop"] = session["last_stop"]
        return reply(fd, out)
    if cmd == "tx.start":
        signal = req.get("signal")
        if not isinstance(signal, str):
            return reply(fd, {"id": rid, "ok": False, "err": "bad_args", "msg": "signal must be a string"})
        if signal not in SIGNALS:
            return reply(fd, {"id": rid, "ok": False, "err": "unsupported", "msg": f"no such signal: {signal}"})

        def refuse(err, msg):
            return reply(fd, {"id": rid, "ok": False, "err": err, "msg": msg})

        max_ms = SESSION_DEFAULT_MS
        if "max_ms" in req:
            if not is_number(req["max_ms"]) or req["max_ms"] < 1:
                return refuse("bad_args", "max_ms must be a positive number")
            if req["max_ms"] > SESSION_HARD_MAX_MS:
                return refuse("limit_exceeded", "a session is limited to 60 minutes")
            max_ms = int(req["max_ms"])
        power, lat, lon = None, None, None
        if signal == "bt4":
            lat, lon = req.get("lat"), req.get("lon")
            if not is_number(lat) or not is_number(lon):
                return refuse("bad_args", "bt4 needs lat and lon in degrees")
            if not (-90 <= lat <= 90 and -180 <= lon <= 180) or (lat == 0 and lon == 0):
                return refuse("bad_args", "lat and lon must be a real position")
            if not is_number(req.get("time")) or req["time"] < UNIX_2019:
                return refuse("bad_args", "bt4 needs time, the host clock in Unix seconds")
            power = POWER_DEFAULT_DBM
            if "power_dbm" in req:
                power = req["power_dbm"]
                if not is_number(power) or power != int(power):
                    return refuse("bad_args", "power_dbm must be a whole number")
                if power > POWER_MAX_DBM:
                    return refuse("limit_exceeded", "power is limited to 0 dBm")
                if power < POWER_MIN_DBM or (power - POWER_MIN_DBM) % POWER_STEP_DB != 0:
                    return refuse("bad_args", "power_dbm must be -24 to 0 in steps of 3")
                power = int(power)
        if session["active"]:
            return refuse("busy", "a session is already running")
        session.update(active=True, signal=signal, began=now_ms(), lease_until=now_ms() + LEASE_MS,
                       max_ms=max_ms, power_dbm=power, lat=lat, lon=lon)
        out = {"id": rid, "ok": True, **lease_fields()}
        event = {"ev": "state", "state": "transmitting", "signal": signal}
        if signal == "bt4":
            print(f"[bt4] would broadcast {RID['id']} at {lat}, {lon}, {power} dBm, "
                  f"for up to {max_ms} ms (nothing is transmitted)", flush=True)
            out["power_dbm"] = power
            event["power_dbm"] = power
        else:
            print(f"[led] blinking, for up to {max_ms} ms", flush=True)
        reply(fd, out)
        return reply(fd, event)
    if cmd == "lease.renew":
        if not session["active"]:
            return reply(fd, {"id": rid, "ok": False, "err": "not_active"})
        session["lease_until"] = now_ms() + LEASE_MS
        return reply(fd, {"id": rid, "ok": True, **lease_fields()})
    if cmd == "tx.stop":
        was_active = session["active"]
        if was_active:
            session["active"] = False
            session["last_stop"] = "stopped"
            print(f"[{session['signal']}] off (stopped)", flush=True)
        reply(fd, {"id": rid, "ok": True, "state": "idle", "stopped": was_active})
        if was_active:
            reply(fd, {"ev": "state", "state": "idle", "reason": "stopped"})
        return None
    if cmd == "display.text":
        text = req.get("text")
        if not isinstance(text, str):
            return reply(fd, {"id": rid, "ok": False, "err": "bad_args", "msg": "text must be a string"})
        if session["active"]:
            return reply(fd, {"id": rid, "ok": False, "err": "busy", "msg": "a session is running"})
        if len(text.encode()) > MAX_TEXT:
            return reply(fd, {"id": rid, "ok": False, "err": "too_long", "msg": f"text is limited to {MAX_TEXT} bytes"})
        print(f"[screen] {text}", flush=True)
        return reply(fd, {"id": rid, "ok": True})
    return reply(fd, {"id": rid, "ok": False, "err": "unknown_cmd", "msg": f"no such command: {cmd}"})


def main():
    master, slave = os.openpty()
    tty.setraw(slave)
    print(os.ttyname(slave), flush=True)
    # What a real stick does at power-on: boot chatter, then the boot event.
    os.write(master, b"ESP-ROM:esp32c5-eco2-20250121\r\nI (312) main_task: Calling app_main()\r\n")
    reply(master, {"ev": "boot", "fw": FW, "proto": PROTO})
    buf = b""
    try:
        while True:
            ready, _, _ = select.select([master], [], [], 0.1)
            tick(master)
            if not ready:
                continue
            try:
                buf += os.read(master, 256)
            except OSError:
                continue  # the host closed the port; keep ticking
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                if len(line) > MAX_LINE:
                    reply(master, {"id": 0, "ok": False, "err": "too_long"})
                elif line.strip():
                    handle(master, line.decode("utf-8", "replace"))
    except KeyboardInterrupt:
        sys.exit(0)


if __name__ == "__main__":
    main()
