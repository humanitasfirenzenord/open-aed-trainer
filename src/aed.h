#pragma once
#include <Arduino.h>

#include <functional>

// Simulated AED: power on -> pads -> analysis -> (charge -> shock) -> CPR -> ...
namespace Aed {

enum class State : uint8_t {
  Off,
  PowerOn,    // power-on tone + self test
  WaitPads,   // "apply pads / plug connector", repeated
  PadsCheck,  // poor contact fault: "check pads" until the connector is re-plugged
  Analyze,    // "do not touch the patient, analysing"
  Motion,     // motion artefact during analysis -> analyse again
  Charge,     // "shock advised, charging, stand clear"
  Armed,      // semi-auto: shock button flashing, waiting for press
  AutoShock,  // fully-auto: "shock in 3, 2, 1"
  Shock,      // "shock delivered"
  Disarm,     // button not pressed in time -> internal discharge
  NoShock,    // "no shock advised"
  Cpr,        // 2 minutes of CPR with metronome
  CprEnd,     // "stop CPR" -> analyse again
};

struct Status {
  bool on;
  State state;
  const char *label;  // Italian, for the UI
  String detail;
  uint8_t scenario;
  uint16_t shocks;
  uint16_t analyses;
  uint32_t elapsed;   // seconds since power on
  int32_t timer;      // seconds, -1 = none
  const char *timerLabel;
  bool armed;
  bool pads;
  int8_t padsVirtual;
  bool autoMode;
  int8_t forced;      // -1 none, 0 next analysis forced non-shockable, 1 shockable
};

void begin(uint8_t scenario, bool autoMode);
void update();  // call every loop

// Controls (call from the main loop only)
void power();
void shockButton();
void shockLongPress();  // off: next scenario
void skip();  // instructor: end the current wait (CPR, pads check...)
void selectScenario(uint8_t i);
void setAutoMode(bool on, bool announce);
void forceNextRhythm(int8_t shockable);  // -1 clears
void injectMotion();
void injectPoorPads();
void injectLowBattery();

Status status();
uint32_t revision();  // changes whenever the status changes meaningfully
void onLog(std::function<void(const String &)> cb);

}  // namespace Aed
