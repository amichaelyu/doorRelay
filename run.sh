#!/usr/bin/env zsh
set -euo pipefail

cd "${0:A:h}"

jj git fetch
jj new master
uv run door_monitor.py
