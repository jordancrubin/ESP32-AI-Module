/*
  WiFiManager.cpp - ESP32 AI Module Network Controller
  Handles WiFi connectivity, mDNS resolution, and connection recovery.
  
  https://www.youtube.com/@retrotechandelectronics
  2026 Jordan Rubin.
*/
#include "WiFiManager.h"
#include <Arduino.h>
#include <ESPmDNS.h>

WiFiManager::WiFiManager(const char* ssid, const char* password) 
    : _ssid(ssid), _password(password), _cachedIP(IPAddress(0,0,0,0)) {}

void WiFiManager::setCredentials(String ssid, String password) {
    _ssid = ssid;
    _password = password;
}

void WiFiManager::connect() {
    if (_ssid == "" || _ssid == "YOUR_WIFI_SSID" ||
        _password == "" || _password == "YOUR_WIFI_PASSWORD") {
        Serial.println("WiFi Error: Missing or default credentials. Aborting connection.");
        return;
    }

    Serial.println("Connecting to WiFi...");
    
    MDNS.end(); // Clean up previous mDNS instance

    WiFi.disconnect(true); // Turn off WiFi to reset radio state
    delay(1500);
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.begin(_ssid, _password);

    unsigned long start = millis();
    // Timeout after 15 seconds
    while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
        delay(500);
        Serial.print(".");
    }
    
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("\nWiFi Connection Timeout.");
        return;
    }

    Serial.println("\nWiFi Connected!");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());

    if (!MDNS.begin("aiesp")) {
        Serial.println("Error setting up MDNS responder!");
    } else {
        MDNS.addService("http", "tcp", 80);
        Serial.println("mDNS responder started: http://aiesp.local");
    }
}

bool WiFiManager::isConnected() {
    return WiFi.status() == WL_CONNECTED;
}

String WiFiManager::resolveHost(String url) {
    if (url.indexOf(".local") != -1) {
        int start = 7; // Length of "http://"
        int end = url.indexOf(':', start);
        if (end == -1) end = url.indexOf('/', start);
        if (end == -1) end = url.length();

        String hostname = url.substring(start, end);
        if (hostname == _cachedHostname && _cachedIP != IPAddress(0,0,0,0)) {
            url.replace(hostname, _cachedIP.toString());
            return url;
        }

        String queryName = hostname;
        if (queryName.endsWith(".local")) {
            queryName = queryName.substring(0, queryName.length() - 6);
        }
        
        IPAddress ip;
        int retries = 3;
        while (retries > 0) {
            ip = MDNS.queryHost(queryName);
            if (ip != IPAddress(0,0,0,0)) break;
            retries--;
            delay(250);
        }
        
        if (ip != IPAddress(0,0,0,0)) {
            _cachedHostname = hostname;
            _cachedIP = ip;
            url.replace(hostname, ip.toString());
            Serial.println("Resolved " + hostname + " to " + ip.toString());
        } else {
            Serial.println("Error: Could not resolve mDNS hostname: " + hostname);
            return "";
        }
    }
    return url;
}

int WiFiManager::getSignalStrength() {
    return WiFi.RSSI();
}