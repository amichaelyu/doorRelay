#!/bin/sh
# Xcode Run Script phase (app + widget targets): copy DOOR_API_TOKEN from the
# repo-root .env into the built bundle's Info.plist as DoorAPIToken, where
# DoorService reads it. Runs after Info.plist processing and before signing.
set -eu

ENV_FILE="${SRCROOT}/../.env"
PLIST="${TARGET_BUILD_DIR}/${INFOPLIST_PATH}"

if [ ! -f "$ENV_FILE" ]; then
  echo "error: $ENV_FILE not found. Add DOOR_API_TOKEN to it (see README)." >&2
  exit 1
fi

# Last DOOR_API_TOKEN= line wins; strip optional `export` and surrounding quotes.
TOKEN=$(sed -n -E 's/^[[:space:]]*(export[[:space:]]+)?DOOR_API_TOKEN[[:space:]]*=[[:space:]]*//p' "$ENV_FILE" \
  | tail -n 1 \
  | sed -E "s/^'(.*)'[[:space:]]*\$/\\1/; s/^\"(.*)\"[[:space:]]*\$/\\1/")

if [ -z "$TOKEN" ]; then
  echo "error: DOOR_API_TOKEN is missing or empty in $ENV_FILE" >&2
  exit 1
fi

plutil -replace DoorAPIToken -string "$TOKEN" "$PLIST"
