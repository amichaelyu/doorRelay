# door relay

esp32 firmware to act as a relay +
docker container to expose the relay as an API +
iOS app with shortcuts and Siri support

## flashing esp32 relay

```
relay/flash.sh                                  # finds the USB serial port itself
PORT=/dev/cu.usbserial-0001 relay/flash.sh      # or name the port
```

## env

everything reads one `.env` in the repo root (gitignored). wrap values in single
quotes so docker compose doesn't expand `$` in them.

```
DOOR_API_TOKEN='...'       # docker api: token the iOS app / shortcut must send (Bearer or X-Door-Token)
RELAY_SECRET='...'         # docker api + mac monitor + esp32: shared HMAC key for the relay handshake
RELAY_WIFI_SSID='...'      # esp32 only: baked in by relay/flash.sh
RELAY_WIFI_PASSWORD='...'  # esp32 only: baked in by relay/flash.sh
prod=false                 # mac monitor only: true imports authorized senders into Contacts
```

optional: `CLOUDFLARED_DIR` (where the tunnel credentials live, default
`~/.cloudflared`) and `TZ` (watchtower's 4am update schedule, default
`America/Los_Angeles`).

`RELAY_SECRET` must be the same everywhere, so reflash the esp32 after changing it.

## cloudflared setup

the docker stack exposes the api through a cloudflare tunnel (no ports opened).
one-time, on any machine with `cloudflared` installed
([downloads](https://developers.cloudflare.com/tunnel/downloads/)). the server
itself doesn't need it (it runs in a container), but the steps below work there too:

```
cloudflared tunnel login                     # pick the zone; writes ~/.cloudflared/cert.pem
cloudflared tunnel create doorrelay          # writes ~/.cloudflared/<TUNNEL_ID>.json
cloudflared tunnel route dns --overwrite-dns doorrelay door.yumichael.com
```

then put the new `<TUNNEL_ID>` in `compose.yaml` (the `cloudflared` command uses it
twice: in `--credentials-file` and as the last argument), and copy the credentials
file to the server:

```
scp ~/.cloudflared/<TUNNEL_ID>.json server:~/.cloudflared/
```

the server only needs that json, not `cert.pem`.

## setting up docker monitor

Docker and [Jujutsu](https://github.com/jj-vcs/jj) are needed 

to start the container on you're server, run:
```
bash run.sh
```
this also fetches the latest commit from the repo using jj and then starts the container with Docker
