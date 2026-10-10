#pragma once
#include <Arduino.h>

// Battery saving: deep sleep when the trainer is off and idle.
// POWER or SHOCK wake it up again (a wake-up is a fresh boot).
namespace Power {

enum class Wake : uint8_t { ColdBoot, PowerButton, ShockButton, Other };

Wake begin();          // call first in setup(): why did we boot?
void activity();       // any user input: postpones the timeouts
void sleepWhenQuiet(); // sleep as soon as the last sound has played
void update(bool trainerOn);  // timeouts; may never return (deep sleep)

}  // namespace Power
