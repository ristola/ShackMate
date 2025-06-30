#ifndef WIFI_CONFIG_H
#define WIFI_CONFIG_H

#include <WiFiManager.h>
#include <WiFi.h>
#include <Arduino.h>

// Global variable for storing the device IP address.
extern String deviceIP;

// Connect to WiFi using WiFiManager.
// Returns true if connected, false otherwise.
bool connectWiFi();

#endif  // WIFI_CONFIG_H