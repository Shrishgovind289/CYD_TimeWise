# CYD_ChronoWeather V1.1
I have created a basic Table Clock using the ESP32 based Cheap Yellow Display(CYD).

Developed ST7796U TFT driver for my CYD device. I could have used the TFT_eSPI but I was stuggling in configuring it so out of frustration manual developed the driver. Its a temporary and testing solution.
I do have plans to used Graphics library to create animations. The fonts.h file is not prefect but gets job done.

In this project I am using NTP Server to get the time data using my network.

WeatherAPI.com was used to get weather data. https://www.weatherapi.com/

![V1](https://github.com/user-attachments/assets/b389e814-ec45-466e-8340-74e36d75b2b3)

In V1.1 an alarm clock was added. I had a an audio .wav file uploaded in the SD card. The audio file was edited to be 8-bit PCM format using Audacity, to make the software side in the ESP32 easy. Initally the audio quality was terrible and the music was too fast but the a microsecond delay was calculated using the sampling rate and add to slow down the looping process to make the sample audible. Bandpass Filter was added to make the audio clear, the alpha and gain values were not calculated just pure trial and error. The audio quality is not perfect due to low sampling rate and filtering but for a 30 sec audio twice is only gonna be playing twice in 24hrs, its good enough.
As far as hardware goes the CYD had an integrated amplifier and I am using the 8-bit internal DAC (One of the main reasons for poor quality). The speaker I used was from a 2nd gen Amazon dot. The dot was damaged and not working so I savaged parts and got the speaker and using it with my CYD.
