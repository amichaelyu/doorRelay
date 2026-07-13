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
    uv run door_monitor.py [path/to/config.yaml]
"""

import os
import re
import sys
import hmac
import time
import hashlib
import sqlite3
import logging
import subprocess

import yaml
import websocket  # from the `websocket-client` package
from dotenv import load_dotenv

CHAT_DB = os.path.expanduser("~/Library/Messages/chat.db")


def env_flag(name, default=False):
    """Read a boolean-ish env var (1/true/yes/on -> True), else `default`."""
    val = os.environ.get(name)
    if val is None:
        return default
    return val.strip().lower() in ("1", "true", "yes", "on")

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s  %(levelname)s  %(message)s",
    datefmt="%Y-%m-%d %H:%M:%S",
)
log = logging.getLogger("door")


def iter_senders(cfg):
    """Yield (raw, name) for each authorized sender. `authorized_senders` is a
    `{number: name}` mapping; a bare list of numbers is also accepted (the name
    then defaults to the number)."""
    senders = cfg.get("authorized_senders") or {}
    items = senders.items() if isinstance(senders, dict) else ((s, None) for s in senders)
    for raw, name in items:
        raw = str(raw).strip()
        if raw:
            yield raw, (str(name).strip() if name else raw)


def load_config(path):
    with open(path, "r") as f:
        cfg = yaml.safe_load(f) or {}

    senders = list(iter_senders(cfg))
    if not senders:
        log.warning("No authorized_senders configured — nothing will ever trigger.")

    relay = cfg.get("relay") or {}
    setup = cfg.get("setup") or {}
    return {
        "authorized": {normalize_sender(raw) for raw, _ in senders},
        # (raw, name) pairs, used to seed Contacts when prod is enabled.
        "senders": senders,
        # Match the trigger word anywhere in the message (substring, any case).
        "trigger_re": re.compile(
            re.escape(str(cfg.get("trigger_word", "open"))),
            re.IGNORECASE,
        ),
        "host": relay.get("host", "192.168.0.67"),
        "port": int(relay.get("port", 81)),
        # `secret` is the HMAC key (never sent); `code` kept as a fallback name.
        "secret": str(relay.get("secret", relay.get("code", ""))),
        "poll_interval": float(cfg.get("poll_interval", 2)),
        "reply": cfg.get("reply") or {},
        "setup": {
            "enabled": bool(setup.get("enabled", False)),
            "trigger_re": re.compile(
                re.escape(str(setup.get("trigger_word", "setup"))),
                re.IGNORECASE,
            ),
            "message": setup.get("message", ""),
            "shortcut_url": setup.get("shortcut_url", ""),
        },
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


# chat.style = 45 is a 1:1 direct chat; 43 is a group chat. Joining through
# chat_message_join and requiring style 45 means a "open" in any group chat is
# ignored — only direct messages can open the door.
NEW_MESSAGES_SQL = """
    SELECT m.ROWID, h.id, m.text, m.attributedBody
    FROM message AS m
    LEFT JOIN handle AS h ON m.handle_id = h.ROWID
    JOIN chat_message_join AS cmj ON cmj.message_id = m.ROWID
    JOIN chat AS c ON c.ROWID = cmj.chat_id
    WHERE m.ROWID > ? AND m.is_from_me = 0 AND c.style = 45
    ORDER BY m.ROWID ASC
"""


def trigger_door(host, port, secret):
    """HMAC challenge-response: the secret is never transmitted.

        us  -> "HELLO"
        esp -> "NONCE:<hex>"
        us  -> "AUTH:" + HMAC-SHA256(secret, nonce)
        esp -> "OK: relay on 5s" | "DENIED"
    """
    url = f"ws://{host}:{port}/"
    log.info("Opening door via %s", url)
    try:
        ws = websocket.create_connection(url, timeout=5)
        try:
            ws.send("HELLO")
            resp = ws.recv()
            if not isinstance(resp, str) or not resp.startswith("NONCE:"):
                log.error("Unexpected challenge from relay: %r", resp)
                return False
            nonce = resp[len("NONCE:"):]
            mac = hmac.new(secret.encode(), nonce.encode(), hashlib.sha256).hexdigest()
            ws.send("AUTH:" + mac)
            result = ws.recv()
            log.info("Relay replied: %s", result)
            return isinstance(result, str) and result.startswith("OK")
        finally:
            ws.close()
    except Exception as e:
        log.error("Failed to reach relay at %s: %s", url, e)
        return False


# AppleScript takes the recipient + body as argv, so nothing from a text
# message is ever interpolated into the script source (no injection risk).
_SEND_SCRIPT = '''
on run {targetHandle, targetMessage}
    tell application "Messages"
        set svc to 1st account whose service type = iMessage
        try
            set toBuddy to participant targetHandle of svc
        on error
            set toBuddy to buddy targetHandle of svc
        end try
        send targetMessage to toBuddy
    end tell
end run
'''


def send_imessage(handle, text):
    try:
        subprocess.run(
            ["osascript", "-", handle, text],
            input=_SEND_SCRIPT,
            text=True, capture_output=True, timeout=15, check=True,
        )
        log.info("Replied to %s: %r", handle, text)
        return True
    except subprocess.CalledProcessError as e:
        log.error("Reply to %s failed: %s", handle, (e.stderr or "").strip())
    except Exception as e:
        log.error("Reply to %s failed: %s", handle, e)
    return False


def send_setup(handle, setup):
    """Reply with the Shortcut link so the sender can add it."""
    msg_parts = [p for p in (setup.get("message"), setup.get("shortcut_url")) if p]
    if msg_parts:
        send_imessage(handle, "\n".join(msg_parts))


# Dumps every stored phone value, one per line, so we can dedupe by digits.
_LIST_PHONES_SCRIPT = '''
tell application "Contacts"
    set out to ""
    repeat with p in people
        repeat with ph in (phones of p)
            set out to out & (value of ph) & linefeed
        end repeat
    end repeat
    return out
end tell
'''


def existing_contact_digits():
    """Set of last-10-digit keys for every phone already in Contacts, or None if
    Contacts can't be read (e.g. Automation access not yet granted)."""
    try:
        res = subprocess.run(
            ["osascript", "-e", _LIST_PHONES_SCRIPT],
            capture_output=True, text=True, timeout=60, check=True,
        )
    except subprocess.CalledProcessError as e:
        log.error("Cannot read Contacts (grant Automation access to your "
                  "terminal in Privacy & Security): %s", (e.stderr or "").strip())
        return None
    except Exception as e:
        log.error("Cannot read Contacts: %s", e)
        return None
    digits = set()
    for line in res.stdout.splitlines():
        d = re.sub(r"\D", "", line)
        if len(d) >= 10:
            digits.add(d[-10:])
    return digits


