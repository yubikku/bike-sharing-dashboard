/*
  Smartify Bike Sharing Prototype - Replacement Station Controller
  Board: ESP32 DEVKIT V1 / ESP32 Dev Module
  Arduino-ESP32 core: 3.x

  Responsibilities:
    - Reads 9 digital ITR9909 occupancy circuits for 3 stations x 3 slots.
    - Reads 9 physical authorization buttons.
    - Accepts QR/web authorizations from Firebase /switches/{station}/{slot} = true.
    - Updates the existing Firebase database structure used by index.html.
    - Detects authorized and unauthorized box/bike removal.
    - Receives bike heartbeat packets over ESP-NOW, uses RSSI as a proximity-based
      boundary simulation, and publishes /bikes/bike1 status to Firebase.

  IMPORTANT:
    - Uploading this sketch replaces any firmware already on the station ESP32.
    - Copy secrets_example.h into a new Arduino tab named secrets.h and fill it in.
    - Sensor polarity is configured by BIKE_PRESENT_LEVEL below. If your sensor
      reports the opposite result, change LOW to HIGH.
*/

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <Firebase_ESP_Client.h>
#include "addons/TokenHelper.h"
#include "secrets.h"

// -----------------------------------------------------------------------------
// Configuration
// -----------------------------------------------------------------------------

constexpr uint8_t SLOT_COUNT = 9;
constexpr uint8_t STATION_COUNT = 3;
constexpr uint8_t SLOTS_PER_STATION = 3;
constexpr uint8_t BIKE_PRESENT_LEVEL = LOW; // Change to HIGH if presence is inverted.

constexpr unsigned long SENSOR_DEBOUNCE_MS = 120;
constexpr unsigned long BUTTON_DEBOUNCE_MS = 50;
constexpr unsigned long AUTHORIZATION_WINDOW_MS = 30000;
constexpr unsigned long QR_POLL_INTERVAL_MS = 2500;
constexpr unsigned long CONNECTION_RETRY_INTERVAL_MS = 10000;

// RSSI geofence simulation values. Calibrate these in your actual demo venue.
constexpr int RSSI_OUTSIDE_THRESHOLD_DBM = -85; // weak signal: candidate outside
constexpr int RSSI_INSIDE_THRESHOLD_DBM = -77;  // strong signal: candidate inside
constexpr unsigned long RSSI_CONFIRM_MS = 3000;
constexpr unsigned long BIKE_HEARTBEAT_TIMEOUT_MS = 5000;
constexpr unsigned long BIKE_LINK_EXTENDED_LOSS_MS = 30000;
constexpr unsigned long BIKE_FIREBASE_UPDATE_INTERVAL_MS = 3000;
constexpr float RSSI_EMA_ALPHA = 0.25f;

constexpr uint8_t RSSI_WINDOW_SIZE = 7;

int8_t rssiWindow[RSSI_WINDOW_SIZE] = {0};
uint8_t rssiWindowCount = 0;
uint8_t rssiWindowIndex = 0;
int medianBikeRssi = -127;

constexpr char BIKE_ID[] = "bike1";

// Slot order matches your physical wiring list.
// station1: slot1, slot2, slot3; then station2; then station3.
const uint8_t SENSOR_PINS[SLOT_COUNT] = {
  27, 26, 25,  // Station 1 sensors: D27, D26, D25
  33, 32, 35,  // Station 2 sensors: D33, D32, D35
  34, 39, 36   // Station 3 sensors: D34, VN(GPIO39), VP(GPIO36)
};

const uint8_t BUTTON_PINS[SLOT_COUNT] = {
  13, 14, 16,  // Station 1 buttons: D13, D14, RX2(GPIO16)
  17, 18, 19,  // Station 2 buttons: TX2(GPIO17), D18, D19
  21, 22, 23   // Station 3 buttons: D21, D22, D23
};

