# CYD_TimeWise
TimeWise is an ESP32-based smart table clock built using the **Cheap Yellow Display (CYD)**.
The project originally started as a simple clock and weather display using the Arduino IDE. Over time, it has evolved into an ESP-IDF-based embedded system with weather visualization, alarms, WebSocket-based remote control, and Navidrome music streaming.
The project has been developed incrementally, with each version representing a major functional milestone.

---
# Version History
## V1 — Time and Weather
The first version of TimeWise was developed using the **Arduino IDE**.
The goal of V1 was simple:
- Display the current time
- Synchronize time over Wi-Fi
- Retrieve and display weather information
### Display
The CYD uses an **ST7796U TFT display**.
Initially, I attempted to configure `TFT_eSPI`, but after struggling with the configuration I decided to manually develop a basic ST7796U display driver.
The driver was mainly intended as a temporary/testing solution, but it was enough to get the original TimeWise interface working.
A simple custom `fonts.h` implementation was also used. It was not perfect, but it got the job done for the first version.
### Time
Time synchronization was implemented using an **NTP server**.
The ESP32 connects to Wi-Fi and periodically synchronizes its system time through NTP.
### Weather
Weather information was retrieved using: https://www.weatherapi.com/
The weather data was parsed by the ESP32 and displayed along with the current time.

