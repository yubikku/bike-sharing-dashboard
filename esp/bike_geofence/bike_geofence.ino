/*
  Smartify Bike Sharing Prototype - Bike Geofence Node
  Board: ESP32-DevKitC V4
  Arduino-ESP32 core: 3.x

  Responsibilities:
    - Sends heartbeat packets to the station ESP32 using ESP-NOW.
    - Receives the station's RSSI-based inside/outside boundary decision.
    - Drives a green LED, red LED, and two-pin passive piezo buzzer.
    - Supports a local button/switch as a reliable manual boundary override.

  BEFORE UPLOADING:
    1. Upload station_controller.ino and open Serial Monitor.
    2. Copy the printed Station ESP32 MAC address into STATION_MAC below.
    3. Copy the printed Wi-Fi / ESP-NOW channel into ESPNOW_CHANNEL below.
       ESP-NOW must operate on the same channel as the Firebase-connected station.
*/

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

// -----------------------------------------------------------------------------
// Configuration
// -----------------------------------------------------------------------------

constexpr char BIKE_ID[] = "bike1";

// 00:4B:12:23:98:4C
// Replace with the station ESP32 MAC address printed in its Serial Monitor.
uint8_t STATION_MAC[6] = {0x00, 0x4B, 0x12, 0x23, 0x98, 0x4C};

// Replace with the channel printed by the station after it connects to Wi-Fi.
constexpr uint8_t ESPNOW_CHANNEL = 5;

// Safe GPIO choices for ESP32-DevKitC V4.
constexpr uint8_t BOUNDARY_SWITCH_PIN = 25; // Switch/button to GND; LOW forces outside.
constexpr uint8_t GREEN_LED_PIN = 26;
constexpr uint8_t RED_LED_PIN = 27;
constexpr uint8_t PIEZO_PIN = 33;

constexpr unsigned long HEARTBEAT_INTERVAL_MS = 250;

// Communication loss must not be interpreted as a geofence crossing.
// After 5 seconds we print a stale-reply warning.
// After 30 seconds we print an extended-loss warning.
// In both cases, the bike retains the last station-confirmed state.
constexpr unsigned long STATION_REPLY_STALE_WARNING_MS = 5000;
constexpr unsigned long STATION_REPLY_HOLD_LAST_STATE_MS = 30000;

constexpr uint32_t PIEZO_ALERT_FREQUENCY_HZ = 2200;

// -----------------------------------------------------------------------------
// Packet definitions: must match station_controller.ino
// -----------------------------------------------------------------------------

enum MessageKind : uint8_t {
  MESSAGE_BIKE_HEARTBEAT = 1,
  MESSAGE_STATION_GEOFENCE_REPLY = 2
};

struct __attribute__((packed)) BikeHeartbeat {
  uint8_t kind;
  char bikeId[12];
  uint32_t sequence;
  uint8_t manualOutside;
};

struct __attribute__((packed)) StationGeofenceReply {
  uint8_t kind;
  char bikeId[12];
  int8_t lastRssi;
  int16_t smoothedRssi;
  uint8_t outsideBoundary;
  uint8_t connected;
};

// -----------------------------------------------------------------------------
// Runtime state
// -----------------------------------------------------------------------------

portMUX_TYPE receiveMux = portMUX_INITIALIZER_UNLOCKED;
StationGeofenceReply pendingReply;
volatile bool hasPendingReply = false;

uint32_t heartbeatSequence = 0;
unsigned long lastHeartbeatMs = 0;
unsigned long lastReplyMs = 0;
bool hasEverReceivedReply = false;
bool outsideBoundary = false;

// These variables describe communication loss only.
// They must not change the geofence decision.
bool stationReplyStale = false;
bool extendedReplyLossLogged = false;

int lastRssi = -127;
int smoothedRssi = -127;

// -----------------------------------------------------------------------------
// Hardware outputs
// -----------------------------------------------------------------------------

void setAlertOutputs(bool alert) {
  digitalWrite(GREEN_LED_PIN, alert ? LOW : HIGH);
  digitalWrite(RED_LED_PIN, alert ? HIGH : LOW);
  if (alert) {
    ledcWriteTone(PIEZO_PIN, PIEZO_ALERT_FREQUENCY_HZ);
  } else {
    ledcWriteTone(PIEZO_PIN, 0);
  }
}

bool manualBoundarySwitchActive() {
  return digitalRead(BOUNDARY_SWITCH_PIN) == LOW;
}

// The station-confirmed RSSI decision controls the normal output state.
// The local manual switch remains available as a deterministic demo override,
// even if station replies temporarily stop.

void applyOutputsFromConfirmedState() {
  bool localManualAlert = manualBoundarySwitchActive();
  setAlertOutputs(localManualAlert || outsideBoundary);
}

// -----------------------------------------------------------------------------
// ESP-NOW callbacks and functions
// -----------------------------------------------------------------------------

void onEspNowReceive(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len != static_cast<int>(sizeof(StationGeofenceReply))) {
    return;
  }

  StationGeofenceReply received;
  memcpy(&received, data, sizeof(received));
  if (received.kind != MESSAGE_STATION_GEOFENCE_REPLY) {
    return;
  }

  portENTER_CRITICAL(&receiveMux);
  pendingReply = received;
  hasPendingReply = true;
  portEXIT_CRITICAL(&receiveMux);
}

bool addStationPeer() {
  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, STATION_MAC, 6);
  peerInfo.channel = ESPNOW_CHANNEL;
  peerInfo.encrypt = false;
  peerInfo.ifidx = WIFI_IF_STA;

  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("Failed to register station ESP-NOW peer. Check station MAC and channel.");
    return false;
  }
  return true;
}