const char *STATION_IDS[STATION_COUNT] = {"station1", "station2", "station3"};
const char *SLOT_IDS[SLOTS_PER_STATION] = {"slot1", "slot2", "slot3"};

// -----------------------------------------------------------------------------
// Firebase objects
// -----------------------------------------------------------------------------

FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig firebaseConfig;
bool firebaseStarted = false;

// -----------------------------------------------------------------------------
// Slot state
// -----------------------------------------------------------------------------

struct SlotState {
  bool present;
  bool sensorCandidate;
  unsigned long sensorCandidateSince;

  int lastButtonReading;
  int stableButtonReading;
  unsigned long buttonChangedAt;

  bool authorized;
  unsigned long authorizedUntil;
  String authorizationSource;
};

SlotState slots[SLOT_COUNT];
unsigned long lastQrPollMs = 0;
unsigned long lastWifiRetryMs = 0;

// -----------------------------------------------------------------------------
// ESP-NOW packet definitions shared with the bike sketch
// -----------------------------------------------------------------------------

enum MessageKind : uint8_t {
  MESSAGE_BIKE_HEARTBEAT = 1,
  MESSAGE_STATION_GEOFENCE_REPLY = 2
};

struct __attribute__((packed)) BikeHeartbeat {
  uint8_t kind;
  char bikeId[12];
  uint32_t sequence;
  uint8_t manualOutside; // 1 when the bike's backup boundary switch is activated.
};

struct __attribute__((packed)) StationGeofenceReply {
  uint8_t kind;
  char bikeId[12];
  int8_t lastRssi;
  int16_t smoothedRssi;
  uint8_t outsideBoundary;
  uint8_t connected;
};

portMUX_TYPE espNowMux = portMUX_INITIALIZER_UNLOCKED;
BikeHeartbeat pendingHeartbeat;
uint8_t pendingBikeMac[6] = {0};
int8_t pendingPacketRssi = -127;
volatile bool hasPendingHeartbeat = false;

bool espNowStarted = false;
bool bikePeerRegistered = false;
uint8_t bikePeerMac[6] = {0};
bool bikeEverSeen = false;
bool bikeConnected = false;
bool manualBoundaryOverride = false;
bool outsideBoundary = false;
bool boundaryStatePublished = false;
int lastBikeRssi = -127;
float smoothedBikeRssi = -127.0f;
bool hasSmoothedRssi = false;
unsigned long lastBikeSeenMs = 0;
unsigned long outsideCandidateSinceMs = 0;
unsigned long insideCandidateSinceMs = 0;
unsigned long lastBikeFirebaseUpdateMs = 0;
String geofenceReason = "waiting_for_bike";
bool extendedBikeLinkLossLogged = false;

// -----------------------------------------------------------------------------
// Utility paths and Firebase helpers
// -----------------------------------------------------------------------------

uint8_t stationIndexForSlot(uint8_t flatIndex) {
  return flatIndex / SLOTS_PER_STATION;
}

uint8_t localSlotIndex(uint8_t flatIndex) {
  return flatIndex % SLOTS_PER_STATION;
}

String slotBasePath(uint8_t flatIndex) {
  return String("/stations/") + STATION_IDS[stationIndexForSlot(flatIndex)] +
         "/slots/" + SLOT_IDS[localSlotIndex(flatIndex)];
}

String switchPath(uint8_t flatIndex) {
  return String("/switches/") + STATION_IDS[stationIndexForSlot(flatIndex)] +
         "/" + SLOT_IDS[localSlotIndex(flatIndex)];
}

String alertPath(uint8_t flatIndex) {
  return String("/alerts/") + STATION_IDS[stationIndexForSlot(flatIndex)] +
         "/" + SLOT_IDS[localSlotIndex(flatIndex)];
}

String readableSlotName(uint8_t flatIndex) {
  return String("Station ") + String(stationIndexForSlot(flatIndex) + 1) +
         " Slot " + String(localSlotIndex(flatIndex) + 1);
}

