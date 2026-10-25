#ifndef NTP_CLOCK_H
#define NTP_CLOCK_H

#include <WiFi.h>
#include <HTTPClient.h>
#include <time.h>
#include <Arduino.h>

#define DAC_PIN 26       // DAC output
#define SD_CS 5          // SD card chip select

class NTPClock {
  public:
    NTPClock(const char* ntpServer);
    void begin();
    void printLocalTime();

  private:
    const char* _ntpServer;

    struct TimezoneInfo {
      long gmtOffset;     // seconds offset from UTC (negative means behind UTC)
      int daylightOffset; // seconds of daylight saving offset
    };

    String detectRegionFromAPI();
    TimezoneInfo getTimezoneFromCoordinates();
    void AlarmSetup(String audioFile);
    void AlarmON(void);
};

#endif


