// OpenAEDTrainer – open source AED trainer firmware, for a trainer built from
// scratch or to replace the broken board of an existing one.
// Hardware: Ai-Thinker ESP32 Audio Kit v2.2 (ESP32-A1S, ES8388).
#include <Arduino.h>
#include <Preferences.h>
#include <Wire.h>

#include "aed.h"
#include "audio.h"
#include "io.h"
#include "pins.h"
#include "power.h"
#include "scenarios.h"
#include "web.h"

static const char *WIFI_PASSWORD = "";  // min 8 chars, "" = open network

static Preferences prefs;
static uint8_t volume;

static void setVolume(uint8_t v, bool save) {
  volume = v;
  Audio::setVolume(v);
  Web::setVolume(v);
  if (save) prefs.putUChar("volume", v);
}

static void handleCommand(const Web::Command &c) {
  using Web::Cmd;
  Power::activity();
  switch (c.cmd) {
    case Cmd::Power: Aed::power(); break;
    case Cmd::Shock: Aed::shockButton(); break;
    case Cmd::Skip: Aed::skip(); break;
    case Cmd::Pads: IO::setPadsVirtual(c.value); break;
    case Cmd::Scenario:
      Aed::selectScenario(c.value);
      prefs.putUChar("scenario", c.value);
      break;
    case Cmd::Auto:
      Aed::setAutoMode(c.value, true);
      prefs.putBool("auto", c.value);
      break;
    case Cmd::Volume: setVolume(constrain(c.value, 0, 100), true); break;
    case Cmd::Force: Aed::forceNextRhythm(c.value); break;
    case Cmd::Motion: Aed::injectMotion(); break;
    case Cmd::PoorPads: Aed::injectPoorPads(); break;
    case Cmd::LowBattery: Aed::injectLowBattery(); break;
  }
}

// Serial console for bench testing, same commands as the web UI:
//   p power   s shock   k skip   m motion   c poor contact   b low battery
//   d0/d1/d- pads off/on/connector   f0/f1/f- force rhythm   a0/a1 mode
//   n<0-9> scenario   v<0-100> volume   ? status
static void serialConsole() {
  static String line;
  while (Serial.available()) {
    char ch = Serial.read();
    if (ch != '\n' && ch != '\r') {
      line += ch;
      continue;
    }
    line.trim();
    if (line.isEmpty()) continue;
    using Web::Cmd;
    char c = line[0];
    String arg = line.substring(1);
    int16_t val = arg == "-" ? -1 : arg.toInt();
    Web::Command cmd{Cmd::Power, val};
    bool ok = true;
    switch (c) {
      case 'p': cmd.cmd = Cmd::Power; break;
      case 's': cmd.cmd = Cmd::Shock; break;
      case 'k': cmd.cmd = Cmd::Skip; break;
      case 'm': cmd.cmd = Cmd::Motion; break;
      case 'c': cmd.cmd = Cmd::PoorPads; break;
      case 'b': cmd.cmd = Cmd::LowBattery; break;
      case 'd': cmd.cmd = Cmd::Pads; break;
      case 'f': cmd.cmd = Cmd::Force; break;
      case 'a': cmd.cmd = Cmd::Auto; break;
      case 'n': cmd.cmd = Cmd::Scenario; cmd.value = val - 1; break;
      case 'v': cmd.cmd = Cmd::Volume; break;
      case '?': {
        auto s = Aed::status();
        Serial.printf("state=%s shocks=%u analyses=%u pads=%d auto=%d scenario=%u timer=%ld vol=%u\n",
                      s.label, s.shocks, s.analyses, s.pads, s.autoMode, s.scenario + 1, (long)s.timer, volume);
        ok = false;
        break;
      }
      default: ok = false; Serial.println("? unknown command");
    }
    if (ok) handleCommand(cmd);
    line = "";
  }
}

static void i2cProbe() {
  // The A436 board revision should answer at 0x10 (ES8388). 0x1A = AC101 board.
  Wire.begin(33, 32);
  for (uint8_t addr : {0x10, 0x11, 0x1A}) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) log_i("I2C device at 0x%02X%s", addr, addr == 0x1A ? " (AC101? wrong board profile!)" : "");
  }
  Wire.end();
}

void setup() {
  Serial.begin(115200);
  Power::Wake wake = Power::begin();
  log_i("OpenAEDTrainer starting (wake: %d)", (int)wake);
  prefs.begin("dae");  // NVS namespace kept: renaming it would reset saved settings

  i2cProbe();
  Audio::begin();  // first: the board driver reconfigures the key GPIOs
  IO::begin();

  bool autoMode = IO::hasModeSwitch() ? false : prefs.getBool("auto", false);
  Aed::begin(prefs.getUChar("scenario", 0), autoMode);
  Aed::onLog([](const String &l) { Web::log(l); });

  char ssid[32];
  snprintf(ssid, sizeof ssid, "OpenAEDTrainer-%04X", (uint16_t)(ESP.getEfuseMac() >> 32));
  Web::begin(ssid, WIFI_PASSWORD);
  setVolume(prefs.getUChar("volume", 70), false);

  // Let the switch settle before reading it
  for (int i = 0; i < 20; i++) {
    IO::update();
    delay(10);
  }
  if (IO::hasModeSwitch()) Aed::setAutoMode(IO::modeSwitchAuto(), false);

  // Woken by POWER: switch the trainer on straight away. Otherwise (SHOCK,
  // first boot) stay in standby with WiFi up and beep as feedback.
  if (wake == Power::Wake::PowerButton) Aed::power();
  else Audio::play("sfx/beep");
}

void loop() {
  IO::update();

  if (IO::powerPressed()) {
    Power::activity();
    bool wasOn = Aed::status().on;
    Aed::power();
    if (wasOn) Power::sleepWhenQuiet();  // off with the button = fully off
  }
  if (IO::shockPressed()) {
    Power::activity();
    Aed::shockButton();
  }
  static bool lastPads = IO::padsConnected();
  if (IO::padsConnected() != lastPads) {
    lastPads = IO::padsConnected();
    Power::activity();
  }
  if (IO::shockLongPressed()) {
    Aed::shockLongPress();
    prefs.putUChar("scenario", Aed::status().scenario);
  }

  if (IO::hasModeSwitch()) {
    static bool lastMode = IO::modeSwitchAuto();
    if (IO::modeSwitchAuto() != lastMode) {
      lastMode = IO::modeSwitchAuto();
      Aed::setAutoMode(lastMode, true);
    }
  }

  uint8_t v;
  if (IO::volumeChanged(v)) setVolume(v, false);

  Web::Command cmd;
  while (Web::nextCommand(cmd)) handleCommand(cmd);
  serialConsole();

  Aed::update();
  Web::update();
  Power::update(Aed::status().on);
  delay(5);
}