![V1](https://github.com/user-attachments/assets/b389e814-ec45-466e-8340-74e36d75b2b3)

### V1 Features
- ESP32 Cheap Yellow Display
- Arduino IDE
- Custom ST7796U TFT driver
- NTP time synchronization
- Wi-Fi connectivity
- WeatherAPI integration
- Current time display
- Current weather display
  
---
# V1.1 — Alarm Clock
V1.1 expanded the original clock by adding an **alarm system with audio playback**.
The alarm audio is stored as a `.wav` file on the SD card.
The original audio file was edited using **Audacity** and converted into an **8-bit PCM WAV file** to simplify playback on the ESP32.
### Audio Playback
Initially, the audio quality was terrible and the audio played much too quickly.
To correct the playback speed, a delay between DAC samples was calculated using the audio sampling rate.
Conceptually:
```text
Sample Period = 1 / Sampling Rate
```
The resulting microsecond delay was added between samples while sending the audio data to the ESP32 DAC.
This allowed the WAV file to play at approximately the correct speed.
### Audio Filtering
A simple **band-pass filter** and gain adjustment were later added to improve the sound.
The filter coefficients and gain values were not mathematically optimized. They were adjusted experimentally until the alarm audio became reasonably clear.
The audio quality is still not perfect due to:
- Low audio sampling rate
- ESP32 internal 8-bit DAC
- Basic filtering
- Limited audio hardware
However, the alarm audio is only around 30 seconds long and plays twice when the alarm is triggered, so the resulting quality is more than sufficient for the application.
### Audio Hardware
The CYD board contains an integrated audio amplifier.
The speaker used for TimeWise was salvaged from a damaged **2nd-generation Amazon Echo Dot**.
The Echo Dot itself was no longer working, so I reused the speaker for the TimeWise project.
### V1.1 Features
Everything from V1 plus:
- Alarm clock
- Alarm enable/disable
- WAV file stored on SD card
- 8-bit PCM audio
- ESP32 internal DAC
- CYD integrated amplifier
- Band-pass filtering
- Gain adjustment
- Salvaged Amazon Echo Dot speaker
V1.1 is the final version developed using the **Arduino IDE**.
At this point, I wanted a significantly more advanced graphical interface with images, animations, better task management, networking, and more control over the ESP32 hardware.
Development therefore moved to **ESP-IDF**.

---
# V2 — ESP-IDF Rewrite
V2 is a major rewrite of TimeWise using **ESP-IDF**.
The goal of V2 was not initially to add new major features, but instead to recreate the core functionality of V1.1 using a better embedded architecture.
The three core functions remain:
```text
Time
Weather
Alarm
```
However, the internal implementation was completely redesigned.
## ESP-IDF
Moving from Arduino to ESP-IDF provided significantly more control over:
- FreeRTOS tasks
- Memory
- Networking
- SPI
- SD card access
- DAC peripherals
- HTTP communication
- Display drivers
- System scheduling
It also made it easier to progressively expand TimeWise into a more complete embedded system.

---
## LVGL
V2 uses **LVGL** for the graphical interface.
This replaced the basic custom graphics implementation used in the Arduino version.
Using LVGL allowed the interface to support:
- Labels
- Images
- Dynamic backgrounds
- Weather icons
- Sun/Moon graphics
- More flexible positioning
- Future animations
  
---
## Flash Partition 
V2 uses significantly more application code and graphical resources than the original Arduino version.
Because of this, the ESP32 flash memory partition layout was modified to provide additional application space.

---
## Weather
V2 replaces WeatherAPI with: https://open-meteo.com/
Open-Meteo provides weather information in a format that is convenient for embedded parsing.
The ESP32 currently retrieves information such as:
- Temperature
- Weather condition
- Weather code
- Day/night state
- Wind speed
- Wind direction
- Sunrise
- Sunset
Weather information is retrieved periodically and used to update the TimeWise interface.

---
## Dynamic Weather Backgrounds
The V2 interface uses full-screen backgrounds that change depending on the current weather.
Instead of storing all of the images inside ESP32 flash, the backgrounds are stored on the **SD card**.
Images are first resized and edited using Canva.
They are then converted from PNG into raw **RGB565 `.BIN` files** using: https://longfangsong.github.io/en/image-to-rgb565/
The resulting files are copied onto the SD card.
The ESP32 loads the appropriate background depending on the current weather condition.
Example directory:
```text
SD Card
│
└── BG
    ├── SUNNY.BIN
    ├── NIGHT.BIN
    ├── CLOUD.BIN
    ├── RAIN.BIN
    ├── FOG.BIN
    ├── SNOW.BIN
    ├── THUND.BIN
    └── ...
```
---
## Weather Icons
Weather icons are also stored on the SD card as RGB565 binary files.
Example:
```text
SD Card
│
└── ICO
    ├── SUNNY.BIN
    ├── NIGHT.BIN
    ├── CLOUD.BIN
    ├── FRAIN.BIN
    ├── HRAIN.BIN
    ├── LRAIN.BIN
    ├── SNOW.BIN
    └── ...
```
The icon displayed depends on the weather code received from Open-Meteo.

---
## Sun and Moon Indicator
V2 also includes a graphical **Sun and Moon indicator**.
The Sun and Moon graphics are converted into C arrays and compiled directly into the firmware.
The astronomical body moves along an arc on the display to approximately represent its position throughout the day or night.
Sunrise and sunset times are provided by Open-Meteo.
Moon timing was more difficult because astronomical APIs use different reference points that did not visually align with what I wanted for the interface.
For the current implementation, the Moon timing is approximated as:
```text
Moonrise = Sunset + 35 minutes | Moonset = Sunrise - 35 minutes
```
This is not intended to be astronomically accurate.
It is primarily used as a visual approximation for the interface.

<img width="599" height="900" alt="WhatsApp Image 2026-08-02 at 10 39 49 PM" src="https://github.com/user-attachments/assets/de70f391-3b28-42cf-a8ad-3d18bfd01db8" />

---
## Alarm
The alarm functionality from V1.1 was rewritten for ESP-IDF.
The alarm audio continues to be stored on the SD card and played through the ESP32 internal DAC and the CYD amplifier.
The new implementation also makes it easier to integrate alarm control with the rest of the system.
### V2 Features
- ESP-IDF
- LVGL
- NTP
- Open-Meteo
- Time and date
- Current weather
- Dynamic RGB565 backgrounds
- Weather icons
- Wind speed
- Wind direction
- Sunrise and sunset information
- Sun/Moon graphical indicator
- Day/night display behavior
- SD card storage
- Alarm system
- WAV audio playback
- ESP32 internal DAC
- FreeRTOS-based scheduling
- Modified flash partition
  
---
# V2.1 — WebSocket Control
V2.1 adds a **web-based remote control interface**.
The ESP32 runs an HTTP/WebSocket server that allows TimeWise to be controlled from another device on the same network.
For example:
```text
Phone / PC
     │
     │ Wi-Fi
     ▼
Web Browser
     │
     │ WebSocket
     ▼
ESP32 TimeWise
```
The webpage itself is stored on the SD card.
Example:
```text
SD Card
│
└── WEB
    ├── index.htm
    ├── Logo.png
    └── DRAW.png
```
The ESP32 serves the page through its HTTP server while WebSockets provide real-time communication with the firmware.

---
## WebSocket Controls
The WebSocket interface currently supports controls such as:
- Alarm time
- Alarm enable/disable
- Alarm stop
- Snooze
- Volume
- Display brightness
Because WebSockets maintain a persistent connection, commands can be sent directly to the ESP32 without repeatedly refreshing the webpage.
Example architecture:
```text
Browser
   │
   │ JSON Commands
   ▼
WebSocket Server
   │
   ▼
TimeWise Firmware
   │
   ├── Alarm
   ├── Audio
   └── Display
```
### V2.1 Features

Everything from V2 plus:
- ESP32 HTTP server
- SD-card-hosted control webpage
- WebSocket communication
- Alarm configuration from browser
- Snooze control
- Alarm stop control
- Volume control
- Brightness control
- Real-time device state
  
---
# V2.2 — Navidrome Music Player
V2.2 adds a network music player to TimeWise.
Music is streamed from a **Navidrome server**. https://www.navidrome.org/
Navidrome provides a Subsonic-compatible API that allows the ESP32 to request songs and stream audio directly from the music server.

---
## Music Selection
TimeWise can request a random song from Navidrome.
Navidrome returns information including:
- Song ID
- Song title
- Artist
- Album
- Duration
This information is then used by the MusicPlayer.

---
## MP3 Streaming
Songs are streamed directly over HTTP.
The complete song is **not downloaded into ESP32 RAM**.
Instead:
```text
Navidrome
    │
    ▼
HTTP MP3 Stream
    │
    ▼
ESP32
```
The MP3 stream is processed continuously as the song plays.

---
## MP3 Decoding
The streamed MP3 data is decoded using the **Espressif audio codec library**.
The decoder produces:
```text
16-bit PCM
44.1 kHz
Stereo
```
depending on the source audio.
The stereo samples are converted to mono before being sent to the ESP32 DAC.

---
## Music Audio Pipeline
The current audio pipeline is:
```text
Navidrome Server
        │
        ▼
Subsonic API
        │
        ▼
HTTP MP3 Stream
        │
        ▼
MP3 Decoder
        │
        ▼
16-bit PCM
        │
        ▼
Stereo → Mono
        │
        ▼
Band-pass Filter
        │
        ▼
Gain
        │
        ▼
ESP32 Internal DAC
        │
        ▼
CYD Amplifier
        │
        ▼
Speaker
```
The same audio filtering and gain processing used by the alarm system is also reused by the music player.

---
# Dedicated Music Mode
One of the biggest challenges with V2.2 was **ESP32 RAM usage**.
MP3 decoding requires a relatively large contiguous block of memory.
At the same time, TimeWise normally has several memory-intensive components running:
```text
LVGL
Weather
WebSocket
HTTP
SD Card
Audio
MP3 Decoder
```
Running all of them simultaneously caused memory allocation problems.
To solve this, TimeWise now uses two different operating modes.

---
## Normal TimeWise Mode
When:
```text
Music Enable = 0
```
TimeWise operates normally.
```text
TIMEWISE MODE
│
├── Time
├── Date
├── Weather
├── Weather Background
├── Weather Icon
├── Sun / Moon
├── Alarm
├── LVGL
└── Full WebSocket Server
```
The MusicPlayer is completely disabled in this mode.

---
## Music Mode
When:
```text
Music Enable = 1
```
TimeWise switches into a dedicated Music Mode.
```text
MUSIC MODE
│
├── Navidrome
├── MP3 Decoder
├── MusicPlayer
├── DAC
├── Now Playing
└── Lightweight WebSocket
```
Normal TimeWise operations are temporarily stopped.
This includes:
- Clock display updates
- Weather requests
- Weather display updates
- Astronomy updates
- Normal alarm scheduling
- Normal LVGL processing
  
---
## LVGL Memory Management
The LVGL task uses a significant amount of RAM.
To make enough memory available for the MP3 decoder, TimeWise performs the following sequence:
```text
Music Enable = 1
        │
        ▼
Draw Music Screen
        │
        ▼
Stop LVGL Tick
        │
        ▼
Delete LVGL Task
        │
        ▼
Release LVGL Stack Memory
        │
        ▼
Start Navidrome
        │
        ▼
Start MP3 Decoder
        │
        ▼
Start DAC
```
The image already written to the TFT remains visible because the LCD retains the framebuffer information that was sent to it.
This allows the LVGL task memory to be reused by the MP3 decoder.

---
## Returning to TimeWise
When:
```text
Music Enable = 0
```
the opposite process occurs:
```text
Stop MusicPlayer
        │
        ▼
Stop MP3 Decoder
        │
        ▼
Release DAC
        │
        ▼
Recreate LVGL Task
        │
        ▼
Restart LVGL
        │
        ▼
Restore Full WebSocket Server
        │
        ▼
Refresh Weather
        │
        ▼
Return to TimeWise
```
---
# Music Controls
The WebSocket interface was expanded in V2.2 with music controls.
Current controls include:
- Music Mode Enable / Disable
- Play Random
- Play
- Pause
- Resume
- Next
- Previous
- Stop
- Music Volume
The webpage also receives information about the currently playing track.

---
## Now Playing
The TimeWise display shows information about the current song including:
```text
Song Title
Artist
Duration
```
Example:
```text
Numb
Linkin Park
3:07
```
---
## Automatic Next Song
When a song finishes normally, TimeWise automatically requests another random song from Navidrome.
The sequence is:
```text
Song Playing
     │
     ▼
Song Finished
     │
     ▼
MusicPlayer Cleanup
     │
     ▼
Request Random Song
     │
     ▼
Navidrome Returns Song
     │
     ▼
Start MusicPlayer
     │
     ▼
Next Song
```
The device can therefore continue playing random music without user interaction.

---
# Current Architecture
The current TimeWise architecture looks approximately like this:
```text
                         ┌─────────────────┐
                         │      ESP32      │
                         └────────┬────────┘
                                  │
             ┌────────────────────┼────────────────────┐
             │                    │                    │
             ▼                    ▼                    ▼
          Display              Network              Audio
             │                    │                    │
             │                    │                    │
        ┌────┴────┐        ┌──────┴──────┐      ┌─────┴─────┐
        │  LVGL   │        │    Wi-Fi    │      │   Alarm   │
        └────┬────┘        └──────┬──────┘      └─────┬─────┘
             │                    │                    │
     ┌───────┼────────┐     ┌─────┼──────┐             │
     │       │        │     │     │      │             ▼
   Time   Weather   Astro   NTP  HTTP  WebSocket       DAC
                              │
                              │
                              ▼
                         Navidrome
                              │
                              ▼
                         MP3 Stream
                              │
                              ▼
                          Decoder
                              │
                              ▼
                             DAC
```
---
# SD Card Structure
The SD card currently stores several TimeWise resources.
Example:
```text
SD Card
│
├── alarm.wav
│
├── BG
│   ├── SUNNY.BIN
│   ├── NIGHT.BIN
│   ├── CLOUD.BIN
│   ├── RAIN.BIN
│   ├── SNOW.BIN
│   └── ...
│
├── ICO
│   ├── SUNNY.BIN
│   ├── NIGHT.BIN
│   ├── CLOUD.BIN
│   ├── LRAIN.BIN
│   ├── HRAIN.BIN
│   └── ...
│
└── WEB
    ├── index.htm
    ├── Logo.png
    └── DRAW.png
```
# Project Evolution
What started as:
```text
ESP32 + TFT
      ↓
Display Time
```
has gradually evolved into:
```text
                    TimeWise
                       │
        ┌──────────────┼──────────────┐
        │              │              │
      Clock          Weather         Audio
        │              │              │
       NTP         Open-Meteo    Alarm + Music
        │              │              │
        └──────────────┼──────────────┘
                       │
                    ESP32
                       │
             ┌─────────┴─────────┐
             │                   │
          WebSocket           Navidrome
             │                   │
             ▼                   ▼
        Remote Control      Music Streaming
```

TimeWise has become an ongoing project for experimenting with **embedded systems, networking, audio processing, graphical interfaces, memory optimization, and ESP32 development**.

# V2.3 — Display Improvements and Automatic Brightness

V2.3 focuses primarily on improving the visual behavior of TimeWise.

After using the display for longer periods, several practical problems became noticeable.

These were not major architectural problems, but they affected how usable the clock was throughout the day.

The main improvements in V2.3 were:
- Automatic night brightness adjustment
- Dynamic font color
- Improved background image updates
- Better coordination between weather data and the display

---
## Problem — Display Too Bright at Night

The TFT backlight worked well during the day, but the same brightness level was uncomfortable when TimeWise was used at night.

Originally, display brightness was simply controlled using a fixed value or through the web interface.

For example:
```text
Day
│
▼
100% Brightness

Night
│
▼
100% Brightness
```

This made the clock unnecessarily bright in a dark room.

Manually adjusting the brightness every night was also not a good solution because TimeWise is intended to operate continuously without requiring regular user interaction.

---
## Solution — Automatic Night Brightness

The display brightness system was connected to the existing day/night information.

TimeWise already knows whether it is currently day or night from the weather and astronomy data.

The brightness logic could therefore use:
```text
is_day
Current Hour
Requested Brightness
```

to calculate the actual TFT backlight level.

The resulting behavior became:
```text
Day
│
▼
Normal User Brightness

Night
│
▼
Reduced Brightness
```

A stronger brightness limit can also be applied during late-night hours.

For example:
```text
Night + Midnight to Early Morning
               │
               ▼
      Very Low Brightness
```

This allows the display to remain readable without lighting up the room.

The user can still select the normal brightness level, while TimeWise automatically applies the appropriate night-time reduction.

---
## Problem — Text Visibility Changed With Background Images

The dynamic weather backgrounds introduced another visual problem.

Some backgrounds are very bright while others are very dark.

For example:
```text
Sunny Background
        │
        ▼
Very Bright Image
```

while:
```text
Thunder / Night Background
        │
        ▼
Dark Image
```

Using one fixed text color for every background meant that text could become difficult to read.

White text works well on dark backgrounds but can disappear against bright clouds or sunlight.

Similarly, dark text can become difficult to see on night or storm backgrounds.

---
## Solution — Dynamic Font Color

V2.3 adds background-dependent font coloring.

Each weather background can be associated with an appropriate text color.

Conceptually:
```text
Weather Condition
       │
       ▼
Background Selection
       │
       ├── Bright Background
       │        │
       │        ▼
       │    Dark Text
       │
       └── Dark Background
                │
                ▼
            Light Text
```

The same color selection can then be applied to:
- Time
- Date
- Location
- Temperature
- Weather condition
- Forecast information

This allows the display to remain readable even when the full-screen weather image changes significantly.

---
## Problem — Background Image Updates

The weather background is a full-screen RGB565 image stored on the SD card.

Changing the background is therefore more complicated than changing a normal LVGL label.

The application must:
```text
Receive New Weather
       │
       ▼
Determine Background
       │
       ▼
Open RGB565 File
       │
       ▼
Load / Stream Image
       │
       ▼
Update LVGL
       │
       ▼
Redraw UI Elements
```

During development, background changes could interfere with other display elements.

Updating the background and weather elements independently could result in parts of the interface being updated at different times.

This was particularly noticeable when:
- Weather changed
- Day changed to night
- A different background was loaded
- Weather icons were refreshed

---
## Solution — Coordinated Weather Display Update

The display update process was reorganized so that weather information is applied as a coordinated update.

The weather response first prepares:
```text
Temperature
Condition
Location
Background
Weather Icon
Day / Night State
Sunrise
Sunset
```

The display module then applies the new information together.

The background image is updated first and the visible weather elements are refreshed immediately afterward.

This reduces unnecessary redraws and makes weather transitions more consistent.

---
# V3 — Server Gateway Architecture

As TimeWise continued to grow, the ESP32 became responsible for an increasing number of tasks.

By V2.2 it was handling:
```text
Display
LVGL
NTP
Weather Requests
Web Server
WebSocket
Navidrome API
MP3 Streaming
MP3 Decoding
Alarm Audio
Music Audio
SD Card
```

Individually these features worked well.

The problem appeared when several of them needed to operate at the same time.

---
## Problem — Too Much Processing on the ESP32

The biggest limitation was memory.

The ESP32 had to maintain:
- LVGL buffers
- FreeRTOS task stacks
- HTTP buffers
- WebSocket buffers
- MP3 buffers
- Decoder state
- Audio buffers
- Weather data
- SD card operations

MP3 decoding was particularly memory intensive.

This resulted in a system where enabling one feature could require disabling another feature.

For example:
```text
Music Enable
      │
      ▼
Stop LVGL
      │
      ▼
Release Display Task Memory
      │
      ▼
Start MP3 Decoder
```

Although this worked, it made the software increasingly complicated.

The ESP32 was effectively acting as both:
```text
Embedded Device
+
Application Server
```

---
## Problem — Direct Network Connections

The ESP32 also communicated directly with several services.

For example:
```text
Browser ────────► ESP32

ESP32 ──────────► Weather API

ESP32 ──────────► Navidrome
```

Each connection required additional:
- Networking code
- Buffers
- Authentication
- Error handling
- Reconnection logic
- Memory

The music player was particularly sensitive to interruptions because the ESP32 needed to maintain the Navidrome connection while simultaneously decoding and playing the MP3 stream.

---
## Solution — Introduce a Server

V3 introduces a Linux server between the ESP32 and external services.

Instead of treating the ESP32 as the central server, TimeWise was changed into a more traditional **server/client architecture**.

```text
External Services
       │
       ▼
TimeWise Server
       │
       ▼
ESP32 Client
```

The server handles tasks that are easier to perform on a Linux computer.

The ESP32 focuses mainly on embedded-device responsibilities.

---
## Server Responsibilities

The server handles:
```text
Web Interface
HTTP API
Weather Requests
Weather Cache
Navidrome Communication
Music Selection
Audio Conversion
Device Commands
Device State
```

The ESP32 handles:
```text
Display
NTP
Alarm
Weather Rendering
Forecast Rendering
Sun / Moon Display
Audio Output
DAC
Buttons / LEDs
HTTP Client
```

The new design therefore becomes:
```text
Browser
   │
   ▼
Linux Server
   │
   ▼
ESP32 Client
```

---
## Problem — Music Reliability

One of the main reasons for introducing the server was the music player.

In V2.2 the connection was:
```text
Navidrome
    │
    ▼
ESP32
    │
    ▼
MP3 Decoder
    │
    ▼
DAC
```

This required the ESP32 to:
1. Connect to Navidrome
2. Authenticate
3. Request a song
4. Maintain the HTTP stream
5. Buffer MP3 data
6. Decode MP3
7. Convert stereo audio
8. Filter the samples
9. Send audio to the DAC

A network interruption or memory problem anywhere in the chain could interrupt playback.

---
## Solution — Server to Client Music Architecture

V3 changes the ESP32 from the music-processing server into a much simpler audio client.

The new architecture becomes:
```text
Navidrome
    │
    ▼
Linux Server
    │
    ▼
ESP32 Client
```

The Linux server has significantly more:
- RAM
- CPU performance
- Networking capability
- Process isolation

than the ESP32.

This makes it a much better place to handle the connection to Navidrome.

The ESP32 only needs to maintain a connection to one service:
```text
TimeWise Gateway
```

This greatly simplifies the embedded networking architecture and provides a more reliable base for the music player.

---
# V3.1 — Server Weather and Five-Hour Forecast

After moving to the server/client architecture, weather was one of the next systems to be transferred to the gateway.

Previously:
```text
ESP32
   │
   ▼
Open-Meteo
```

V3.1 changes this to:
```text
Open-Meteo
    │
    ▼
TimeWise Server
    │
    ▼
ESP32
```

---
## Problem — Weather Requests on ESP32

Weather requests require:
- HTTP connections
- Response buffers
- JSON parsing
- Error handling
- API URL generation

The ESP32 already had several other network operations running.

Weather requests therefore added another Internet connection that the ESP32 needed to maintain.

There were also cases where the HTTP request itself succeeded but the complete weather update did not.

For example:
```text
HTTP Status = 200
      │
      ▼
Response Received
      │
      ▼
Parsing / Display Update Fails
```

This made debugging difficult because:
```text
Successful HTTP Request
```

did not necessarily mean:
```text
Successful Weather Update
```

---
## Solution — Server-Side Weather Request

The Open-Meteo connection was moved to the Linux server.

The server now performs the Internet request and stores the weather data.

The ESP32 only requests weather from the local gateway.

```text
Open-Meteo
     │
     ▼
Linux Gateway
     │
     ▼
Weather Cache
     │
     ▼
ESP32
```

This separates the system into two responsibilities.

The server handles:
```text
Internet Request
API Formatting
Weather Cache
```

The ESP32 handles:
```text
Weather Display
Weather Icons
Background Selection
Sun / Moon
```

---
## Problem — Five-Hour Forecast

Adding the forecast introduced another problem.

The original weather request was primarily designed around current weather.

A forecast contains several entries:
```text
Current
+1 Hour
+2 Hours
+3 Hours
+4 Hours
+5 Hours
...
```

Only the next five useful future entries are required by the display.

Trying to treat current weather and forecast information as one single display update made the weather code increasingly complicated.

It also meant that a forecast update could unnecessarily interfere with:
- Current temperature
- Current background
- Current weather icon
- Sun/Moon data

---
## Solution — Separate Current Weather and Forecast

V3.1 separates the two operations.

Current weather uses:
```text
/weather
```

Forecast data uses:
```text
/forecast
```

This creates two independent data paths:
```text
                 Gateway
                    │
          ┌─────────┴─────────┐
          │                   │
          ▼                   ▼
       /weather           /forecast
          │                   │
          ▼                   ▼
Current Weather        Five-Hour Forecast
```

The current-weather update controls:
- Temperature
- Weather condition
- Main weather icon
- Background
- Day/night state
- Sunrise
- Sunset
- Sun/Moon information

The forecast update controls only:
- Forecast time
- Forecast temperature
- Forecast weather code
- Forecast icon

This prevents the five-hour forecast from unnecessarily redrawing the entire dashboard.

---
## Problem — Selecting the Correct Five Hours

The Open-Meteo hourly response can contain many hours of data.

The ESP32 should not simply take the first five entries because some entries may correspond to:
- Earlier hours
- The current hour
- Previous cached data

The display should show the next **five future hours**.

---
## Solution — Forecast Time Filtering

The forecast data is compared against the current NTP time.

The logic becomes:
```text
Receive Forecast
       │
       ▼
Read Current NTP Time
       │
       ▼
Ignore Past Hours
       │
       ▼
Ignore Current Hour
       │
       ▼
Select Next Five Hours
       │
       ▼
Display Forecast
```

Each selected entry provides:
```text
Time
Temperature
Weather Code
Day / Night
```

The ESP32 then determines which weather icon should be displayed.

---
## Problem — Forecast Updating During Other Display Operations

The display can also be busy with:
- Background updates
- Music Mode
- Alarm playback
- LVGL activity
- Current weather updates

Immediately drawing the forecast from the HTTP task could cause several parts of the interface to update simultaneously.

---
## Solution — Pending Forecast Data

The received forecast is first stored as pending data.

```text
HTTP Forecast Response
        │
        ▼
Pending Forecast Buffer
        │
        ▼
Main Application
        │
        ▼
LVGL Display Update
```

The main application applies the forecast when it is safe to modify the display.

This keeps network operations separate from display operations and avoids unnecessary competition between tasks.

---
# V3.2 — Navidrome API and Server Audio Streaming

V3.2 focuses on redesigning the music system around the new server architecture.

The biggest change is that the ESP32 is no longer responsible for MP3 decoding.

---
## Problem — Direct Navidrome API on ESP32

In V2.2, the ESP32 communicated directly with the Navidrome Subsonic API.

The ESP32 was responsible for:
```text
Authentication
Song Selection
Song Metadata
Stream URL
MP3 Download
MP3 Decode
Audio Playback
```

This added a significant amount of networking and application logic to the embedded firmware.

It also meant that Navidrome credentials and API behavior needed to be handled by the ESP32.

---
## Solution — Navidrome Client on the Server

V3.2 moves the Navidrome API implementation to the TimeWise server.

```text
ESP32
   │
   │ Music Command
   ▼
TimeWise Server
   │
   │ Subsonic API
   ▼
Navidrome
```

The server stores the Navidrome configuration using environment variables instead of storing credentials inside the ESP32 firmware.

The server performs:
- Authentication
- Song selection
- Metadata requests
- Stream requests

The ESP32 only needs to understand the TimeWise gateway API.

---
## Problem — MP3 Decoding on ESP32

MP3 decoding was one of the most memory-intensive operations in TimeWise.

The decoder required:
- Compressed audio buffer
- Decoder state
- PCM output buffer
- FreeRTOS task stack

At the same time, the ESP32 also needed memory for LVGL and the rest of TimeWise.

This caused the original Music Mode to require several services to be stopped just to free enough memory for the MP3 decoder.

---
## Solution — Decode MP3 on the Server

The MP3 decoder was removed from the ESP32.

The Linux server now performs the decoding using **FFmpeg**.

```text
Navidrome
    │
    ▼
MP3 Stream
    │
    ▼
FFmpeg
    │
    ▼
PCM
```

The server converts the source audio into a format that the ESP32 can send almost directly to its DAC.

The output format is:
```text
24000 Hz
Mono
Unsigned 8-bit PCM
```

---
## Why 24 kHz Mono 8-bit PCM?

The ESP32 uses its internal **8-bit DAC**.

Sending high-resolution stereo audio would therefore provide little benefit while increasing:
- Network bandwidth
- Buffer size
- Processing requirements

The server performs:
```text
Original MP3
     │
     ▼
Decode
     │
     ▼
Resample to 24 kHz
     │
     ▼
Stereo → Mono
     │
     ▼
Convert to Unsigned 8-bit
```

The ESP32 receives audio in a form that closely matches what its DAC requires.

---
## Problem — Audio Streaming Reliability

Streaming audio continuously introduces another challenge.

The ESP32 cannot download the complete song before playback because songs can be several megabytes.

Holding an entire song in RAM is impossible.

The audio therefore needs to be processed continuously.

---
## Solution — Streaming PCM

The ESP32 requests:
```text
/music/stream?id=<song_id>
```

The gateway starts FFmpeg and streams the converted PCM data as it is generated.

```text
Navidrome MP3
      │
      ▼
FFmpeg
      │
      ▼
Small PCM Chunks
      │
      ▼
HTTP
      │
      ▼
ESP32 Buffer
      │
      ▼
DAC
```

Only a small amount of audio needs to exist in ESP32 memory at any given time.

This creates a true streaming pipeline.

---
## Problem — Music Connection Management

In the older architecture the ESP32 needed to maintain the connection all the way to Navidrome.

That meant:
```text
ESP32
   │
   ├── Navidrome Authentication
   ├── API Request
   ├── MP3 Connection
   ├── MP3 Decoder
   └── DAC
```

A failure in any one component could stop the complete music system.

---
## Solution — Separate Connections

V3.2 divides the music system into separate connections.

```text
Navidrome
    │
    │ Server Connection
    ▼
TimeWise Gateway
    │
    │ Client Connection
    ▼
ESP32
```

The server manages the external Navidrome connection.

The ESP32 manages only the local TimeWise connection.

This makes the system easier to:
- Debug
- Recover
- Modify
- Expand

It also means the ESP32 does not need to understand the internal Navidrome API.

---
## Music Selection Flow

Music selection now works approximately like this:
```text
User Presses Play Random
        │
        ▼
Browser
        │
        ▼
POST /api/control
        │
        ▼
Gateway Command Queue
        │
        ▼
ESP32
        │
        ▼
POST /music/select
        │
        ▼
Gateway
        │
        ▼
Navidrome API
        │
        ▼
Song Selected
        │
        ▼
Metadata Returned to ESP32
        │
        ▼
GET /music/stream
        │
        ▼
PCM Playback
```

---
## Music Audio Pipeline

The final V3.2 music pipeline is:
```text
Navidrome
    │
    ▼
MP3
    │
    ▼
TimeWise Gateway
    │
    ▼
FFmpeg
    │
    ▼
24 kHz Unsigned 8-bit Mono PCM
    │
    ▼
HTTP Stream
    │
    ▼
ESP32
    │
    ▼
U8 → S16
    │
    ▼
Band-pass Filter
    │
    ▼
Q8 Gain
    │
    ▼
S16 → U8
    │
    ▼
ESP32 DAC
    │
    ▼
CYD Amplifier
    │
    ▼
Speaker
```

---
# V3.3 — Forecast Scheduling and Memory Management

V3.3 focuses on improving the reliability of the weather dashboard while working within the ESP32's limited internal RAM. The server/client architecture introduced in V3 remains unchanged: the Linux gateway continues to provide the `/weather` and `/forecast` endpoints, while the ESP32 handles scheduling, data selection, and rendering.

---
## Problem — Weather Tasks Could Not Always Start

During extended operation, the ESP32 sometimes had enough total free heap but not a large enough contiguous allocation to create a new weather request task with an 8 KB stack. When task creation failed, subsequent weather and forecast requests could not proceed.

### Solution — Persistent Weather Worker

V3.3 creates a dedicated weather worker task at startup and signals it when an HTTP request is due, rather than creating and deleting the task for each request. This reserves the worker stack while memory is less fragmented and avoids repeated large task-stack allocations.

The worker handles **one request at a time**: either current weather or forecast data. This addresses task creation failures, but does not eliminate other potential memory or display problems.

---
## Problem — Five-Hour Forecast Icon Allocation

Each forecast weather icon is loaded from an RGB565 file on the SD card and converted to ARGB8888 for LVGL. For example, `NIGHT.BIN` has a 49 × 50 source image, using approximately 4,900 bytes of temporary RGB565 pixel data. Its rendered 29 × 30 ARGB8888 version occupies 3,480 bytes of retained pixel memory.

Both buffers are briefly needed during conversion. Additionally, each forecast card retains its own converted icon buffer, even when several cards display the same image.

With five forecast cards, logs showed the first three icons loading successfully, followed by 3,480-byte allocation failures for the fourth and fifth. The forecast API had already returned five entries successfully: **the failure occurred during icon conversion and allocation, not during data retrieval**.

### Solution — Four-Hour Forecast

V3.3 displays **the next four valid future hourly forecasts**. The ESP32 skips past and current hours and stores only four future entries. One fewer forecast card reduces the number of LVGL objects, time and temperature labels, icon buffers, and associated rendering work.

**No server-side change is required.** The gateway may continue to return its existing hourly data; the ESP32 selects the first four valid future entries. The number of forecast HTTP requests remains unchanged.

---
## Forecast Icon Buffer Release

V3.3 releases retained forecast icon buffers before scheduled current-weather requests, allowing the main weather update to run with more free memory. After a later forecast request, the icons are reloaded from the SD card.

The half-hour schedule uses local NTP time:

| Time | Action |
|---|---|
| `HH:59:55` | Release forecast icons; request current weather |
| `HH:01:30` | Request forecast data |
| `HH:01:55` | Permit forecast icon reload |
| `HH:29:55` | Release forecast icons; request current weather |
| `HH:31:30` | Request forecast data |
| `HH:31:55` | Permit forecast icon reload |

This makes current weather a priority over forecast graphics. The clock, date, main weather icon, background, and Sun/Moon indicator are intended to remain operational during forecast-buffer release and reload.

---
## Memory Diagnostics

V3.3 adds heap diagnostics that report:

- **Free heap:** total currently available memory.
- **Largest free block:** largest allocation that can be satisfied from a contiguous block.
- **Minimum free heap:** lowest free-heap value observed since boot.

Example observations from testing:

| Measurement | Five-hour test | Four-hour test |
|---|---:|---:|
| Free heap before icon reload | 21,344 bytes | 30,836 bytes |
| Largest block before reload | 10,240 bytes | 22,528 bytes |
| Forecast icons loaded | 3 of 5 | 4 of 4 |
| Free heap after reload | 10,580 bytes | 16,420 bytes |

The tests began with different available heap values, so the entire difference cannot be attributed solely to removing one forecast card. With four `NIGHT.BIN` icons, 13,920 bytes of converted icon pixels were retained; the total observed heap reduction was approximately 14,416 bytes, including other allocations and overhead.

---
## Overnight Test and Known Limitation

An overnight test showed that the clock and other TimeWise functions continued running, but forecast icon visibility failed by around 9 AM. Because there were no serial logs captured at the moment of failure, the cause has not yet been confirmed. Possibilities include a later allocation failure or a state-management problem during icon release and reload.

**V3.3 is a more consistent experimental configuration, not a complete fix for long-term forecast icon reliability.**

Possible future improvements include streaming RGB565 conversion one row at a time, reusing converted icon buffers for identical forecast conditions, and recording more detailed diagnostics for each forecast icon allocation and visibility transition.

---
# Version Summary
| Version | Major Features |
|---|---|
| **V1** | Time + Weather |
| **V1.1** | Time + Weather + Alarm |
| **V2** | ESP-IDF rewrite |
| **V2.1** | WebSocket remote control |
| **V2.2** | Direct Navidrome streaming and ESP32 MP3 decoding |
| **V2.3** | Automatic night brightness, dynamic font color and improved background updates |
| **V3** | Linux server/client architecture and HTTP gateway |
| **V3.1** | Server-side weather API and five-hour forecast |
| **V3.2** | Server-side Navidrome API, MP3 decoding and PCM audio streaming |
| **V3.3** | Four-hour forecast, persistent weather worker, half-hour scheduling, icon buffer release/reload and heap diagnostics |

The development progression can be summarized as:
```text
V1
Time + Weather
│
▼
V1.1
Alarm
│
▼
V2
ESP-IDF + LVGL
│
▼
V2.1
WebSocket Control
│
▼
V2.2
Direct Navidrome + ESP32 MP3 Decode
│
▼
V2.3
Display Automation
Brightness + Font Color + Background Updates
│
▼
V3
Linux Server / ESP32 Client
│
▼
V3.1
Server Weather + Five-Hour Forecast
│
▼
V3.2
Server Navidrome + FFmpeg + PCM Streaming
│
▼
V3.3
Four-Hour Forecast + RAM Management
```

## License

The TimeWise source code is licensed under the [MIT License](LICENSE).

Copyright © 2026 Shrishgovind Umesh Revankar.

Third-party software libraries and dependencies used by TimeWise are subject to their respective licenses.

Unless otherwise stated, media assets such as images, icons, fonts, and audio files are not covered by the MIT License.
