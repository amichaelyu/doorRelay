#!/usr/bin/env python3
"""
Monitor the macOS Messages database (chat.db) and open the door when an
authorized sender texts the trigger word.

When a new incoming iMessage/SMS arrives from a number listed in config.yaml
and its text contains the trigger word (default "open", any case), this sends
a message to the ESP32 relay's WebSocket server, which pulses the relay for 5s
(see relay.ino).

Requires Full Disk Access for whatever runs it (Terminal / python), otherwise
chat.db cannot be read.

Usage:
    ./.venv/bin/python door_monitor.py [path/to/config.yaml]
"""

import os
import re
import sys
import time
import sqlite3
import logging

import yaml
import websocket  # from the `websocket-client` package

CHAT_DB = os.path.expanduser("~/Library/Messages/chat.db")

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s  %(levelname)s  %(message)s",
    datefmt="%Y-%m-%d %H:%M:%S",
)
log = logging.getLogger("door")


def load_config(path):
    with open(path, "r") as f:
        cfg = yaml.safe_load(f) or {}

    senders = cfg.get("authorized_senders") or []
    if not senders:
        log.warning("No authorized_senders configured — nothing will ever trigger.")

    relay = cfg.get("relay") or {}
    return {
        "authorized": {normalize_sender(str(s)) for s in senders},
        "trigger_re": re.compile(
            r"\b" + re.escape(str(cfg.get("trigger_word", "open"))) + r"\b",
            re.IGNORECASE,
        ),
        "host": relay.get("host", "192.168.0.67"),
        "port": int(relay.get("port", 81)),
        "code": str(relay.get("code", "")),
        "poll_interval": float(cfg.get("poll_interval", 2)),
    }


def normalize_sender(raw):
    """Emails match exactly (lowercased); phone numbers match by last 10 digits."""
    raw = raw.strip().lower()
    if "@" in raw:
        return raw
    digits = re.sub(r"\D", "", raw)
    return digits[-10:] if len(digits) >= 10 else digits


def extract_text(text, attributed_body):
    """message.text is usually populated; fall back to attributedBody if not."""
    if text:
        return text
    if not attributed_body:
        return ""
    # attributedBody is a serialized NSAttributedString (typedstream). The plain
    # text sits after an "NSString" marker; pull a best-effort copy of it.
    try:
        blob = attributed_body
        idx = blob.find(b"NSString")
        if idx == -1:
            return ""
        # skip past "NSString" + class-version bytes to the length-prefixed data
        chunk = blob[idx + 8:]
        # a small preamble precedes the UTF-8 bytes; strip leading control bytes
        chunk = chunk.lstrip(b"\x01\x02\x03\x04\x05\x06\x07\x08\x09\x2b\x81\x84\x86")
        # length may be 1 byte, or 0x81/0x82-prefixed 2/3 byte little-endian
        return chunk.split(b"\x86", 1)[0].decode("utf-8", "ignore")
    except Exception:
        return ""


def open_db():
    # read-only URI connection so we never interfere with Messages
    return sqlite3.connect(f"file:{CHAT_DB}?mode=ro", uri=True, timeout=5)


def high_watermark(conn):
    row = conn.execute("SELECT MAX(ROWID) FROM message").fetchone()
    return row[0] or 0


NEW_MESSAGES_SQL = """
    SELECT m.ROWID, h.id, m.text, m.attributedBody
    FROM message AS m
    LEFT JOIN handle AS h ON m.handle_id = h.ROWID
    WHERE m.ROWID > ? AND m.is_from_me = 0
    ORDER BY m.ROWID ASC
"""


def trigger_door(host, port, code):
    url = f"ws://{host}:{port}/"
    log.info("Opening door via %s", url)
    try:
        ws = websocket.create_connection(url, timeout=5)
        try:
            ws.send(code)
            try:
                log.info("Relay replied: %s", ws.recv())
            except Exception:
                pass  # relay ack is optional
        finally:
            ws.close()
        return True
    except Exception as e:
        log.error("Failed to reach relay at %s: %s", url, e)
        return False


def main():
    cfg_path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "config.yaml"
    )
    cfg = load_config(cfg_path)
    log.info(
        "Watching chat.db | %d authorized sender(s) | relay ws://%s:%d",
        len(cfg["authorized"]), cfg["host"], cfg["port"],
    )

    try:
        conn = open_db()
    except sqlite3.OperationalError as e:
        log.error("Cannot open chat.db (%s). Grant Full Disk Access to your "
                  "terminal/python in System Settings > Privacy & Security.", e)
        sys.exit(1)

    last_rowid = high_watermark(conn)
    log.info("Starting from message ROWID %d — only new messages will count.", last_rowid)

    while True:
        try:
            rows = conn.execute(NEW_MESSAGES_SQL, (last_rowid,)).fetchall()
        except sqlite3.OperationalError as e:
            # DB may be transiently locked by Messages; retry next tick
            log.debug("DB read hiccup: %s", e)
            time.sleep(cfg["poll_interval"])
            continue

        for rowid, sender, text, attributed_body in rows:
            last_rowid = max(last_rowid, rowid)
            body = extract_text(text, attributed_body)
            if sender is None:
                continue
            if normalize_sender(sender) not in cfg["authorized"]:
                continue
            if not cfg["trigger_re"].search(body):
                log.info("Authorized msg from %s (no trigger word): %r", sender, body)
                continue
            log.info("TRIGGER from %s: %r", sender, body)
            trigger_door(cfg["host"], cfg["port"], cfg["code"])

        time.sleep(cfg["poll_interval"])


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        log.info("Stopped.")