bool firebaseReadyNow() {
  return firebaseStarted && Firebase.ready();
}

void logFirebaseFailure(const String &action) {
  Serial.print("Firebase error while ");
  Serial.print(action);
  Serial.print(": ");
  Serial.println(fbdo.errorReason());
}

bool updateFirebaseNode(const String &path, FirebaseJson &json) {
  if (!firebaseReadyNow()) {
    return false;
  }
  if (!Firebase.RTDB.updateNode(&fbdo, path.c_str(), &json)) {
    logFirebaseFailure(String("updating ") + path);
    return false;
  }
  return true;
}

bool setFirebaseBool(const String &path, bool value) {
  if (!firebaseReadyNow()) {
    return false;
  }
  if (!Firebase.RTDB.setBool(&fbdo, path.c_str(), value)) {
    logFirebaseFailure(String("setting ") + path);
    return false;
  }
  return true;
}

// -----------------------------------------------------------------------------
// Slot behavior
// -----------------------------------------------------------------------------

bool readBikePresent(uint8_t flatIndex) {
  return digitalRead(SENSOR_PINS[flatIndex]) == BIKE_PRESENT_LEVEL;
}

void publishStationCounts(uint8_t stationIndex) {
  int bikeCount = 0;
  for (uint8_t local = 0; local < SLOTS_PER_STATION; local++) {
    uint8_t flat = stationIndex * SLOTS_PER_STATION + local;
    if (slots[flat].present) {
      bikeCount++;
    }
  }

  FirebaseJson patch;
  patch.set("bikeCount", bikeCount);
  patch.set("emptyCount", SLOTS_PER_STATION - bikeCount);
  patch.set("lastUpdated/.sv", "timestamp");
  updateFirebaseNode(String("/stations/") + STATION_IDS[stationIndex], patch);
}

void publishPresence(uint8_t flatIndex) {
  FirebaseJson patch;
  patch.set("bikePresent", slots[flatIndex].present);
  patch.set("lastUpdated/.sv", "timestamp");
  updateFirebaseNode(slotBasePath(flatIndex), patch);
  publishStationCounts(stationIndexForSlot(flatIndex));
}

void clearAuthorization(uint8_t flatIndex, bool publish = true) {
  slots[flatIndex].authorized = false;
  slots[flatIndex].authorizedUntil = 0;
  slots[flatIndex].authorizationSource = "";

  if (publish) {
    FirebaseJson patch;
    patch.set("buttonAuthorized", false);
    patch.set("authorizationSource", "");
    patch.set("authorizationExpiresInMs", 0);
    patch.set("lastUpdated/.sv", "timestamp");
    updateFirebaseNode(slotBasePath(flatIndex), patch);
  }
}

void writeLastEvent(uint8_t flatIndex, const String &eventText) {
  FirebaseJson patch;
  patch.set("lastEvent", eventText);
  patch.set("lastEventAt/.sv", "timestamp");
  updateFirebaseNode(slotBasePath(flatIndex), patch);
}

void authorizeSlot(uint8_t flatIndex, const char *source) {
  if (!slots[flatIndex].present) {
    Serial.printf("Authorization rejected for %s: slot is empty.\n", readableSlotName(flatIndex).c_str());
    writeLastEvent(flatIndex, String("authorization_rejected_empty_slot_") + source);
    return;
  }

  slots[flatIndex].authorized = true;
  slots[flatIndex].authorizedUntil = millis() + AUTHORIZATION_WINDOW_MS;
  slots[flatIndex].authorizationSource = source;

  Serial.printf("Authorized %s via %s for %lu ms.\n",
                readableSlotName(flatIndex).c_str(), source, AUTHORIZATION_WINDOW_MS);

  FirebaseJson patch;
  patch.set("buttonAuthorized", true); // Kept for compatibility with your existing dashboard.
  patch.set("authorizationSource", source);
  patch.set("authorizationExpiresInMs", static_cast<int>(AUTHORIZATION_WINDOW_MS));
  patch.set("lastEvent", String("authorized_via_") + source);
  patch.set("lastEventAt/.sv", "timestamp");
  updateFirebaseNode(slotBasePath(flatIndex), patch);
}

