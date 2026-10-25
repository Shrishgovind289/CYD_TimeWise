#include "ST7796U.h"
#include "NTPClock.h"
#include "WeatherStation.h"
#include <WiFi.h>

ST7796U tft(LCD_CS, LCD_DC, LCD_RST, LCD_BL, LCD_SCK, LCD_MOSI);

const char* ssid = "SSID";
const char* password = "PASSWORD";
String weatherApiKey = "WEATHER API";

NTPClock ntpclock("pool.ntp.org");
WeatherStation weatherStation(weatherApiKey);

void setup() {
  Serial.begin(115200);
  
  tft.begin();
  tft.fillScreen(0x0000);          // Black screen
  tft.printText(60, 40, "Hello, Mr. Revankar", 0xFFFF, 0x0000);  // White text on black

  WiFi.mode(WIFI_STA);
  delay(100);
  /*int n = WiFi.scanNetworks(); //Testing
  for (int i = 0; i < n; i++) {
    Serial.println(WiFi.SSID(i));
  }*/
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  tft.printText(60, 60, "Connecting to WiFi", 0xFFFF, 0x0000);
  
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(WiFi.status()); // Testing
    tft.printText(60, 70, WiFi.status(), 0xFFFF, 0x0000);
  }
  tft.clearLine(60, 60, 200, 0x0000);
  tft.clearLine(60, 70, 200, 0x0000);

  ntpclock.begin();          // sync time via 

  ntpclock.printLocalTime();
  weatherStation.fetchWeather();
}

void loop() {
  weatherStation.updateWeather();  // fetch weather every 10 seconds using fixed latlon
  ntpclock.printLocalTime();           // print time
  delay(1000);
}
