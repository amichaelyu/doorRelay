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

// ---------- User config ----------
const char* ssid     = "calvinsmart";
const char* password = "jagadeep";

// Static IP (must be in your subnet, ideally outside the DHCP pool)
IPAddress local_IP(192, 168, 0, 67);
IPAddress gateway(192, 168, 0, 1);
IPAddress subnet(255, 255, 255, 0);
IPAddress primaryDNS(1, 1, 1, 1);
IPAddress secondaryDNS(1, 0, 0, 1);

const uint16_t WS_PORT   = 81;      // WebSocket server port
const int      RELAY_PIN = 26;      // GPIO -> relay module trigger

// Shared secret. The relay ONLY fires when an inbound message equals this
// string exactly. Must match DOOR_CODE in config.yaml / door_monitor.py.
const char* DOOR_CODE = "k9$Wm2!qR7#zP4@xL8&";

// This module is HIGH-level trigger, so active-low is FALSE.
// (If you ever swap to an active-low module, flip this to true.)
const bool     ACTIVE_LOW = false;

const unsigned long PULSE_MS = 5000;   // 5 second pulse

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

void onWsEvent(uint8_t clientNum, WStype_t type, uint8_t* payload, size_t len) {
  switch (type) {
    case WStype_CONNECTED: {
      IPAddress ip = webSocket.remoteIP(clientNum);
      Serial.printf("Client %u connected from %s\n", clientNum, ip.toString().c_str());
      break;
    }
    case WStype_DISCONNECTED:
      Serial.printf("Client %u disconnected\n", clientNum);
      break;

    case WStype_TEXT:
    case WStype_BIN: {
      // Only fire when the message matches the shared secret exactly.
      size_t codeLen = strlen(DOOR_CODE);
      bool authorized = (len == codeLen) &&
                        (memcmp(payload, DOOR_CODE, codeLen) == 0);
      if (authorized) {
        Serial.printf("Valid code from client %u\n", clientNum);
        triggerRelay();
        webSocket.sendTXT(clientNum, "OK: relay on 5s");
      } else {
        Serial.printf("Rejected message from client %u (bad code)\n", clientNum);
        webSocket.sendTXT(clientNum, "DENIED");
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