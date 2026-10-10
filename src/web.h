#pragma once
#include <Arduino.h>

// WiFi access point + instructor web UI (WebSocket, JSON).
// Commands from the browser are queued and executed by the main loop.
namespace Web {

enum class Cmd : uint8_t {
  Power, Shock, Skip, Pads, Scenario, Auto, Volume, Force, Motion, PoorPads, LowBattery,
};

struct Command {
  Cmd cmd;
  int16_t value;
};

void begin(const char *ssid, const char *password);
void update();                    // DNS + periodic state push
bool nextCommand(Command &out);   // pop one queued command
void log(const String &line);     // broadcast an event-log line
void setVolume(uint8_t percent);  // included in state messages
String ip();

}  // namespace Web