void sendHeartbeat() {
  BikeHeartbeat heartbeat = {};
  heartbeat.kind = MESSAGE_BIKE_HEARTBEAT;
  strncpy(heartbeat.bikeId, BIKE_ID, sizeof(heartbeat.bikeId) - 1);
  heartbeat.sequence = ++heartbeatSequence;
  heartbeat.manualOutside = manualBoundarySwitchActive() ? 1 : 0;

  esp_err_t result = esp_now_send(STATION_MAC, reinterpret_cast<uint8_t *>(&heartbeat), sizeof(heartbeat));
  if (result != ESP_OK) {
    Serial.printf("Heartbeat queue failed, ESP-NOW error code: %d\n", static_cast<int>(result));
  }
}

void processStationReply() {
    StationGeofenceReply reply;
  bool available = false;

  portENTER_CRITICAL(&receiveMux);
  if (hasPendingReply) {
    reply = pendingReply;
    hasPendingReply = false;
    available = true;
  }
  portEXIT_CRITICAL(&receiveMux);

  if (!available || String(reply.bikeId) != BIKE_ID) {
    return;
  }

  bool recoveredFromStaleReply = stationReplyStale;

  hasEverReceivedReply = true;
  lastReplyMs = millis();
  stationReplyStale = false;
  extendedReplyLossLogged = false;

  lastRssi = reply.lastRssi;
  smoothedRssi = reply.smoothedRssi;

  if (recoveredFromStaleReply) {
    Serial.println(
      "Station replies recovered. Resuming station-confirmed geofence updates."
    );
  }

  bool newOutside = reply.outsideBoundary != 0;

  if (newOutside != outsideBoundary) {
    outsideBoundary = newOutside;

    Serial.printf(
      "Boundary status changed: %s | last RSSI=%d dBm | smoothed RSSI=%d dBm\n",
      outsideBoundary ? "OUTSIDE - ALERT" : "INSIDE",
      lastRssi,
      smoothedRssi
    );
  }

  applyOutputsFromConfirmedState();
}

void monitorReplyTimeout() {
  // Keep the manual override responsive even during a station/Firebase stall.
  // Otherwise preserve the last RSSI-based state confirmed by the station.
  applyOutputsFromConfirmedState();

  if (!hasEverReceivedReply) {
    return;
  }

  const unsigned long silentForMs = millis() - lastReplyMs;

  if (!stationReplyStale &&
      silentForMs > STATION_REPLY_STALE_WARNING_MS) {
    stationReplyStale = true;

    Serial.printf(
      "Station replies unavailable for more than %lu seconds. "
      "Holding last confirmed geofence state: %s. "
      "No boundary alert generated by link loss.\n",
      STATION_REPLY_STALE_WARNING_MS / 1000,
      outsideBoundary ? "OUTSIDE / ALERT" : "INSIDE"
    );
  }

  if (!extendedReplyLossLogged &&
      silentForMs > STATION_REPLY_HOLD_LAST_STATE_MS) {
    extendedReplyLossLogged = true;

    Serial.printf(
      "Station replies unavailable for more than %lu seconds. "
      "Still holding last confirmed state: %s. "
      "Use the local boundary switch for a deterministic alert while the link recovers.\n",
      STATION_REPLY_HOLD_LAST_STATE_MS / 1000,
      outsideBoundary ? "OUTSIDE / ALERT" : "INSIDE"
    );
  }
}

// -----------------------------------------------------------------------------
// Setup and main loop
// -----------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println();
  Serial.println("============================================================");
  Serial.println("Smartify Bike Node - ESP-NOW RSSI Boundary Simulation");
  Serial.println("============================================================");

  pinMode(BOUNDARY_SWITCH_PIN, INPUT_PULLUP);
  pinMode(GREEN_LED_PIN, OUTPUT);
  pinMode(RED_LED_PIN, OUTPUT);

  // Arduino-ESP32 3.x LEDC API: attach PWM output used to produce a passive piezo tone.
  if (!ledcAttach(PIEZO_PIN, PIEZO_ALERT_FREQUENCY_HZ, 8)) {
    Serial.println("Warning: could not attach piezo PWM output.");
  }
  setAlertOutputs(false);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);

  if (esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE) != ESP_OK) {
    Serial.println("Failed to set ESP-NOW Wi-Fi channel.");
  }

  Serial.print("Bike ESP32 MAC: ");
  Serial.println(WiFi.macAddress());
  Serial.print("Bike ESP-NOW channel: ");
  Serial.println(ESPNOW_CHANNEL);

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW initialization failed. Halted.");
    while (true) {
      delay(1000);
    }
  }

  if (esp_now_register_recv_cb(onEspNowReceive) != ESP_OK) {
    Serial.println("ESP-NOW receive callback registration failed. Halted.");
    while (true) {
      delay(1000);
    }
  }

  if (!addStationPeer()) {
    Serial.println("Station peer registration failed. Verify STATION_MAC and restart after editing code.");
  }

  Serial.println("Bike heartbeat transmitter ready.");
  Serial.println("Close the GPIO25-to-GND switch to manually force an outside-boundary alert.");
}

void loop() {
  unsigned long now = millis();

  if (now - lastHeartbeatMs >= HEARTBEAT_INTERVAL_MS) {
    lastHeartbeatMs = now;
    sendHeartbeat();
  }

  processStationReply();
  monitorReplyTimeout();

  delay(5);
}
