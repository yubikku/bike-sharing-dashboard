#pragma once

// Copy this file into a new Arduino IDE tab named secrets.h.
// Fill in the values from your Wi-Fi hotspot/router and Firebase project.
// Do not commit your completed secrets.h to a public GitHub repository.

#define WIFI_SSID "YOUR_2_4_GHZ_WIFI_NAME"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"

// Firebase Project Settings > General > Web API Key
#define FIREBASE_API_KEY "YOUR_FIREBASE_WEB_API_KEY"

// Realtime Database URL, without a trailing slash.
// Example: https://your-project-default-rtdb.asia-southeast1.firebasedatabase.app
#define FIREBASE_DATABASE_URL "YOUR_FIREBASE_REALTIME_DATABASE_URL"

// Create a dedicated Firebase Authentication Email/Password account for the station ESP32.
// Your Realtime Database Rules must permit this authenticated account to read/write required paths.
#define FIREBASE_USER_EMAIL "station-device@example.com"
#define FIREBASE_USER_PASSWORD "YOUR_DEVICE_ACCOUNT_PASSWORD"
