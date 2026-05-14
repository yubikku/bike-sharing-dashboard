#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include "addons/TokenHelper.h"
#include "addons/RTDBHelper.h"

// --------------------------- Wi-Fi and Firebase configuration
#define WIFI_SSID "Cenacle Residences"
#define WIFI_PASSWORD "Cenacle55555"
#define API_KEY "AIzaSyDP8Un-9W82nZ0P1wx29o3hc7oKOxh2rb4"
#define DATABASE_URL "https://coe197-bike-sharing-default-rtdb.asia-southeast1.firebasedatabase.app/"

// --------------------------- ESP32 pin assignments
#define LED1_PIN 12
#define LED2_PIN 14
#define SWITCH1_PIN 18
#define SWITCH2_PIN 19
#define SWITCH3_PIN 21
#define SLOT1_TRIG_PIN 25
#define SLOT1_ECHO_PIN 35
#define SLOT2_TRIG_PIN 26
#define SLOT2_ECHO_PIN 32
#define SLOT3_TRIG_PIN 27
#define SLOT3_ECHO_PIN 33
#define PWMChannel 0

// --------------------------- Timing, PWM, and sensor conversion constants
const int freq = 5000;
const int resolution = 8;
const unsigned long debounce_delay = 50;
const unsigned long led_read_interval = 500;
const unsigned long ultrasonic_read_interval = 2000;
const unsigned long sensor_gap_millis = 100;
const unsigned long echo_timeout_micros = 30000;
const float sound_speed_cm_per_microsecond = 0.0343;
const float centimeters_to_Inch = 2.54;
const float bike_present_threshold_cm = 10.0;

// --------------------------- Firebase objects
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

// --------------------------- LED and Firebase authentication states
bool signupOK = false;
int pwmValue = 0;
bool ledStatus = false;

// --------------------------- Button authorization states
bool switch1State = false;
bool switch2State = false;
bool switch3State = false;
bool switch1Locked = false;
bool switch2Locked = false;
bool switch3Locked = false;

// --------------------------- Last known HC-SR04 bike presence states
bool lastSlot1BikePresent = false;
bool lastSlot2BikePresent = false;
bool lastSlot3BikePresent = false;
bool slot1PresenceInitialized = false;
bool slot2PresenceInitialized = false;
bool slot3PresenceInitialized = false;

// --------------------------- Interrupt flags for button presses
volatile bool switch1Triggered = false;
volatile bool switch2Triggered = false;
volatile bool switch3Triggered = false;

// --------------------------- millis() timing states
unsigned long lastSwitch1DebounceTime = 0;
unsigned long lastSwitch2DebounceTime = 0;
unsigned long lastSwitch3DebounceTime = 0;
unsigned long switch1ReleaseStartTime = 0;
unsigned long switch2ReleaseStartTime = 0;
unsigned long switch3ReleaseStartTime = 0;
unsigned long lastLedReadMillis = 0;
unsigned long lastUltrasonicReadMillis = 0;

// --------------------------- Interrupt service routines
void IRAM_ATTR switch1ISR() {
  switch1Triggered = true;
}

void IRAM_ATTR switch2ISR() {
  switch2Triggered = true;
}

void IRAM_ATTR switch3ISR() {
  switch3Triggered = true;
}

// =============
// Arduino Setup
// =============
void setup() {
  // put your setup code here, to run once:
  // --------------------------- Configure LED, button, and HC-SR04 pins
  pinMode(LED2_PIN, OUTPUT);
  pinMode(SWITCH1_PIN, INPUT_PULLUP);
  pinMode(SWITCH2_PIN, INPUT_PULLUP);
  pinMode(SWITCH3_PIN, INPUT_PULLUP);
  pinMode(SLOT1_TRIG_PIN, OUTPUT);
  pinMode(SLOT2_TRIG_PIN, OUTPUT);
  pinMode(SLOT3_TRIG_PIN, OUTPUT);
  pinMode(SLOT1_ECHO_PIN, INPUT);
  pinMode(SLOT2_ECHO_PIN, INPUT);
  pinMode(SLOT3_ECHO_PIN, INPUT);

  digitalWrite(SLOT1_TRIG_PIN, LOW);
  digitalWrite(SLOT2_TRIG_PIN, LOW);
  digitalWrite(SLOT3_TRIG_PIN, LOW);

  // --------------------------- Attach button interrupts
  attachInterrupt(digitalPinToInterrupt(SWITCH1_PIN), switch1ISR, FALLING);
  attachInterrupt(digitalPinToInterrupt(SWITCH2_PIN), switch2ISR, FALLING);
  attachInterrupt(digitalPinToInterrupt(SWITCH3_PIN), switch3ISR, FALLING);

  // --------------------------- Configure LED1 PWM
  ledcAttach(LED1_PIN, freq, resolution);
  ledcWrite(LED1_PIN, 0);

  // --------------------------- Start Serial Monitor
  Serial.begin(115200);
  Serial.println("Iteration 5: Slot authorization + HC-SR04 alerts + Firebase");
  Serial.println("Bike present threshold: distance > 0 cm and distance < 10 cm");

  // --------------------------- Connect ESP32 to Wi-Fi
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to Wi-Fi");
  while(WiFi.status() != WL_CONNECTED) {
    Serial.print("."); delay(500);
  }
  Serial.println();
  Serial.print("Connected with IP: ");
  Serial.println(WiFi.localIP());
  Serial.println();

  // --------------------------- Configure and start Firebase
  config.api_key = API_KEY;
  config.database_url = DATABASE_URL;
  auth.user.email = "device@abc.com";
  auth.user.password = "mainesp32";
  signupOK = true;
  Serial.println("Firebase device account configured");

  config.token_status_callback = tokenStatusCallback;
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);
} // end of setup

