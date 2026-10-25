#include "NTPClock.h"
#include "ST7796U.h"
#include <SD.h>

File wavFile;

const int SAMPLE_RATE = 22050;          // set this to your WAV's sample rate
const int SAMPLE_PERIOD_US = 1000000 / SAMPLE_RATE;

const float alphaLP = 0.05f; //Low Pass filter coefficient
const float alphaHP = 0.5f;  //High Pass filter coefficient
float lp = 128.0f;
float hp = 128.0f;

int Audio_Enable = 4; //Speaker Enable pin

//To make sure the audio plays finite loop was designed
unsigned long currtime, prevtime = 0;
unsigned long interval = ((30 * 1000) + 50); //song length * 1000 + 50 buffer processing

ST7796U time_display(LCD_CS, LCD_DC, LCD_RST, LCD_BL, LCD_SCK, LCD_MOSI);

NTPClock::NTPClock(const char* ntpServer)
  : _ntpServer(ntpServer) {}

void NTPClock::begin() 
{
  TimezoneInfo tz = getTimezoneFromCoordinates();

  configTime(tz.gmtOffset, tz.daylightOffset, _ntpServer);

  // Wait for time to be set
  struct tm timeinfo;

  for(int retries = 0; !getLocalTime(&timeinfo) && retries < 10; retries++) 
    delay(500);

  Serial.println(getLocalTime(&timeinfo) ? "Time synced successfully." : "Failed to sync time.");  
}

NTPClock::TimezoneInfo NTPClock::getTimezoneFromCoordinates() {
  HTTPClient http;
  
  // Replace with your actual coordinates
  String url = "http://api.timezonedb.com/v2.1/get-time-zone?key=UP52STE0GQDX&format=json&by=position&lat=40.745298843183434&lng=-74.04564634190454";
  
  http.begin(url);
  int httpCode = http.GET();

  if (httpCode != 200) {
    Serial.println("Timezone API error");
    http.end();
    return {0, 0}; // UTC fallback
  }

  String payload = http.getString();
  http.end();

  // Parse gmtOffset from response
  int offsetIndex = payload.indexOf("\"gmtOffset\":");
  if (offsetIndex == -1) return {0, 0};
  
  offsetIndex += 12;
  int gmtOffset = payload.substring(offsetIndex, payload.indexOf(",", offsetIndex)).toInt();
  
  // Parse DST offset if needed
  int dstIndex = payload.indexOf("\"dst\":\"");
  int dstOffset = 0;
  if (dstIndex != -1 && payload.charAt(dstIndex + 7) == '1') {
    dstOffset = 3600; // 1 hour DST
  }

  return {gmtOffset, dstOffset};
}

void NTPClock::printLocalTime() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    Serial.println("Failed to obtain time");
    return;
  }

  char timeStr[30]; 
  strftime(timeStr, sizeof(timeStr), "%a, %b %D %r", &timeinfo);
  
  if(timeinfo.tm_hour == 8 && timeinfo.tm_min == 00) //Hardcode time in 24-hour format
  {
    AlarmSetup("/wake_up_audio_Lsample.wav");
    AlarmON();
    Serial.println("Alarm ON");
  }

  time_display.printText(40, 100, timeStr, 0xFFFF, 0x0000);  // White text on black
  Serial.println(timeStr);
}

void NTPClock::AlarmSetup(String audioFile) // Add "/" before the file name
{
  pinMode(Audio_Enable, OUTPUT);
  digitalWrite(Audio_Enable, HIGH); //Grounding for speaker

  if (!SD.begin(SD_CS)) {
    Serial.println("SD Card init failed!");
  }

  wavFile = SD.open(audioFile);
  if (!wavFile) {
    Serial.println("Failed to open Audio File");
  }
}

void NTPClock::AlarmON(void)
{
  currtime = millis();
  digitalWrite(Audio_Enable, LOW);
  
  while(currtime <= interval + prevtime) //Loop to ensure the audio plays out
  {
    
    if (!wavFile.available()) {
      wavFile.seek(44); // restart file
      return;
    }

    int sample = wavFile.read();   // 0..255
    if(sample < 0) return;

    float sampleF = (float)sample;
    //Bandpass Filtering
    lp = lp + alphaLP * (sampleF - lp); 
    hp = hp + alphaHP * (sampleF - hp);

    float bp = hp - lp;
    bp = (bp * 4) + 128; //Gain = 4 for volume
    if(bp < 0) bp = 0;
    if(bp > 255) bp = 255;

    bp = constrain(bp, 0, 255); //8 bit DAC can from 0 to 255 only

    dacWrite(DAC_PIN, (uint8_t)bp);
    delayMicroseconds(SAMPLE_PERIOD_US); //Enough delay to play each 8-bit line
  }

  prevtime = currtime;
  digitalWrite(Audio_Enable, HIGH);
}
