#include <WiFi.h>
#include <WiFiManager.h>
#include "GlobalVariables.h"
#include "WiFiConfig.h"

// Initialize the global variable

bool connectWiFi() {
  WiFiManager wifiManager;
  // Optional: force configuration mode by resetting stored credentials:
  // wifiManager.resetSettings();
  
  // Optionally, set a timeout for the configuration portal:
  // wifiManager.setConfigPortalTimeout(180);

  // Try to auto-connect; if credentials are not available, an AP will be started.
  if (!wifiManager.autoConnect("ESP32 ICOM Gateway")) {
    Serial.println("Failed to connect to WiFi.");
    return false;
  }
  
  // WiFi connected.
  IPAddress ip = WiFi.localIP();
  deviceIP = ip.toString();
  Serial.print("Connected, IP address: ");
  Serial.println(deviceIP);
  return true;
}