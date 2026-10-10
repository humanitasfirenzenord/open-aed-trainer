#pragma once
#include <Arduino.h>

// Minimal bit-banged TM1637 driver for the trainer's single-digit 7-segment
// display (the TM1637 drives it on its first digit output, GRID1).
// Disabled when PIN_TM1637_CLK is -1.
namespace Display {

void begin();
void clear();
// One char (only the first digit is fitted): 0-9, A-Z (best effort), '-',
// '_', ' '. A '.' after it lights the decimal point, e.g. "8." or "3.".
void show(const char *text);
void showSegments(uint8_t segs);    // raw: bit 0 = a ... bit 6 = g, bit 7 = dp
void setBrightness(uint8_t level);  // 0-7

}  // namespace Display
