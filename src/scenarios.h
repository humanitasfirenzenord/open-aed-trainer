#pragma once
#include <Arduino.h>

// A training scenario only decides *what the virtual patient does*; the AED
// behaviour itself (prompts, charging, CPR...) lives in the state machine.
//
// To add a scenario, append a line to the table in scenarios.cpp.
struct Scenario {
  const char *name;  // shown in the web UI
  const char *desc;

  // Result of each rhythm analysis, in order: 'S' shockable (VF/pVT),
  // 'N' non-shockable (asystole/PEA/ROSC). The last letter repeats forever.
  const char *rhythm;

  // Faults injected before the given analysis (1 = first). 0 = never.
  uint8_t poorPadsAt;  // "check pads" until the connector is re-plugged
  uint8_t motionAt;    // motion artefact interrupts the analysis

  bool lowBattery;      // "battery low" prompt after power on
  uint8_t cprCycles;    // 30:2 cycles of CPR between analyses
};

namespace Scenarios {
size_t count();
const Scenario &get(size_t i);
// Is analysis number n (1-based) shockable in scenario s?
bool shockable(const Scenario &s, uint16_t n);
}  // namespace Scenarios
