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
Moonrise = Sunset + 35 minutes
Moonset = Sunrise - 35 minutes
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
---
# Version Summary
| Version | Major Features |
|---|---|
| **V1** | Time + Weather |
| **V1.1** | Time + Weather + Alarm |
| **V2** | ESP-IDF rewrite of Time + Weather + Alarm |
| **V2.1** | WebSocket remote control |
| **V2.2** | Navidrome music streaming |
The development progression can be summarized as:
```text
V1
Time + Weather
│
▼
V1.1
Time + Weather + Alarm
│
▼
V2
ESP-IDF Rewrite
Time + Weather + Alarm
│
▼
V2.1
WebSocket Remote Control
│
▼
V2.2
Navidrome Music Player
```
--
# Current Status
**V2.2 is currently undergoing endurance testing.**
The main areas being tested are:
- Long-duration music playback
- Automatic song changes
- Repeated Music Mode enable/disable cycles
- WebSocket reconnects
- Wi-Fi interruptions
- Navidrome interruptions
- MP3 decoder stability
- DAC stability
- Heap usage
- Memory fragmentation
- LVGL task destruction/recreation
- Long-term TimeWise operation
The goal is to verify that the system remains stable over extended periods before adding additional functionality.
---
# Future Development
Possible future improvements include:
- Improved Music Mode UI
- Additional animations
- Better audio quality
- Improved audio filtering
- Additional Navidrome controls
- Playlist support
- Song search
- Improved astronomy calculations
- Additional weather information
- More WebSocket controls
- Further memory optimization
- Remote access outside the local network
- Long-term reliability improvements
---
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
