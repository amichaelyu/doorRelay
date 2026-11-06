# The API half of doorRelay has no macOS dependencies: door_monitor_api.py
# borrows only load_config and trigger_door from door_monitor, and both are pure
# Python (yaml + hmac + a websocket to the ESP32). The chat.db monitor stays a
# native macOS process — it needs ~/Library/Messages and osascript for replies.
FROM ghcr.io/astral-sh/uv:python3.12-bookworm-slim

ENV UV_COMPILE_BYTECODE=1 \
    UV_LINK_MODE=copy \
    PYTHONUNBUFFERED=1 \
    PATH="/app/.venv/bin:$PATH"

WORKDIR /app

# Dependencies first, so editing the two .py files below doesn't re-resolve the
# lockfile on every rebuild.
COPY pyproject.toml uv.lock ./
RUN uv sync --frozen --no-dev

COPY door_monitor.py door_monitor_api.py ./

# No port is published. cloudflared shares this container's network namespace,
# so the API keeps its 127.0.0.1 bind and the tunnel is the only thing that can
# reach it — same invariant the module docstring asks for.
HEALTHCHECK --interval=30s --timeout=3s --start-period=5s --retries=3 \
    CMD python -c "import urllib.request; urllib.request.urlopen('http://127.0.0.1:8000/health').read()"

CMD ["python", "door_monitor_api.py"]
