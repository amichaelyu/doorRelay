"""
HTTP API that opens the door, meant to sit behind a Cloudflare Worker at
door.yumichael.com/open.

The Worker terminates the public request, checks whatever it checks, and
forwards an authenticated call to this service. This service runs on the Mac
that shares the relay's LAN, so it can perform the HMAC handshake with the
ESP32 (see relay.ino) and pulse the relay — the same `trigger_door` path the
chat.db monitor uses.

Reaching this box from a Cloudflare Worker means a public path in: either a
Cloudflare Tunnel (`cloudflared tunnel`) pointed at this port, or a port
forward. Bind to 127.0.0.1 (the default) when a tunnel fronts it so the door
opener is never directly exposed on the LAN/WAN.

Auth: every request must present the shared API token, as either
    Authorization: Bearer <token>
    X-Door-Token: <token>
The token comes from $DOOR_API_TOKEN (preferred) or config.yaml `api.token`.
Startup refuses to run without one — an unauthenticated door opener is a hole.

Usage:
    uv run door_monitor_api.py [path/to/config.yaml]

Env:
    DOOR_API_TOKEN   shared secret the Worker must present (required)
    DOOR_API_HOST    bind address (default 127.0.0.1)
    DOOR_API_PORT    bind port (default 8000)
"""

import os
import sys
import hmac
import logging
from typing import Optional

import uvicorn
from fastapi import FastAPI, Header, HTTPException, status
from pydantic import BaseModel
from dotenv import load_dotenv

# Reuse the config loader and the relay handshake from the chat.db monitor so
# there is exactly one place that knows how to talk to the ESP32.
import door_monitor

log = logging.getLogger("door.api")


def load_api_token(cfg):
    """Shared secret the Worker must present. $DOOR_API_TOKEN wins over the
    config `api.token` so the secret can stay out of the committed config."""
    token = os.environ.get("DOOR_API_TOKEN")
    if token:
        return token.strip()
    return str((cfg.get("api") or {}).get("token", "")).strip()


class OpenResult(BaseModel):
    opened: bool
    detail: str


def create_app(cfg, token):
    app = FastAPI(title="doorRelay API", docs_url=None, redoc_url=None)

    def check_auth(authorization, x_door_token):
        """Accept the token from either header; constant-time compare so a
        wrong guess leaks no timing signal."""
        presented = x_door_token
        if not presented and authorization:
            scheme, _, value = authorization.partition(" ")
            if scheme.lower() == "bearer":
                presented = value.strip()
        # Compare as bytes: compare_digest on str raises on non-ASCII input, so
        # a malformed header would 500 instead of cleanly failing auth. Encoding
        # keeps it constant-time and total over any header value.
        ok = bool(presented) and hmac.compare_digest(
            presented.encode("utf-8"), token.encode("utf-8")
        )
        if not ok:
            raise HTTPException(
                status_code=status.HTTP_401_UNAUTHORIZED,
                detail="missing or invalid token",
            )

    @app.get("/health")
    def health():
        return {"ok": True}

    @app.post("/open", response_model=OpenResult)
    def open_door(
        authorization: Optional[str] = Header(default=None),
        x_door_token: Optional[str] = Header(default=None),
    ):
        check_auth(authorization, x_door_token)
        log.info("Open request accepted — triggering relay.")
        opened = door_monitor.trigger_door(cfg["host"], cfg["port"], cfg["secret"])
        if opened:
            return OpenResult(opened=True, detail="relay pulsed")
        # 502: we authenticated fine but the relay couldn't be reached/refused.
        raise HTTPException(
            status_code=status.HTTP_502_BAD_GATEWAY,
            detail="relay unreachable or denied",
        )

    return app


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    load_dotenv(os.path.join(here, ".env"))
    cfg_path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, "config.yaml")
    cfg = door_monitor.load_config(cfg_path)

    token = load_api_token(cfg)
    if not token:
        log.error(
            "No API token set. Put DOOR_API_TOKEN in .env (or api.token in "
            "config.yaml) before starting — refusing to run open."
        )
        sys.exit(1)

    host = os.environ.get("DOOR_API_HOST", "127.0.0.1")
    port = int(os.environ.get("DOOR_API_PORT", "8000"))
    log.info(
        "doorRelay API on http://%s:%d | relay ws://%s:%d",
        host, port, cfg["host"], cfg["port"],
    )

    app = create_app(cfg, token)
    uvicorn.run(app, host=host, port=port, log_level="info")


if __name__ == "__main__":
    main()
