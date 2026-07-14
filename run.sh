#!/usr/bin/env zsh
set -euo pipefail

cd "${0:A:h}"

jj git fetch
jj new master

# Start the door API locally, then forward door.yumichael.com to it through the
# Cloudflare tunnel. The tunnel's origin is http://127.0.0.1:8000 — the API's
# default bind — so door.yumichael.com/open reaches the API's /open endpoint.
uv run door_monitor_api.py &
API_PID=$!

# Ctrl-C (or any exit) tears down the API alongside the tunnel.
trap 'kill "$API_PID" 2>/dev/null || true' EXIT INT TERM

cloudflared tunnel run --url http://127.0.0.1:8000 doorrelay
