/*
 * ESP32 WebSocket-triggered relay pulse
 * -------------------------------------
 * When any WebSocket client sends a message, the relay is energized
 * for 5 seconds, then turns itself off. Non-blocking: the socket stays
 * responsive the whole time (no delay(5000) anywhere).
 *
 * Hardware chain:
 *   ESP32 GPIO -> opto-isolated relay module (HIGH-level trigger)
 *              -> module contacts switch 24VAC into MY2NJ coil (A1/A2)
 *              -> MY2NJ DPDT contacts switch your actual load
 *
 * Library required (install via Library Manager):
 *   "WebSockets" by Markus Sattler  (a.k.a. arduinoWebSockets / Links2004)
 */

#include <WiFi.h>
#include <WebSocketsServer.h>
#include <esp_random.h>     // hardware RNG for unguessable nonces
#include "mbedtls/md.h"     // HMAC-SHA256
#include "secrets.h"        // generated from ../.env by flash.sh (gitignored)

// ---------- User config ----------
const char* ssid     = RELAY_WIFI_SSID;
const char* password = RELAY_WIFI_PASSWORD;

// Static IP (must be in your subnet, ideally outside the DHCP pool)
IPAddress local_IP(192, 168, 0, 67);
IPAddress gateway(192, 168, 0, 1);
IPAddress subnet(255, 255, 255, 0);
IPAddress primaryDNS(1, 1, 1, 1);
IPAddress secondaryDNS(1, 0, 0, 1);

const uint16_t WS_PORT   = 81;      // WebSocket server port
const int      RELAY_PIN = 23;      // GPIO -> relay module trigger

// Shared HMAC secret. This NEVER travels over the wire. Instead the relay
// challenges each client with a fresh random nonce; the client must reply with
// HMAC-SHA256(secret, nonce). Comes from RELAY_SECRET in .env, which the
// Python side reads too, so both ends share one source.
const char* SHARED_SECRET = RELAY_SECRET;

// A nonce is single-use and expires this many ms after it is issued.
const unsigned long NONCE_TTL_MS = 30000;   // 30 seconds

// This module is HIGH-level trigger, so active-low is FALSE.
// (If you ever swap to an active-low module, flip this to true.)
const bool     ACTIVE_LOW = false;

const unsigned long PULSE_MS = 2000;   // 2 second pulse

// Connection maintenance. WebSocket heartbeats discard clients that vanished
// without a TCP close. WiFi retries escalate from a normal reconnect to a full
// radio reset. A daily refresh bounds the time spent in the rare state where
// the driver still reports WL_CONNECTED but the network path is no longer live.
const unsigned long WIFI_RETRY_MS = 10000;              // normal retry every 10s
const unsigned long WIFI_RADIO_RESET_MS = 60000;        // hard recovery after 60s
const unsigned long WIFI_REFRESH_MS = 24UL * 60UL * 60UL * 1000UL;
const uint32_t WS_PING_INTERVAL_MS = 30000;
const uint32_t WS_PONG_TIMEOUT_MS = 5000;
const uint8_t WS_MISSED_PONG_LIMIT = 2;

// If true: a new request while already on RESTARTS the 5s window.
// If false: requests during an active pulse are ignored.
const bool RETRIGGER_EXTENDS = true;
// ---------------------------------

WebSocketsServer webSocket = WebSocketsServer(WS_PORT);

bool relayOn = false;
unsigned long relayOffAt = 0;
bool webSocketRunning = false;
bool wifiWasConnected = false;
unsigned long wifiDownSince = 0;
unsigned long lastWifiAttempt = 0;
unsigned long lastWifiRefresh = 0;

void relayWrite(bool on) {
  // XOR with ACTIVE_LOW so one flag handles both trigger polarities
  digitalWrite(RELAY_PIN, (on ^ ACTIVE_LOW) ? HIGH : LOW);
}

void triggerRelay() {
  if (relayOn && !RETRIGGER_EXTENDS) {
    return;                       // already pulsing, ignore per policy
  }
  relayOn = true;
  relayOffAt = millis() + PULSE_MS;
  relayWrite(true);
  Serial.println("Relay ON (5s)");
}

// ---- Challenge-response state (one pending nonce per WS client) ----
#define MAX_WS_CLIENTS 8
struct Challenge {
  char nonce[33];          // 16 random bytes as 32 hex chars + NUL
  unsigned long issuedAt;  // millis() when issued
  bool valid;              // false once used or never issued
};
Challenge challenges[MAX_WS_CLIENTS];

