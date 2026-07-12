# CYD_TimeWise V1.1
I have created a basic Table Clock using the ESP32 based Cheap Yellow Display(CYD).

Developed ST7796U TFT driver for my CYD device. I could have used the TFT_eSPI but I was stuggling in configuring it so out of frustration manual developed the driver. Its a temporary and testing solution.
I do have plans to used Graphics library to create animations. The fonts.h file is not prefect but gets job done.

In this project I am using NTP Server to get the time data using my network.

WeatherAPI.com was used to get weather data. https://www.weatherapi.com/

![V1](https://github.com/user-attachments/assets/b389e814-ec45-466e-8340-74e36d75b2b3)

In V1.1 an alarm clock was added. I had a an audio .wav file uploaded in the SD card. The audio file was edited to be 8-bit PCM format using Audacity, to make the software side in the ESP32 easy. Initally the audio quality was terrible and the music was too fast but the a microsecond delay was calculated using the sampling rate and add to slow down the looping process to make the sample audible. Bandpass Filter was added to make the audio clear, the alpha and gain values were not calculated just pure trial and error. The audio quality is not perfect due to low sampling rate and filtering but for a 30 sec audio twice is only gonna be playing twice in 24hrs, its good enough.
As far as hardware goes the CYD had an integrated amplifier and I am using the 8-bit internal DAC (One of the main reasons for poor quality). The speaker I used was from a 2nd gen Amazon dot. The dot was damaged and not working so I savaged parts and got the speaker and using it with my CYD.

This where the Arduino IDE version stops, mainly due to restrictions in GUI, but it is the easiest to work with. Future versions will be developed in ESP-IDF in Visual Studio.

# CYD_TimeWise V2
The current goal is to do all the functionality that I had hoped to to do but was limited by the graphics driver. Since this version involves alot of images and animations, the firmware reqiores alot more flash memory, therefore the memory partition was changed. 
Inside the ESP_IDF folder you can find the imgae resources used for the project and all the contains that are currently in the SD card.
For Background images and Icons I have edited each image in Canva for size. Then used https://longfangsong.github.io/en/image-to-rgb565/ website to convert the images from PNG to RGB565 BIN file. The file is then loaded in SD card and as per usage the background changes. 
I have an astrobody as well which will be floating across the screen and that is converted into a C array.
Right now the images are being displayed. But further work is still underway.