# argv-only, same as the other send scripts — no name/number is interpolated
# into the source, so nothing from config can inject AppleScript.
_CREATE_CONTACT_SCRIPT = '''
on run {targetName, targetPhone}
    tell application "Contacts"
        set newPerson to make new person with properties {first name:targetName}
        make new phone at end of phones of newPerson with properties {label:"mobile", value:targetPhone}
        save
    end tell
end run
'''


def create_contact(name, phone):
    try:
        subprocess.run(
            ["osascript", "-", name, phone],
            input=_CREATE_CONTACT_SCRIPT,
            text=True, capture_output=True, timeout=30, check=True,
        )
        log.info("Added contact %s (%s)", name, phone)
        return True
    except subprocess.CalledProcessError as e:
        log.error("Failed to add contact %s (%s): %s", name, phone, (e.stderr or "").strip())
    except Exception as e:
        log.error("Failed to add contact %s (%s): %s", name, phone, e)
    return False


def import_contacts(cfg):
    """Add every authorized phone number to Contacts if not already present."""
    # Only phone entries can become phone contacts; skip email senders.
    senders = [(raw, name) for raw, name in cfg["senders"] if "@" not in raw]
    if not senders:
        log.info("No phone senders to import.")
        return
    existing = existing_contact_digits()
    if existing is None:
        return  # couldn't read Contacts; error already logged
    added = 0
    for raw, name in senders:
        digits = normalize_sender(raw)
        if digits and digits in existing:
            continue
        if create_contact(name, raw):
            existing.add(digits)
            added += 1
    log.info("Contact import: %d added, %d already present.", added, len(senders) - added)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    load_dotenv(os.path.join(here, ".env"))
    cfg_path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, "config.yaml")
    cfg = load_config(cfg_path)
    log.info(
        "Watching chat.db | %d authorized sender(s) | relay ws://%s:%d",
        len(cfg["authorized"]), cfg["host"], cfg["port"],
    )

    # prod defaults to false; when true, seed Contacts with authorized senders.
    if env_flag("prod"):
        log.info("prod=true — importing authorized senders into Contacts…")
        import_contacts(cfg)

    try:
        conn = open_db()
    except sqlite3.OperationalError as e:
        log.error("Cannot open chat.db (%s). Grant Full Disk Access to your "
                  "terminal/python in System Settings > Privacy & Security.", e)
        sys.exit(1)

    last_rowid = high_watermark(conn)
    log.info("Starting from message ROWID %d — only new messages will count.", last_rowid)

    # Send a heartbeat message every second so we know the monitor is alive.
    heartbeat_handle = "door@yumichael.com"
    heartbeat_interval = 1.0
    next_heartbeat = time.monotonic()

    while True:
        now = time.monotonic()
        if now >= next_heartbeat:
            send_imessage(heartbeat_handle, "door monitor heartbeat")
            next_heartbeat = now + heartbeat_interval

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
            setup = cfg["setup"]
            if setup["enabled"] and setup["trigger_re"].search(body):
                log.info("SETUP request from %s: %r", sender, body)
                send_setup(sender, setup)
                continue
            if not cfg["trigger_re"].search(body):
                log.info("Authorized msg from %s (no trigger word): %r", sender, body)
                continue
            log.info("TRIGGER from %s: %r", sender, body)
            opened = trigger_door(cfg["host"], cfg["port"], cfg["secret"])
            reply = cfg["reply"]
            if reply.get("enabled"):
                if opened:
                    open_msg = reply.get("on_open")
                    if open_msg:
                        send_imessage(sender, open_msg)
                    close_msg = reply.get("on_close")
                    if close_msg:
                        time.sleep(2)
                        send_imessage(sender, close_msg)
                else:
                    fail_msg = reply.get("on_fail")
                    if fail_msg:
                        send_imessage(sender, fail_msg)

        time.sleep(cfg["poll_interval"])


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        log.info("Stopped.")