// HMAC-SHA256(key, msg) -> 64-char lowercase hex string (out must hold 65).
void hmacSha256Hex(const char* key, const char* msg, char* out) {
  uint8_t mac[32];
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, info, 1);            // 1 => HMAC mode
  mbedtls_md_hmac_starts(&ctx, (const uint8_t*)key, strlen(key));
  mbedtls_md_hmac_update(&ctx, (const uint8_t*)msg, strlen(msg));
  mbedtls_md_hmac_finish(&ctx, mac);
  mbedtls_md_free(&ctx);
  for (int i = 0; i < 32; i++) sprintf(out + i * 2, "%02x", mac[i]);
  out[64] = '\0';
}

// Fill out[33] with 16 hardware-random bytes as hex.
void makeNonce(char* out) {
  for (int i = 0; i < 16; i++) sprintf(out + i * 2, "%02x", (uint8_t)(esp_random() & 0xFF));
  out[32] = '\0';
}

// Constant-time compare of two n-byte strings (avoids HMAC timing leaks).
bool constTimeEqual(const char* a, const char* b, size_t n) {
  uint8_t diff = 0;
  for (size_t i = 0; i < n; i++) diff |= (uint8_t)a[i] ^ (uint8_t)b[i];
  return diff == 0;
}

void onWsEvent(uint8_t clientNum, WStype_t type, uint8_t* payload, size_t len) {
  switch (type) {
    case WStype_CONNECTED: {
      IPAddress ip = webSocket.remoteIP(clientNum);
      Serial.printf("Client %u connected from %s\n", clientNum, ip.toString().c_str());
      if (clientNum < MAX_WS_CLIENTS) challenges[clientNum].valid = false;
      break;
    }
    case WStype_DISCONNECTED:
      Serial.printf("Client %u disconnected\n", clientNum);
      if (clientNum < MAX_WS_CLIENTS) challenges[clientNum].valid = false;
      break;

    case WStype_TEXT:
    case WStype_BIN: {
      if (clientNum >= MAX_WS_CLIENTS) break;   // out of tracked range, ignore

      // Protocol:
      //   client -> "HELLO"          request a challenge
      //   server -> "NONCE:<hex>"    fresh single-use nonce
      //   client -> "AUTH:<hmachex>" HMAC-SHA256(secret, nonce)
      //   server -> "OK: relay on 5s" | "DENIED"
      String msg;
      msg.reserve(len + 1);
      for (size_t i = 0; i < len; i++) msg += (char)payload[i];   // no NUL assumption

      if (msg.startsWith("AUTH:")) {
        String provided = msg.substring(5);
        Challenge& ch = challenges[clientNum];
        bool fresh = ch.valid && (millis() - ch.issuedAt < NONCE_TTL_MS);
        ch.valid = false;                       // single-use: consume regardless

        char expected[65];
        hmacSha256Hex(SHARED_SECRET, ch.nonce, expected);
        bool ok = fresh && provided.length() == 64 &&
                  constTimeEqual(provided.c_str(), expected, 64);

        if (ok) {
          Serial.printf("Client %u authenticated\n", clientNum);
          triggerRelay();
          webSocket.sendTXT(clientNum, "OK: relay on 5s");
        } else {
          Serial.printf("Client %u auth FAILED\n", clientNum);
          webSocket.sendTXT(clientNum, "DENIED");
        }
      } else {
        // Any other message is treated as a challenge request.
        Challenge& ch = challenges[clientNum];
        makeNonce(ch.nonce);
        ch.issuedAt = millis();
        ch.valid = true;
        String reply = "NONCE:";
        reply += ch.nonce;
        webSocket.sendTXT(clientNum, reply);
        Serial.printf("Issued nonce to client %u\n", clientNum);
      }
      break;
    }

    default:
      break;
  }
}

void startWebSocket() {
  if (webSocketRunning) return;

  webSocket.begin();
  webSocket.onEvent(onWsEvent);
  webSocket.enableHeartbeat(WS_PING_INTERVAL_MS, WS_PONG_TIMEOUT_MS,
                            WS_MISSED_PONG_LIMIT);
  webSocketRunning = true;
  Serial.printf("WebSocket server on ws://%s:%u/\n",
                WiFi.localIP().toString().c_str(), WS_PORT);
}