void raiseUnauthorizedAlert(uint8_t flatIndex) {
  String message = String("Unauthorized removal detected at ") + readableSlotName(flatIndex);
  Serial.println(message);

  FirebaseJson patch;
  patch.set("alertActive", true);
  patch.set("alertMessage", message);
  patch.set("lastEvent", "unauthorized_removal");
  patch.set("lastEventAt/.sv", "timestamp");
  updateFirebaseNode(slotBasePath(flatIndex), patch);
  setFirebaseBool(alertPath(flatIndex), true);
}

void processStablePresenceChange(uint8_t flatIndex, bool previousPresent) {
  publishPresence(flatIndex);

  if (previousPresent && !slots[flatIndex].present) {
    bool authorizationStillValid = slots[flatIndex].authorized &&
      static_cast<long>(slots[flatIndex].authorizedUntil - millis()) > 0;

    if (authorizationStillValid) {
      Serial.printf("Successful authorized rental: %s (%s).\n",
                    readableSlotName(flatIndex).c_str(),
                    slots[flatIndex].authorizationSource.c_str());
      FirebaseJson patch;
      patch.set("buttonAuthorized", false);
      patch.set("authorizationSource", "");
      patch.set("authorizationExpiresInMs", 0);
      patch.set("lastEvent", "authorized_rental");
      patch.set("lastEventAt/.sv", "timestamp");
      updateFirebaseNode(slotBasePath(flatIndex), patch);
      clearAuthorization(flatIndex, false);
    } else {
      clearAuthorization(flatIndex);
      raiseUnauthorizedAlert(flatIndex);
    }
  } else if (!previousPresent && slots[flatIndex].present) {
    Serial.printf("Bike/box docked at %s.\n", readableSlotName(flatIndex).c_str());
    clearAuthorization(flatIndex);
    writeLastEvent(flatIndex, "bike_docked");
    // Alerts intentionally remain until reset from the admin dashboard.
  }
}

void pollQrAuthorizationCommands() {
  if (!firebaseReadyNow() || millis() - lastQrPollMs < QR_POLL_INTERVAL_MS) {
    return;
  }
  lastQrPollMs = millis();

  for (uint8_t i = 0; i < SLOT_COUNT; i++) {
    bool request = false;
    if (Firebase.RTDB.getBool(&fbdo, switchPath(i).c_str(), &request)) {
      if (request) {
        authorizeSlot(i, "qr");
        // Consume a QR command once. The website can set it to true again for another request.
        setFirebaseBool(switchPath(i), false);
      }
    } else {
      logFirebaseFailure(String("reading QR command ") + switchPath(i));
    }
  }
}

void initializeFirebaseStationState() {
  Serial.println("Publishing current sensor presence and clearing stale authorizations...");
  for (uint8_t i = 0; i < SLOT_COUNT; i++) {
    publishPresence(i);
    clearAuthorization(i);
    setFirebaseBool(switchPath(i), false);
  }
  // Existing alertActive/alertMessage values are intentionally not cleared here.
}

// -----------------------------------------------------------------------------
// ESP-NOW RSSI-based boundary simulation
// -----------------------------------------------------------------------------

void onEspNowReceive(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len != static_cast<int>(sizeof(BikeHeartbeat)) || info == nullptr) {
    return;
  }

  BikeHeartbeat received;
  memcpy(&received, data, sizeof(received));
  if (received.kind != MESSAGE_BIKE_HEARTBEAT) {
    return;
  }

  int8_t rssi = -127;
  if (info->rx_ctrl != nullptr) {
    rssi = info->rx_ctrl->rssi;
  }

  portENTER_CRITICAL(&espNowMux);
  pendingHeartbeat = received;
  memcpy(pendingBikeMac, info->src_addr, 6);
  pendingPacketRssi = rssi;
  hasPendingHeartbeat = true;
  portEXIT_CRITICAL(&espNowMux);
}

