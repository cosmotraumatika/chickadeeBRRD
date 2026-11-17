#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Jamie Saker
"""A pretend chickadeeBRRD stick on a pseudo-terminal.

Speaks protocol v0 so the CLI (and later the Android app) can be developed
without hardware. Prints the port to use, then answers requests until Ctrl-C.

    python3 tools/fakestick.py
    chickchirp --port /dev/pts/N info
"""
import json
import os
import select
import sys
import time
import tty

FW = "0.1.0-fake"
PROTO = 0
MAX_LINE = 1024
MAX_TEXT = 64
LEASE_MS = 3000
SESSION_MAX_MS = int(os.environ.get("FAKESTICK_SESSION_MAX_MS", "300000"))
SIGNALS = ["led"]
started = time.monotonic()

# The transmit session, as the firmware's state machine keeps it.
session = {"active": False, "signal": None, "began": 0.0, "lease_until": 0.0, "last_stop": None}


def now_ms():
    return time.monotonic() * 1000


def end_session(fd, reason):
    session["active"] = False
    session["last_stop"] = reason
    print(f"[led] off ({reason})", flush=True)
    reply(fd, {"ev": "state", "state": "idle", "reason": reason})


def tick(fd):
    """Stop the session when its limit or its lease runs out."""
    if not session["active"]:
        return
    if now_ms() - session["began"] >= SESSION_MAX_MS:
        end_session(fd, "limit")
    elif now_ms() >= session["lease_until"]:
        end_session(fd, "lease_expired")


def lease_fields():
    remaining = max(0, SESSION_MAX_MS - (now_ms() - session["began"]))
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
                          "limits": {"lease_ms": LEASE_MS, "session_max_ms": SESSION_MAX_MS, "text_max": MAX_TEXT}})
    if cmd == "status":
        up = int((time.monotonic() - started) * 1000)
        out = {"id": rid, "ok": True, "state": "idle", "uptime_ms": up}
        if session["active"]:
            out.update(state="transmitting", signal=session["signal"],
                       session_remaining_ms=lease_fields()["session_remaining_ms"],
                       lease_remaining_ms=int(max(0, session["lease_until"] - now_ms())))
        if session["last_stop"]:
            out["last_stop"] = session["last_stop"]
        return reply(fd, out)
    if cmd == "tx.start":
        signal = req.get("signal")
        if not isinstance(signal, str):
            return reply(fd, {"id": rid, "ok": False, "err": "bad_args", "msg": "signal must be a string"})
        if signal not in SIGNALS:
            return reply(fd, {"id": rid, "ok": False, "err": "unsupported", "msg": f"no such signal: {signal}"})
        if session["active"]:
            return reply(fd, {"id": rid, "ok": False, "err": "busy", "msg": "a session is already running"})
        session.update(active=True, signal=signal, began=now_ms(), lease_until=now_ms() + LEASE_MS)
        print(f"[led] blinking ({signal})", flush=True)
        reply(fd, {"id": rid, "ok": True, **lease_fields()})
        return reply(fd, {"ev": "state", "state": "transmitting", "signal": signal})
    if cmd == "lease.renew":
        if not session["active"]:
            return reply(fd, {"id": rid, "ok": False, "err": "not_active"})
        session["lease_until"] = now_ms() + LEASE_MS
        return reply(fd, {"id": rid, "ok": True, **lease_fields()})
    if cmd == "tx.stop":
        was_active = session["active"]
        reply(fd, {"id": rid, "ok": True, "state": "idle", "stopped": was_active})
        if was_active:
            end_session(fd, "stopped")
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
