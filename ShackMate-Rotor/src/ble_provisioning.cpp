#include "ble_provisioning.h"
#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include <Preferences.h>

// Use the global Preferences instance defined in main.cpp
extern Preferences wifiPrefs;

// Updated UUID definitions to match your iOS app
#define SERVICE_UUID     "d8e7a9c1-6c5c-4db4-9e12-9b6f5c35c00a"
#define SSID_CHAR_UUID   "d8e7a9c1-6c5c-4db4-9e12-9b6f5c35c001"
#define PASS_CHAR_UUID   "d8e7a9c1-6c5c-4db4-9e12-9b6f5c35c002"

// Global variables for BLE server and WiFi credentials
BLEServer *pServer = nullptr;
String receivedSSID = "";
String receivedPASS = "";

// Function to check if both credentials have been received and then save them
void checkCredentials() {
  if (receivedSSID.length() > 0 && receivedPASS.length() > 0) {
    wifiPrefs.putString("ssid", receivedSSID);
    wifiPrefs.putString("password", receivedPASS);
    Serial.println("Credentials saved. Restarting in 2 seconds...");
    // Commit the changes by ending the Preferences session.
    // (If the global instance is needed later you can re-open it on reboot.)
    wifiPrefs.end();
    delay(2000);
    ESP.restart();
  }
}

// Callback for the SSID characteristic
class ProvisioningSSIDCallback : public BLECharacteristicCallbacks {
public:
  void onWrite(BLECharacteristic *characteristic) override {
    std::string value = characteristic->getValue();
    receivedSSID = String(value.c_str());
    Serial.println("Received SSID: " + receivedSSID);
    checkCredentials();
  }
};

// Callback for the Password characteristic
class ProvisioningPASSCallback : public BLECharacteristicCallbacks {
public:
  void onWrite(BLECharacteristic *characteristic) override {
    std::string value = characteristic->getValue();
    receivedPASS = String(value.c_str());
    Serial.println("Received Password: " + receivedPASS);
    checkCredentials();
  }
};

void startBLEProvisioning() {
  // Assume wifiPrefs.begin("wifi", false) has been called in setup()
  BLEDevice::init("ShackMate-Rotor");
  pServer = BLEDevice::createServer();
  BLEService *service = pServer->createService(SERVICE_UUID);

  // Create the SSID characteristic and assign its callback
  BLECharacteristic *ssidCharacteristic = service->createCharacteristic(
      SSID_CHAR_UUID,
      BLECharacteristic::PROPERTY_WRITE
  );
  ssidCharacteristic->setCallbacks(new ProvisioningSSIDCallback());

  // Create the Password characteristic and assign its callback
  BLECharacteristic *passCharacteristic = service->createCharacteristic(
      PASS_CHAR_UUID,
      BLECharacteristic::PROPERTY_WRITE
  );
  passCharacteristic->setCallbacks(new ProvisioningPASSCallback());

  // Start the service and begin advertising the service UUID
  service->start();
  BLEAdvertising *advertising = BLEDevice::getAdvertising();
  advertising->addServiceUUID(SERVICE_UUID);
  advertising->start();

  Serial.println("[BLE] Provisioning Service Started. Waiting for credentials...");
}

void stopBLEServer() {
  if (pServer) {
    pServer->disconnect(0);
    Serial.println("[BLE] All BLE clients disconnected.");
  }
}

void stopBLEAdvertising() {
  BLEDevice::getAdvertising()->stop();
  stopBLEServer();
  Serial.println("[BLE] BLE Advertising and Server stopped.");
}