bool ensureBikePeer(const uint8_t *mac) {
  if (bikePeerRegistered && memcmp(mac, bikePeerMac, 6) == 0) {
    return true;
  }

  if (esp_now_is_peer_exist(mac)) {
    memcpy(bikePeerMac, mac, 6);
    bikePeerRegistered = true;
    return true;
  }

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, mac, 6);
  peerInfo.channel = 0; // Use the station's current Wi-Fi channel.
  peerInfo.encrypt = false;
  peerInfo.ifidx = WIFI_IF_STA;

  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("Could not add bike as ESP-NOW peer.");
    return false;
  }

  memcpy(bikePeerMac, mac, 6);
  bikePeerRegistered = true;
  Serial.printf("Registered Bike ESP-NOW peer: %02X:%02X:%02X:%02X:%02X:%02X\n",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return true;
}

void publishBikeState() {
 if (!bikeEverSeen) {
    return;
  }

  FirebaseJson patch;

  patch.set("connected", bikeConnected);

  // Communication status is separated from boundary status.
  patch.set(
    "connectionState",
    bikeConnected ? "connected" : "stale_holding_last_state"
  );

  patch.set("stateHeldDueToLinkLoss", !bikeConnected);
  patch.set("lastRssi", lastBikeRssi);
  patch.set("smoothedRssi", static_cast<int>(roundf(smoothedBikeRssi)));

  // Link loss is not a measurement of location.
  // Preserve the last confirmed geofence decision.
  patch.set("geofenceState", outsideBoundary ? "outside" : "inside");
  patch.set("boundaryAlert", outsideBoundary);
  patch.set("buzzerActive", outsideBoundary);
  patch.set("decisionSource", geofenceReason);
  patch.set("lastUpdate/.sv", "timestamp");

  updateFirebaseNode(String("/bikes/") + BIKE_ID, patch);
  lastBikeFirebaseUpdateMs = millis();
}

void setBoundaryState(bool newOutside, const String &reason) {
  bool changed =
    !boundaryStatePublished ||
    outsideBoundary != newOutside ||
    geofenceReason != reason;

  outsideBoundary = newOutside;
  geofenceReason = reason;

  if (changed) {
    Serial.printf(
      "Bike geofence state: %s | reason=%s | RSSI=%d | smoothed=%d\n",
      outsideBoundary ? "OUTSIDE / ALERT" : "INSIDE",
      geofenceReason.c_str(),
      lastBikeRssi,
      static_cast<int>(roundf(smoothedBikeRssi))
    );

    boundaryStatePublished = true;

    // Do not publish to Firebase here.
    // The ESP-NOW reply must be sent first so an SSL stall cannot delay the bike update.
  }
}

void sendBoundaryReplyToBike() {
  if (!bikePeerRegistered) {
    return;
  }

  StationGeofenceReply reply = {};
  reply.kind = MESSAGE_STATION_GEOFENCE_REPLY;
  strncpy(reply.bikeId, BIKE_ID, sizeof(reply.bikeId) - 1);
  reply.lastRssi = static_cast<int8_t>(lastBikeRssi);
  reply.smoothedRssi = static_cast<int16_t>(roundf(smoothedBikeRssi));
  reply.outsideBoundary = outsideBoundary ? 1 : 0;
  reply.connected = bikeConnected ? 1 : 0;
  esp_now_send(bikePeerMac, reinterpret_cast<uint8_t *>(&reply), sizeof(reply));
}

