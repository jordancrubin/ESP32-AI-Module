#pragma once
#include <WiFi.h>
#include <ESPmDNS.h>

class WiFiManager {
public:
    WiFiManager(const char* ssid, const char* password);
    void setCredentials(String ssid, String password);
    void connect();
    bool isConnected();
    String resolveHost(String url);
    int getSignalStrength();
    
private:
    String _ssid;
    String _password;
    String _cachedHostname;
    IPAddress _cachedIP;
};