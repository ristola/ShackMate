#ifndef BLE_PROVISIONING_H
#define BLE_PROVISIONING_H

// Starts the BLE provisioning service (advertising as "ShackMate-Rotor")
void startBLEProvisioning();

// Stops the BLE server (disconnects any clients)
void stopBLEServer();

// Stops BLE advertising (and disconnects clients)
void stopBLEAdvertising();

#endif // BLE_PROVISIONING_H