void evaluateRssiBoundary(unsigned long now) {
   if (!bikeEverSeen) {
    return;
  }

  // A missing heartbeat means communication became stale.
  // It does not prove that the bike moved outside the boundary.
  if (now - lastBikeSeenMs > BIKE_HEARTBEAT_TIMEOUT_MS) {
    if (bikeConnected) {
      bikeConnected = false;
      geofenceReason = "heartbeat_timeout_holding_last_state";

      Serial.printf(
        "Bike heartbeat unavailable for more than %lu seconds. "
        "Holding last confirmed geofence state: %s. "
        "No boundary alert generated by connection loss.\n",
        BIKE_HEARTBEAT_TIMEOUT_MS / 1000,
        outsideBoundary ? "OUTSIDE / ALERT" : "INSIDE"
      );

      publishBikeState();
    }

    if (!extendedBikeLinkLossLogged &&
        now - lastBikeSeenMs > BIKE_LINK_EXTENDED_LOSS_MS) {
      extendedBikeLinkLossLogged = true;

      Serial.printf(
        "Bike heartbeat unavailable for more than %lu seconds. "
        "Still holding last confirmed state until communication recovers.\n",
        BIKE_LINK_EXTENDED_LOSS_MS / 1000
      );
    }

    return;
  }

  if (manualBoundaryOverride) {
    outsideCandidateSinceMs = 0;
    insideCandidateSinceMs = 0;
    setBoundaryState(true, "manual_boundary_switch");
    return;
  }

  int rssi = static_cast<int>(roundf(smoothedBikeRssi));

  if (rssi <= RSSI_OUTSIDE_THRESHOLD_DBM) {
    insideCandidateSinceMs = 0;

    if (outsideCandidateSinceMs == 0) {
      outsideCandidateSinceMs = now;
    }

    if (now - outsideCandidateSinceMs >= RSSI_CONFIRM_MS) {
      setBoundaryState(true, "rssi_threshold");
    }

  } else if (rssi >= RSSI_INSIDE_THRESHOLD_DBM) {
    outsideCandidateSinceMs = 0;

    if (insideCandidateSinceMs == 0) {
      insideCandidateSinceMs = now;
    }

    if (now - insideCandidateSinceMs >= RSSI_CONFIRM_MS) {
      setBoundaryState(false, "rssi_threshold");
    }

  } else {
    // Hysteresis band: retain the previous confirmed boundary state.
    outsideCandidateSinceMs = 0;
    insideCandidateSinceMs = 0;
  }
}

int addRssiSampleAndGetMedian(int8_t newRssi) {
  rssiWindow[rssiWindowIndex] = newRssi;
  rssiWindowIndex = (rssiWindowIndex + 1) % RSSI_WINDOW_SIZE;

  if (rssiWindowCount < RSSI_WINDOW_SIZE) {
    rssiWindowCount++;
  }

  int values[RSSI_WINDOW_SIZE];

  for (uint8_t i = 0; i < rssiWindowCount; i++) {
    values[i] = rssiWindow[i];
  }

  for (uint8_t i = 0; i < rssiWindowCount - 1; i++) {
    for (uint8_t j = i + 1; j < rssiWindowCount; j++) {
      if (values[j] < values[i]) {
        int temp = values[i];
        values[i] = values[j];
        values[j] = temp;
      }
    }
  }

  return values[rssiWindowCount / 2];
}

