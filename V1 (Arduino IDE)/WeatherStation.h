#ifndef WEATHERSTATION_H
#define WEATHERSTATION_H

#include <WifiLocation.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "ST7796U.h"

class WeatherStation {
public:
    WeatherStation(const String& weatherApiKey);
    
    void begin(const char* ssid, const char* password);
    void updateWeather();           // Fetch weather periodically
    void fetchWeather();

private:
    const char* _ssid;
    const char* _password;
    String _weatherApiKey;
    WifiLocation _location;
    String _latlon = "LAT, LON";  // Default Home

    unsigned long _weatherTimer = 0;
    const unsigned long _weatherInterval = 1800000;  // 30 mintues

    String httpGETRequest(const char* serverName);
};

#endif