// =============
// Arduino Loop
// =============
void loop() {
  // put your main code here, to run repeatedly:
  // --------------------------- READ slot buttons and STORE authorization to RTDB
  processSlotButton(SWITCH1_PIN, "/switches/switch1", "/slots/slot1/buttonAuthorized", switch1Triggered, switch1State, switch1Locked, lastSwitch1DebounceTime, switch1ReleaseStartTime);
  processSlotButton(SWITCH2_PIN, "/switches/switch2", "/slots/slot2/buttonAuthorized", switch2Triggered, switch2State, switch2Locked, lastSwitch2DebounceTime, switch2ReleaseStartTime);
  processSlotButton(SWITCH3_PIN, "/switches/switch3", "/slots/slot3/buttonAuthorized", switch3Triggered, switch3State, switch3Locked, lastSwitch3DebounceTime, switch3ReleaseStartTime);

  // --------------------------- READ HC-SR04 sensors and process slot events
  if (Firebase.ready() && signupOK && (millis() - lastUltrasonicReadMillis > ultrasonic_read_interval || lastUltrasonicReadMillis == 0)) {
    lastUltrasonicReadMillis = millis();

    readAndProcessSlot(1, SLOT1_TRIG_PIN, SLOT1_ECHO_PIN, "/slots/slot1/bikePresent", "/switches/switch1", "/slots/slot1/buttonAuthorized", "/slots/slot1/alertActive", "/slots/slot1/alertMessage", "/alerts/slot1", lastSlot1BikePresent, slot1PresenceInitialized, switch1State);
    delay(sensor_gap_millis);

    readAndProcessSlot(2, SLOT2_TRIG_PIN, SLOT2_ECHO_PIN, "/slots/slot2/bikePresent", "/switches/switch2", "/slots/slot2/buttonAuthorized", "/slots/slot2/alertActive", "/slots/slot2/alertMessage", "/alerts/slot2", lastSlot2BikePresent, slot2PresenceInitialized, switch2State);
    delay(sensor_gap_millis);

    readAndProcessSlot(3, SLOT3_TRIG_PIN, SLOT3_ECHO_PIN, "/slots/slot3/bikePresent", "/switches/switch3", "/slots/slot3/buttonAuthorized", "/slots/slot3/alertActive", "/slots/slot3/alertMessage", "/alerts/slot3", lastSlot3BikePresent, slot3PresenceInitialized, switch3State);
  }

  // --------------------------- READ data from a RTDB to control devices attached to the ESP32
  if (Firebase.ready() && signupOK && (millis() - lastLedReadMillis > led_read_interval || lastLedReadMillis == 0)) {
    lastLedReadMillis = millis();

    // start get_led 1
    if (Firebase.RTDB.getInt(&fbdo, "/LED/analog")){
      if (fbdo.dataType() == "int") {
        pwmValue = fbdo.intData();
        // Serial.println(" Successful READ from " + fbdo.dataPath() + ": " + pwmValue + " (" + fbdo.dataType() + ") ");
        ledcWrite(LED1_PIN, pwmValue);
      }
    } else {
        Serial.println("FAILED: " + fbdo.errorReason());
    }// end get_led 1

    // start get_led 2
    if (Firebase.RTDB.getBool(&fbdo, "/LED/digital")){
      if (fbdo.dataType() == "boolean") {
        ledStatus = fbdo.boolData();
        // Serial.println(" Successful READ from " + fbdo.dataPath() + ": " + ledStatus + " (" + fbdo.dataType() + ") ");
        digitalWrite(LED2_PIN, ledStatus);
      }
    } else {
        Serial.println("FAILED: " + fbdo.errorReason());
    }
  }// end get_led 2

} // end of loop

