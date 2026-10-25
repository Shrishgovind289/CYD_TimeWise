#include "ST7796U.h"
#include "fonts.h"       // your 5x8 font array as in your working code
#include <SPI.h>

SPIClass spi(VSPI);

ST7796U::ST7796U(uint8_t cs, uint8_t dc, uint8_t rst, uint8_t bl, uint8_t sck, uint8_t mosi)
  : _cs(cs), _dc(dc), _rst(rst), _bl(bl), _sck(sck), _mosi(mosi) {}

void ST7796U::writeCommand(uint8_t cmd) {
  digitalWrite(_dc, LOW);
  digitalWrite(_cs, LOW);
  spi.transfer(cmd);
  digitalWrite(_cs, HIGH);
}

void ST7796U::writeData(uint8_t data) {
  digitalWrite(_dc, HIGH);
  digitalWrite(_cs, LOW);
  spi.transfer(data);
  digitalWrite(_cs, HIGH);
}

void ST7796U::sendData(const uint8_t *data, size_t len) {
  digitalWrite(_dc, HIGH);
  digitalWrite(_cs, LOW);
  for(size_t i = 0; i < len; i++) {
    spi.transfer(data[i]);
  }
  digitalWrite(_cs, HIGH);
}

void ST7796U::reset() {
  digitalWrite(_rst, LOW);
  delay(50);
  digitalWrite(_rst, HIGH);
  delay(50);
}

void ST7796U::initDisplay() {
  spi.begin(_sck, -1, _mosi, _cs);

  pinMode(_cs, OUTPUT);
  pinMode(_dc, OUTPUT);
  pinMode(_rst, OUTPUT);
  pinMode(_bl, OUTPUT);
  digitalWrite(_bl, HIGH); // backlight on

  reset();

  writeCommand(0x11); delay(100); // Sleep Out
  writeCommand(0x36); writeData(0x48); // Memory Access Control (MX, BGR)
  writeCommand(0x3A); writeData(0x55); // Interface Pixel Format (16-bit/pixel)
  writeCommand(0x29); delay(10);  // Display ON
}

void ST7796U::begin() {
  initDisplay();
}

void ST7796U::setAddrWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
  writeCommand(0x2A); // Column Address Set
  writeData(x0 >> 8); writeData(x0 & 0xFF);
  writeData(x1 >> 8); writeData(x1 & 0xFF);

  writeCommand(0x2B); // Row Address Set
  writeData(y0 >> 8); writeData(y0 & 0xFF);
  writeData(y1 >> 8); writeData(y1 & 0xFF);

  writeCommand(0x2C); // Memory Write
}

void ST7796U::fillScreen(uint16_t color) {
  setAddrWindow(0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1);

  digitalWrite(_dc, HIGH);
  digitalWrite(_cs, LOW);

  // Send color as two bytes, MSB then LSB, for every pixel
  uint8_t highByte = color >> 8;
  uint8_t lowByte = color & 0xFF;

  for (uint32_t i = 0; i < (uint32_t)LCD_WIDTH * LCD_HEIGHT; i++) {
    spi.transfer(highByte);
    spi.transfer(lowByte);
  }

  digitalWrite(_cs, HIGH);
}

void ST7796U::drawPixel(uint16_t x, uint16_t y, uint16_t color) {
  setAddrWindow(x, y, x, y);

  digitalWrite(_dc, HIGH);
  digitalWrite(_cs, LOW);

  spi.transfer(color >> 8);
  spi.transfer(color & 0xFF);

  digitalWrite(_cs, HIGH);
}

void ST7796U::drawChar(uint16_t x, uint16_t y, char c, uint16_t color, uint16_t bg) {
  if(c < 32 || c > 126) return;
  const char* chr = font[c - 32];

  for (uint8_t i = 0; i < 5; i++) {
    uint8_t line = chr[i];
    for (uint8_t j = 0; j < 8; j++) {
      drawPixel(x + i, y + j, (line & 0x01) ? color : bg);
      line >>= 1;
    }
  }
}

void ST7796U::printText(uint16_t x, uint16_t y, const char* str, uint16_t color, uint16_t bg) {
  while (*str) {
    drawChar(x, y, *str, color, bg);
    x += 6;
    str++;
  }
}

