#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>

// Unique ID for your bike. You can generate a random UUID online if you make more bikes.
#define BIKE_UUID "12345678-1234-1234-1234-123456789abc"

void setup() {
  Serial.begin(115200);
  Serial.println("Starting BLE Beacon for Bike...");

  // Initialize BLE
  BLEDevice::init("SmartBike_01");
  BLEServer *pServer = BLEDevice::createServer();
  
  // Set up advertising
  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(BIKE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x06);  // Functions that help with iPhone connections issue
  pAdvertising->setMinPreferred(0x12);
  
  BLEDevice::startAdvertising();
  Serial.println("Bike is now broadcasting!");
}

void loop() {
  // The BLE broadcast runs in the background. 
  // You can put battery monitoring or other bike logic here.
  delay(2000);
}