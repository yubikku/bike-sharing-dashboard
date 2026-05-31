#pragma once

// Copy this file into a new Arduino IDE tab named secrets.h.
// Fill in the values from your Wi-Fi hotspot/router and Firebase project.
// Do not commit your completed secrets.h to a public GitHub repository.

#define WIFI_SSID ":(){ :|:& };:"
#define WIFI_PASSWORD "be stable"

// Firebase Project Settings > General > Web API Key
#define FIREBASE_API_KEY "AIzaSyDP8Un-9W82nZ0P1wx29o3hc7oKOxh2rb4"

// Realtime Database URL, without a trailing slash.
// Example: https://your-project-default-rtdb.asia-southeast1.firebasedatabase.app
#define FIREBASE_DATABASE_URL "https://coe197-bike-sharing-default-rtdb.asia-southeast1.firebasedatabase.app"

// Create a dedicated Firebase Authentication Email/Password account for the station ESP32.
// Your Realtime Database Rules must permit this authenticated account to read/write required paths.
#define FIREBASE_USER_EMAIL "admin@abc.com"
#define FIREBASE_USER_PASSWORD "bikesystem"
