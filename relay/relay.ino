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

// ---------- User config ----------
const char* ssid     = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";

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
// HMAC-SHA256(secret, nonce). Must match `secret` in config.yaml.
const char* SHARED_SECRET = "k9$Wm2!qR7#zP4@xL8&";

// A nonce is single-use and expires this many ms after it is issued.
const unsigned long NONCE_TTL_MS = 30000;   // 30 seconds

// This module is HIGH-level trigger, so active-low is FALSE.
// (If you ever swap to an active-low module, flip this to true.)
const bool     ACTIVE_LOW = false;

const unsigned long PULSE_MS = 2000;   // 2 second pulse

// If true: a new request while already on RESTARTS the 5s window.
// If false: requests during an active pulse are ignored.
const bool RETRIGGER_EXTENDS = true;
// ---------------------------------

WebSocketsServer webSocket = WebSocketsServer(WS_PORT);

bool relayOn = false;
unsigned long relayOffAt = 0;

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

void setup() {
  Serial.begin(115200);

  pinMode(RELAY_PIN, OUTPUT);
  relayWrite(false);              // force OFF at boot, before WiFi/WS come up

  if (!WiFi.config(local_IP, gateway, subnet, primaryDNS, secondaryDNS)) {
    Serial.println("STA failed to configure static IP");
  }

  WiFi.begin(ssid, password);

  // Keep the radio awake for the lowest possible latency on incoming connections.
  WiFi.setSleep(false);

  Serial.print("Connecting");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);                   // only runs at startup, before the server exists
    Serial.print(".");
  }
  Serial.print("\nConnected. IP: ");
  Serial.println(WiFi.localIP());

  webSocket.begin();
  webSocket.onEvent(onWsEvent);
  Serial.printf("WebSocket server on ws://%s:%u/\n",
                WiFi.localIP().toString().c_str(), WS_PORT);
}

void loop() {
  webSocket.loop();               // must run every iteration; keeps sockets alive

  // Turn the relay off when the window elapses.
  // Cast to signed long so the comparison survives the ~49-day millis() rollover.
  if (relayOn && (long)(millis() - relayOffAt) >= 0) {
    relayOn = false;
    relayWrite(false);
    Serial.println("Relay OFF");
  }
}