// ================
// HELPER functions
// ================
// --------------------------- Process one slot button and write authorization to RTDB
void processSlotButton(int pin, const char *switch_path, const char *authorized_path, volatile bool &triggered, bool &authorized, bool &locked, unsigned long &lastDebounceTime, unsigned long &releaseStartTime) {
  bool shouldProcess = false;
  int currentReading = digitalRead(pin);
  unsigned long currentMillis = millis();

  // --------------------------- Unlock only after stable button release
  if (locked) {
    if (currentReading == HIGH) {
      if (releaseStartTime == 0) {
        releaseStartTime = currentMillis;
      } else if ((currentMillis - releaseStartTime) > debounce_delay) {
        locked = false;
        releaseStartTime = 0;
      }
    } else {
      releaseStartTime = 0;
    }
  }

  // --------------------------- Safely copy and clear interrupt flag
  noInterrupts();
  if (triggered) {
    triggered = false;
    shouldProcess = true;
  }
  interrupts();

  if (!shouldProcess) {
    return;
  }

  if ((currentMillis - lastDebounceTime) <= debounce_delay) {
    return;
  }

  // --------------------------- Accept one valid authorization press only
  if (currentReading == LOW && !locked) {
    lastDebounceTime = currentMillis;
    releaseStartTime = 0;
    locked = true;
    authorized = true;

    if (Firebase.ready() && signupOK) {
      writeBool(switch_path, true);
      writeBool(authorized_path, true);
    }
  }
}

// --------------------------- Read one slot and process normal or unauthorized removal
void readAndProcessSlot(int slot_number, int trig_pin, int echo_pin, const char *bike_present_path, const char *switch_path, const char *authorized_path, const char *alert_active_path, const char *alert_message_path, const char *alert_path, bool &last_bike_present, bool &presence_initialized, bool &authorized) {
  float distance_cm = readDistanceCm(trig_pin, echo_pin);
  bool bike_present = distance_cm > 0 && distance_cm < bike_present_threshold_cm;

  printSlotReading(slot_number, distance_cm, bike_present);

  if (presence_initialized && bike_present == last_bike_present) {
    return;
  }

  writeBool(bike_present_path, bike_present);

  // --------------------------- First sensor state sync
  if (!presence_initialized) {
    last_bike_present = bike_present;
    presence_initialized = true;

    if (bike_present) {
      clearSlotAuthorization(switch_path, authorized_path, authorized);
      clearSlotAlert(alert_active_path, alert_message_path, alert_path);
    }
    return;
  }

  // --------------------------- Bike returned to slot
  if (bike_present && !last_bike_present) {
    clearSlotAuthorization(switch_path, authorized_path, authorized);
    clearSlotAlert(alert_active_path, alert_message_path, alert_path);
  }

  // --------------------------- Bike removed from slot
  if (!bike_present && last_bike_present) {
    if (authorized) {
      clearSlotAuthorization(switch_path, authorized_path, authorized);
      clearSlotAlert(alert_active_path, alert_message_path, alert_path);
    } else {
      raiseSlotAlert(slot_number, alert_active_path, alert_message_path, alert_path);
    }
  }

  last_bike_present = bike_present;
}

// --------------------------- Clear one slot authorization
void clearSlotAuthorization(const char *switch_path, const char *authorized_path, bool &authorized) {
  authorized = false;
  writeBool(switch_path, false);
  writeBool(authorized_path, false);
}

// --------------------------- Clear one slot alert
void clearSlotAlert(const char *alert_active_path, const char *alert_message_path, const char *alert_path) {
  writeBool(alert_active_path, false);
  writeString(alert_message_path, "No alert");
  writeBool(alert_path, false);
}

// --------------------------- Raise one slot alert
void raiseSlotAlert(int slot_number, const char *alert_active_path, const char *alert_message_path, const char *alert_path) {
  char message[50];
  snprintf(message, sizeof(message), "Unauthorized removal detected at Slot %d", slot_number);

  writeBool(alert_active_path, true);
  writeString(alert_message_path, message);
  writeBool(alert_path, true);
}

// --------------------------- Write boolean value to RTDB
bool writeBool(const char *path, bool value) {
  if (Firebase.RTDB.setBool(&fbdo, path, value)) {
    return true;
  }

  Serial.println("FAILED: " + fbdo.errorReason());
  return false;
}

// --------------------------- Write string value to RTDB
bool writeString(const char *path, const char *value) {
  if (Firebase.RTDB.setString(&fbdo, path, value)) {
    return true;
  }

  Serial.println("FAILED: " + fbdo.errorReason());
  return false;
}

// --------------------------- Measure HC-SR04 distance in centimeters
float readDistanceCm(int trig_pin, int echo_pin) {
  digitalWrite(trig_pin, LOW);
  delayMicroseconds(2);
  digitalWrite(trig_pin, HIGH);
  delayMicroseconds(10);
  digitalWrite(trig_pin, LOW);

  unsigned long duration = pulseIn(echo_pin, HIGH, echo_timeout_micros);

  if (duration == 0) {
    return -1.0;
  }

  return (duration * sound_speed_cm_per_microsecond) / 2.0;
}

// --------------------------- Print sensor reading for Serial confirmation
void printSlotReading(int slot_number, float distance_cm, bool bike_present) {
  Serial.print("Slot ");
  Serial.print(slot_number);
  Serial.print(": ");

  if (distance_cm < 0) {
    Serial.print("No echo");
  } else {
    float distanceInches = distance_cm / centimeters_to_Inch;
    Serial.print(distance_cm, 1);
    Serial.print(" cm / ");
    Serial.print(distanceInches, 1);
    Serial.print(" in");
  }

  Serial.print(" | Bike present: ");
  Serial.println(bike_present ? "YES" : "NO");
}
