#include "display.h"

#include "pins.h"

namespace Display {
namespace {

constexpr int CLK = PIN_TM1637_CLK;
constexpr int DIO = PIN_TM1637_DIO;
uint8_t brightness = 5;
uint8_t lastSegs[4] = {0xFF, 0xFF, 0xFF, 0xFF};

//   -a-
//  f   b
//   -g-
//  e   c
//   -d-
uint8_t glyph(char c) {
  static const uint8_t digits[] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};
  if (c >= '0' && c <= '9') return digits[c - '0'];
  switch (toupper(c)) {
    case 'A': return 0x77; case 'B': return 0x7C; case 'C': return 0x39;
    case 'D': return 0x5E; case 'E': return 0x79; case 'F': return 0x71;
    case 'G': return 0x3D; case 'H': return 0x76; case 'I': return 0x06;
    case 'J': return 0x1E; case 'L': return 0x38; case 'N': return 0x54;
    case 'O': return 0x5C; case 'P': return 0x73; case 'R': return 0x50;
    case 'S': return 0x6D; case 'T': return 0x78; case 'U': return 0x3E;
    case 'Y': return 0x6E; case '-': return 0x40; case '_': return 0x08;
    default: return 0x00;
  }
}

inline void bitDelay() { delayMicroseconds(5); }
// Open-drain style: the module has its own pull-ups
inline void clkHigh() { pinMode(CLK, INPUT); }
inline void clkLow() { pinMode(CLK, OUTPUT); digitalWrite(CLK, LOW); }
inline void dioHigh() { pinMode(DIO, INPUT); }
inline void dioLow() { pinMode(DIO, OUTPUT); digitalWrite(DIO, LOW); }

void start() { dioLow(); bitDelay(); }
void stop() { dioLow(); bitDelay(); clkHigh(); bitDelay(); dioHigh(); bitDelay(); }

void writeByte(uint8_t b) {
  for (int i = 0; i < 8; i++) {
    clkLow();
    (b & 1) ? dioHigh() : dioLow();
    bitDelay();
    clkHigh();
    bitDelay();
    b >>= 1;
  }
  clkLow();  // ACK clock
  dioHigh();
  bitDelay();
  clkHigh();
  bitDelay();
  clkLow();
  bitDelay();
}

void writeSegs(const uint8_t segs[4]) {
  if (CLK < 0) return;
  if (memcmp(segs, lastSegs, 4) == 0) return;
  memcpy(lastSegs, segs, 4);
  start(); writeByte(0x40); stop();         // auto-increment address
  start(); writeByte(0xC0);                 // from digit 0
  for (int i = 0; i < 4; i++) writeByte(segs[i]);
  stop();
  start(); writeByte(0x88 | (brightness & 7)); stop();  // display on
}

}  // namespace

void begin() {
  if (CLK < 0) return;
  clkHigh();
  dioHigh();
  clear();
}

void clear() {
  const uint8_t z[4] = {0, 0, 0, 0};
  writeSegs(z);
}

void show(const char *text) {
  uint8_t segs[4] = {0, 0, 0, 0};
  int d = 0;
  for (const char *p = text; *p && d < 4; p++) {
    if (*p == ':' || *p == '.') {
      if (d > 0) segs[d - 1] |= 0x80;  // colon/dp is bit 7 of the previous digit
      continue;
    }
    segs[d++] = glyph(*p);
  }
  writeSegs(segs);
}

void showSegments(uint8_t s) {
  const uint8_t segs[4] = {s, 0, 0, 0};
  writeSegs(segs);
}

void setBrightness(uint8_t level) {
  brightness = level & 7;
  memset(lastSegs, 0xFF, 4);  // force refresh
}

}  // namespace Display
