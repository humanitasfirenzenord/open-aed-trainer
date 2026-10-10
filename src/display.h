#pragma once
#include <Arduino.h>

// Minimal bit-banged TM1637 driver for a 4-digit 7-segment module.
// Disabled when PIN_TM1637_CLK is -1.
namespace Display {

void begin();
void clear();
// Up to 4 chars: 0-9, A-Z (best effort), '-', '_', ' '. A ':' after the
// 2nd char lights the colon, e.g. "1:59" or " 2:00".
void show(const char *text);
void setBrightness(uint8_t level);  // 0-7

}  // namespace Display
