#include "WeatherStation.h"

ST7796U weather_display(LCD_CS, LCD_DC, LCD_RST, LCD_BL, LCD_SCK, LCD_MOSI);
unsigned long numUpdates;

WeatherStation::WeatherStation(const String& weatherApiKey)
    : _weatherApiKey(weatherApiKey) {}

void WeatherStation::updateWeather() {
    unsigned long currentMillis = millis();

    if (currentMillis - _weatherTimer > _weatherInterval) {
        _weatherTimer = currentMillis;
        fetchWeather();

        numUpdates++;
        String update = String(numUpdates);
        const char* c_update = update.c_str();
        char char_update[update.length() + 1];
        strcpy(char_update, c_update);
    
        weather_display.printText(40, 380, "Weather Update:", 0xFFFF, 0x0000);  // White text on black
        weather_display.printText(133, 380, char_update, 0xFFFF, 0x0000);  // White text on black
    }
}

void WeatherStation::fetchWeather() {
    String serverPath = "http://api.weatherapi.com/v1/forecast.json?key=" + _weatherApiKey + "&q=" + _latlon + "&days=3&aqi=no&alerts=yes";
    Serial.println("Requesting weather: " + serverPath);

    String jsonBuffer = httpGETRequest(serverPath.c_str());
    const size_t capacity = 60 * 1024; //60 = number of bytes && 1024 = 10 bytes processor 
    DynamicJsonDocument doc(capacity);
    DeserializationError error = deserializeJson(doc, jsonBuffer);
    Serial.println(jsonBuffer);

    if (error) {
        Serial.println("Weather JSON parse failed!");
        Serial.println(error.c_str());
        return;
    }

    JsonObject location = doc["location"];
    JsonArray forecastDay = doc["forecast"]["forecastday"];
    JsonObject Aaj = doc["current"];
    JsonObject Kal = forecastDay[1];
    JsonObject Parso = forecastDay[2];
    
    // Location Info
    String city = location["name"].as<String>();
    String region = location["region"].as<String>();

    Serial.println("City: " + city);
    Serial.println("Region: " + region);
    weather_display.printText(40, 60, "City:", 0xFFFF, 0x0000);
    weather_display.printText(73, 60, city.c_str(), 0xFFFF, 0x0000);
    weather_display.printText(40, 80, "Region:", 0xFFFF, 0x0000);
    weather_display.printText(83, 80, region.c_str(), 0xFFFF, 0x0000);

    // Current Weather
    weather_display.printText(40, 120, "Today's Weather Data:", 0xFFFF, 0x0000);

    String condition = Aaj["condition"]["text"];//.as<String>();
    String temp = String(Aaj["temp_c"].as<float>(), 1);
    String feelslike = String(Aaj["feelslike_c"].as<float>(), 1);
    String humidity = String(Aaj["humidity"].as<int>());

    weather_display.printText(40, 140, "Condition:", 0xFFFF, 0x0000);
    weather_display.printText(100, 140, condition.c_str(), 0xFFFF, 0x0000);
    weather_display.printText(40, 160, "Temperature (C):", 0xFFFF, 0x0000);
    weather_display.printText(140, 160, temp.c_str(), 0xFFFF, 0x0000);
    weather_display.printText(40, 180, "Feels Like (C):", 0xFFFF, 0x0000);
    weather_display.printText(135, 180, feelslike.c_str(), 0xFFFF, 0x0000);
    weather_display.printText(40, 200, "Humidity (%):", 0xFFFF, 0x0000);
    weather_display.printText(120, 200, humidity.c_str(), 0xFFFF, 0x0000);

    // Forecast Next Day
    String kal_date = Kal["date"];//.as<String>();
    String kal_condition = Kal["day"]["condition"]["text"].as<String>();
    String kal_maxtemp = String(Kal["day"]["maxtemp_c"].as<float>(), 1);
    String kal_mintemp = String(Kal["day"]["mintemp_c"].as<float>(), 1);

    weather_display.printText(40, 220, kal_date.c_str(), 0xFFFF, 0x0000);
    weather_display.printText(100, 220, " Weather Data:", 0xFFFF, 0x0000);
    weather_display.printText(40, 240, "Condition:", 0xFFFF, 0x0000);
    weather_display.printText(100, 240, kal_condition.c_str(), 0xFFFF, 0x0000);
    weather_display.printText(40, 260, "Max Temp (C):", 0xFFFF, 0x0000);
    weather_display.printText(120, 260, kal_maxtemp.c_str(), 0xFFFF, 0x0000);
    weather_display.printText(40, 280, "Min Temp (C):", 0xFFFF, 0x0000);
    weather_display.printText(120, 280, kal_mintemp.c_str(), 0xFFFF, 0x0000);

    // Forecast Next Day
    String parso_date = Parso["date"];//.as<String>();
    String parso_condition = Parso["day"]["condition"]["text"].as<String>();
    String parso_maxtemp = String(Parso["day"]["maxtemp_c"].as<float>(), 1);
    String parso_mintemp = String(Parso["day"]["mintemp_c"].as<float>(), 1);

    weather_display.printText(40, 300, parso_date.c_str(), 0xFFFF, 0x0000);
    weather_display.printText(100, 300, " Weather Data:", 0xFFFF, 0x0000);
    weather_display.printText(40, 320, "Condition:", 0xFFFF, 0x0000);
    weather_display.printText(100, 320, parso_condition.c_str(), 0xFFFF, 0x0000);
    weather_display.printText(40, 340, "Max Temp (C):", 0xFFFF, 0x0000);
    weather_display.printText(120, 340, parso_maxtemp.c_str(), 0xFFFF, 0x0000);
    weather_display.printText(40, 360, "Min Temp (C):", 0xFFFF, 0x0000);
    weather_display.printText(120, 360, parso_mintemp.c_str(), 0xFFFF, 0x0000);
}

String WeatherStation::httpGETRequest(const char* serverName) {
    WiFiClient client;
    HTTPClient http;
    http.begin(client, serverName);

    int httpResponseCode = http.GET();
    String payload = "{}";

    if (httpResponseCode > 0) {
        payload = http.getString();
    } else {
        Serial.print("HTTP GET error: ");
        Serial.println(httpResponseCode);
    }
    http.end();
    return payload;
}