void processPendingBikeHeartbeat() {
  BikeHeartbeat heartbeat;
  uint8_t sourceMac[6];
  int8_t packetRssi;
  bool available = false;

  portENTER_CRITICAL(&espNowMux);
  if (hasPendingHeartbeat) {
    heartbeat = pendingHeartbeat;
    memcpy(sourceMac, pendingBikeMac, 6);
    packetRssi = pendingPacketRssi;
    hasPendingHeartbeat = false;
    available = true;
  }
  portEXIT_CRITICAL(&espNowMux);

  if (!available || String(heartbeat.bikeId) != BIKE_ID) {
    return;
  }

  ensureBikePeer(sourceMac);
  
  bool recoveredFromLinkLoss = bikeEverSeen && !bikeConnected;
  
  bikeEverSeen = true;
  bikeConnected = true;
  extendedBikeLinkLossLogged = false;
  
  if (recoveredFromLinkLoss) {
    geofenceReason = "heartbeat_recovered";
    Serial.println("Bike heartbeat recovered. Resuming RSSI boundary evaluation.");
  }
  
  manualBoundaryOverride = heartbeat.manualOutside != 0;
  lastBikeRssi = packetRssi;
  lastBikeSeenMs = millis();
  
  medianBikeRssi = addRssiSampleAndGetMedian(packetRssi);
  
  if (!hasSmoothedRssi) {
    smoothedBikeRssi = medianBikeRssi;
    hasSmoothedRssi = true;
  } else {
    smoothedBikeRssi =
      RSSI_EMA_ALPHA * medianBikeRssi +
      (1.0f - RSSI_EMA_ALPHA) * smoothedBikeRssi;
  }
  
  Serial.printf(
    "[RSSI] raw=%d dBm | median=%d dBm | smoothed=%.1f dBm | outside<=%d | inside>=%d\n",
    lastBikeRssi,
    medianBikeRssi,
    smoothedBikeRssi,
    RSSI_OUTSIDE_THRESHOLD_DBM,
    RSSI_INSIDE_THRESHOLD_DBM
  );

  if (!boundaryStatePublished) {
    setBoundaryState(false, "first_connection");
  }

  // Compute the newest RSSI/manual-switch result before replying.
  evaluateRssiBoundary(millis());

  // Reply to the bike before any Firebase operations occur.
  sendBoundaryReplyToBike();


}

bool startEspNow() {
  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW initialization failed.");
    return false;
  }
  if (esp_now_register_recv_cb(onEspNowReceive) != ESP_OK) {
    Serial.println("ESP-NOW receive callback registration failed.");
    return false;
  }
  espNowStarted = true;
  Serial.println("ESP-NOW receiver started for Bike 1 RSSI geofence simulation.");
  return true;
}

// -----------------------------------------------------------------------------
// Network and setup
// -----------------------------------------------------------------------------

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to Wi-Fi");
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 25000) {
    delay(400);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Wi-Fi connected. IP: ");
    Serial.println(WiFi.localIP());
    Serial.print("Station ESP32 MAC: ");
    Serial.println(WiFi.macAddress());
    Serial.print("Wi-Fi / ESP-NOW channel to use on the bike ESP32: ");
    Serial.println(WiFi.channel());
  } else {
    Serial.println("Wi-Fi connection failed. Inputs still work locally; Firebase/ESP-NOW will start after reconnection.");
  }
}

void startFirebase() {
  firebaseConfig.api_key = FIREBASE_API_KEY;
  firebaseConfig.database_url = FIREBASE_DATABASE_URL;
  firebaseConfig.token_status_callback = tokenStatusCallback;
  auth.user.email = FIREBASE_USER_EMAIL;
  auth.user.password = FIREBASE_USER_PASSWORD;

  Firebase.reconnectWiFi(true);
  Firebase.begin(&firebaseConfig, &auth);
  firebaseStarted = true;
  Serial.println("Firebase client started. Waiting for authenticated readiness in loop.");
}

void initializeHardware() {
  for (uint8_t i = 0; i < SLOT_COUNT; i++) {
    // Pins 34, 35, 36 and 39 are input-only and require your existing external pull-up circuit.
    pinMode(SENSOR_PINS[i], INPUT);
    pinMode(BUTTON_PINS[i], INPUT_PULLUP);

    slots[i].present = readBikePresent(i);
    slots[i].sensorCandidate = slots[i].present;
    slots[i].sensorCandidateSince = millis();
    slots[i].lastButtonReading = digitalRead(BUTTON_PINS[i]);
    slots[i].stableButtonReading = slots[i].lastButtonReading;
    slots[i].buttonChangedAt = millis();
    slots[i].authorized = false;
    slots[i].authorizedUntil = 0;
    slots[i].authorizationSource = "";

    Serial.printf("Initial %-18s sensor GPIO=%u present=%s | button GPIO=%u\n",
                  readableSlotName(i).c_str(), SENSOR_PINS[i],
                  slots[i].present ? "YES" : "NO", BUTTON_PINS[i]);
  }
}

