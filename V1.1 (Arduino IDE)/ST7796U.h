#ifndef ST7796U_H
#define ST7796U_H

#include <Arduino.h>

#define LCD_CS   15
#define LCD_DC   2
#define LCD_RST  3
#define LCD_BL   27
#define LCD_SCK  14
#define LCD_MOSI 13

#define LCD_WIDTH  320
#define LCD_HEIGHT 480

class ST7796U {
    public:
        ST7796U(uint8_t cs, uint8_t dc, uint8_t rst, uint8_t bl, uint8_t sck, uint8_t mosi);
        void begin();
        void fillScreen(uint16_t color);
        void printText(uint16_t x, uint16_t y, const char* str, uint16_t color, uint16_t bg);

    private:
        uint8_t _cs, _dc, _rst, _bl, _sck, _mosi;

    void writeCommand(uint8_t cmd);
    void writeData(uint8_t data);
    void sendData(const uint8_t *data, size_t len);
    void reset();
    void initDisplay();
    void setAddrWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);
    void drawPixel(uint16_t x, uint16_t y, uint16_t color);
    void drawChar(uint16_t x, uint16_t y, char c, uint16_t color, uint16_t bg);
};

#endif