void stopWebSocket() {
  if (!webSocketRunning) return;

  // A WiFi disconnect invalidates the underlying TCP sockets. Close both the
  // clients and listener so they are recreated on the recovered interface.
  webSocket.disconnect();
  webSocket.close();
  webSocketRunning = false;
  for (uint8_t i = 0; i < MAX_WS_CLIENTS; i++) challenges[i].valid = false;
}

void configureAndBeginWifi() {
  WiFi.mode(WIFI_STA);
  if (!WiFi.config(local_IP, gateway, subnet, primaryDNS, secondaryDNS)) {
    Serial.println("STA failed to configure static IP");
  }
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid, password);
}

void resetWifiRadio(unsigned long now) {
  Serial.println("WiFi recovery: resetting radio and TCP sockets");
  stopWebSocket();
  WiFi.disconnect(true, false);   // radio off; retain saved AP credentials
  configureAndBeginWifi();
  wifiWasConnected = false;
  wifiDownSince = now;
  lastWifiAttempt = now;
}

void setup() {
  Serial.begin(115200);

  pinMode(RELAY_PIN, OUTPUT);
  relayWrite(false);              // force OFF at boot, before WiFi/WS come up

  configureAndBeginWifi();

  // Wait briefly for the first join, but never block forever. If the AP is down
  // at boot we still start the server and keep retrying from loop(). We do NOT
  // reboot on WiFi loss: with no AP to reach, a restart just boot-loops, whereas
  // a running relay reconnects the moment WiFi comes back.
  Serial.print("Connecting");
  unsigned long startedAt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startedAt < 15000) {
    delay(500);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("\nConnected. IP: ");
    Serial.println(WiFi.localIP());
    wifiWasConnected = true;
    lastWifiRefresh = millis();
    startWebSocket();
  } else {
    Serial.println("\nNo WiFi yet; will keep retrying.");
    wifiDownSince = millis();
  }
}

// Non-blocking connection state machine. It also rebuilds the WebSocket listener
// after every WiFi recovery because the old TCP sockets no longer belong to a
// usable network interface.
void ensureWifi() {
  unsigned long now = millis();
  bool connected = WiFi.status() == WL_CONNECTED;

  if (connected) {
    if (!wifiWasConnected) {
      wifiWasConnected = true;
      wifiDownSince = 0;
      lastWifiRefresh = now;
      Serial.printf("WiFi recovered. IP: %s, RSSI: %d dBm\n",
                    WiFi.localIP().toString().c_str(), WiFi.RSSI());
      startWebSocket();
    }

    // WiFi.status() is only the driver's link state, not an end-to-end health
    // check. Refresh periodically so a falsely connected state cannot persist
    // indefinitely. This causes a short, intentional disconnect once per day.
    if (now - lastWifiRefresh >= WIFI_REFRESH_MS) {
      Serial.println("Scheduled WiFi refresh");
      resetWifiRadio(now);
    }
    return;
  }

  if (wifiWasConnected || wifiDownSince == 0) {
    Serial.printf("WiFi lost (status %d); closing stale sockets\n", WiFi.status());
    stopWebSocket();
    wifiWasConnected = false;
    wifiDownSince = now;
    lastWifiAttempt = 0;          // make the first retry immediate
  }

  if (now - wifiDownSince >= WIFI_RADIO_RESET_MS) {
    resetWifiRadio(now);
    return;
  }

  if (lastWifiAttempt == 0 || now - lastWifiAttempt >= WIFI_RETRY_MS) {
    lastWifiAttempt = now;
    Serial.println("WiFi down; requesting reconnect...");
    WiFi.reconnect();
  }
}

void loop() {
  if (webSocketRunning) {
    webSocket.loop();             // keeps sockets and heartbeat processing alive
  }

  ensureWifi();                   // reconnect in the background if the link dropped

  // Turn the relay off when the window elapses.
  // Cast to signed long so the comparison survives the ~49-day millis() rollover.
  if (relayOn && (long)(millis() - relayOffAt) >= 0) {
    relayOn = false;
    relayWrite(false);
    Serial.println("Relay OFF");
  }

  delay(1);   // yield to the RTOS (WiFi/TCP tasks + watchdog); ~1ms, no real latency cost
}
