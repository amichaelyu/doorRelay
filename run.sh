#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

# Dev-machine convenience: hop to the latest master before building. Skipped on
# a deploy box, which won't have a jj colocated repo.
if command -v jj >/dev/null 2>&1 && [ -d .jj ]; then
  jj git fetch
  jj new master
fi

# Both the door API and the Cloudflare tunnel come up together; the tunnel
# reaches the API over their shared loopback (see compose.yaml). Ctrl-C tears
# down the pair.
trap 'docker compose down --remove-orphans' EXIT INT TERM

docker compose up --build
