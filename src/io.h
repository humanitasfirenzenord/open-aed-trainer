#pragma once
#include <Arduino.h>

namespace IO {

enum class Led : uint8_t { Power, Shock, Pads, Cpr, COUNT };
enum class Pattern : uint8_t { Off, On, Slow, Fast, Metronome };

void begin();
void update();  // call every loop

// Buttons: true once per press (debounced, falling edge)
bool powerPressed();
bool shockPressed();
bool shockLongPressed();  // held > 2 s, reported once

// Pad connector, with an optional override from the web UI
bool padsConnected();
bool padsPhysical();
void setPadsVirtual(int8_t v);  // -1 = follow connector, 0 = off, 1 = on
int8_t padsVirtual();

// Old language switch, now semi (false) / fully automatic (true)
bool hasModeSwitch();
bool modeSwitchAuto();

// Volume pot: returns true when it moved, value in 0-100
bool volumeChanged(uint8_t &percent);

void setLed(Led led, Pattern p);
Pattern ledPattern(Led led);
void setMetronomeBpm(uint16_t bpm);

}  // namespace IO
