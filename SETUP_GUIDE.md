# Smartify Replacement ESP32 Firmware Setup

## Files

- `station_controller/station_controller.ino`: one ESP32 DEVKIT V1 controlling all nine station slots, Firebase, QR command consumption, and ESP-NOW RSSI receiving.
- `station_controller/secrets_example.h`: copy into an Arduino tab named `secrets.h`, then fill in credentials locally.
- `station_controller/WIRING.txt`: exact 9-sensor and 9-button wiring used by the sketch.
- `bike_geofence/bike_geofence.ino`: ESP32-DevKitC V4 bike node with RSSI response, LEDs, piezo buzzer, and optional manual boundary switch.
- `bike_geofence/WIRING.txt`: recommended bike wiring.

## Required Arduino IDE setup

1. Install Arduino IDE 2.x.
2. Install board package `esp32 by Espressif Systems`, current 3.x release.
3. Select the station board as `DOIT ESP32 DEVKIT V1` or `ESP32 Dev Module`.
4. Select the bike board as `ESP32 Dev Module` or `ESP32 DevKitC` if it is present in the board list.
5. Install library `Firebase Arduino Client Library for ESP8266 and ESP32` by Mobizt, which provides `Firebase_ESP_Client.h`.

## Before flashing the station

Uploading `station_controller.ino` replaces the running station program. The replacement code targets the database layout found in your export:

- `/stations/{stationId}/slots/{slotId}`
- `/alerts/{stationId}/{slotId}`
- `/switches/{stationId}/{slotId}`

The station treats `/switches/{stationId}/{slotId} = true` as a one-time QR/web authorization command. It accepts the command, writes `buttonAuthorized = true` under the matching slot, and changes the switch back to `false`.

The sketch clears stale authorizations and stale `/switches` commands at boot, but it intentionally does not clear existing alert records.

## Station Arduino sketch tabs

Arduino expects the main file and the secrets header to be tabs in the same sketch folder:

1. Open `station_controller.ino` in Arduino IDE.
2. Create a new tab named `secrets.h`.
3. Copy the contents of `secrets_example.h` into `secrets.h`.
4. Fill in your Wi-Fi and Firebase values.
5. Keep `secrets.h` private and out of public GitHub commits.

You need a Firebase Authentication Email/Password user for the station device unless your Realtime Database rules permit unauthenticated access. If Serial Monitor reports a Firebase permission error, check the RTDB Rules or provide the Rules screenshot for correction.

## Upload station controller

1. Connect the station ESP32 with a USB data cable.
2. Select its board and port in Arduino IDE.
3. Compile and upload `station_controller.ino`.
4. Open Serial Monitor at 115200 baud.
5. Copy the printed `Station ESP32 MAC` and printed Wi-Fi/ESP-NOW channel.
6. If box presence is inverted, change `BIKE_PRESENT_LEVEL` in the station sketch from `LOW` to `HIGH`, upload again, and retest.

## QR command test after station firmware upload

With a box present at Station 1 Slot 2, use Firebase Console to set:

```
/switches/station1/slot2 = true
```

Expected result within about one second:

```
/stations/station1/slots/slot2/buttonAuthorized = true
/stations/station1/slots/slot2/authorizationSource = "qr"
/switches/station1/slot2 = false
```

Remove the box within 30 seconds. The slot should record an authorized rental without raising an alert.

## Flash the bike ESP32

1. Wire the switch, LEDs, and piezo according to `bike_geofence/WIRING.txt`.
2. Open `bike_geofence.ino`.
3. Insert the station MAC address into `STATION_MAC`.
4. Change `ESPNOW_CHANNEL` to the channel printed by the station.
5. Select the bike ESP32-DevKitC V4 port and upload.
6. Open Serial Monitor at 115200 baud.

## RSSI calibration

The station sketch defaults to:

```
RSSI_OUTSIDE_THRESHOLD_DBM = -75
RSSI_INSIDE_THRESHOLD_DBM = -67
RSSI_CONFIRM_MS = 3000
```

Use these only as initial test values. In the demonstration space:

1. Keep the station board in its final position.
2. Place the bike close to it and observe `/bikes/bike1/smoothedRssi`.
3. Place the bike at the intended represented boundary and observe the RSSI.
4. Place it outside the represented boundary and observe the RSSI.
5. Adjust the inside and outside thresholds so there is a gap between them.

The switch on the bike is a reliable manual override: closing GPIO25 to GND immediately simulates an outside-boundary event even when radio readings fluctuate.

## Notes about your piezo buzzer

The supplied bike sketch assumes a small two-pin passive piezo element. It generates a tone using the Arduino-ESP32 3.x LEDC PWM API. For a louder buzzer or any device drawing substantial current, use a transistor driver rather than driving it directly from an ESP32 pin.