void processSensorInputs() {
  unsigned long now = millis();
  for (uint8_t i = 0; i < SLOT_COUNT; i++) {
    bool reading = readBikePresent(i);
    if (reading != slots[i].sensorCandidate) {
      slots[i].sensorCandidate = reading;
      slots[i].sensorCandidateSince = now;
    }

    if (slots[i].sensorCandidate != slots[i].present &&
        now - slots[i].sensorCandidateSince >= SENSOR_DEBOUNCE_MS) {
      bool previous = slots[i].present;
      slots[i].present = slots[i].sensorCandidate;
      Serial.printf("Presence change: %s -> %s\n", readableSlotName(i).c_str(),
                    slots[i].present ? "BIKE PRESENT" : "EMPTY");
      processStablePresenceChange(i, previous);
    }
  }
}

void processButtonInputs() {
  unsigned long now = millis();
  for (uint8_t i = 0; i < SLOT_COUNT; i++) {
    int reading = digitalRead(BUTTON_PINS[i]);
    if (reading != slots[i].lastButtonReading) {
      slots[i].lastButtonReading = reading;
      slots[i].buttonChangedAt = now;
    }

    if (reading != slots[i].stableButtonReading &&
        now - slots[i].buttonChangedAt >= BUTTON_DEBOUNCE_MS) {
      slots[i].stableButtonReading = reading;
      if (reading == LOW) {
        authorizeSlot(i, "button");
      }
    }
  }
}

void expireAuthorizations() {
  unsigned long now = millis();
  for (uint8_t i = 0; i < SLOT_COUNT; i++) {
    if (slots[i].authorized && static_cast<long>(now - slots[i].authorizedUntil) >= 0) {
      Serial.printf("Authorization expired: %s\n", readableSlotName(i).c_str());
      clearAuthorization(i);
      writeLastEvent(i, "authorization_expired");
    }
  }
}

bool initialFirebasePublished = false;

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println();
  Serial.println("============================================================");
  Serial.println("Smartify Replacement Station Controller - 9 Slots + ESP-NOW");
  Serial.println("============================================================");

  initializeHardware();
  connectWiFi();

  if (WiFi.status() == WL_CONNECTED) {
    startFirebase();
    startEspNow();
  }
}

void loop() {
  processSensorInputs();
  processButtonInputs();
  expireAuthorizations();

  if (WiFi.status() != WL_CONNECTED) {
    if (millis() - lastWifiRetryMs >= CONNECTION_RETRY_INTERVAL_MS) {
      lastWifiRetryMs = millis();
      Serial.println("Retrying Wi-Fi connection...");
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    }
    delay(5);
    return;
  }

  if (!firebaseStarted) {
    startFirebase();
  }
  if (!espNowStarted) {
    startEspNow();
    Serial.print("Wi-Fi / ESP-NOW channel to use on bike: ");
    Serial.println(WiFi.channel());
  }

  if (firebaseReadyNow() && !initialFirebasePublished) {
    initializeFirebaseStationState();
    initialFirebasePublished = true;
  }

  // Handle radio data first. If a Firebase SSL request blocks temporarily,
  // the newest pending heartbeat is answered before beginning another Firebase request.
  processPendingBikeHeartbeat();
  evaluateRssiBoundary(millis());
  
  // Firebase work is intentionally performed after ESP-NOW processing.
  pollQrAuthorizationCommands();
  
  if (bikeEverSeen && firebaseReadyNow() &&
      millis() - lastBikeFirebaseUpdateMs >= BIKE_FIREBASE_UPDATE_INTERVAL_MS) {
    publishBikeState();
  }

  delay(5);
}
