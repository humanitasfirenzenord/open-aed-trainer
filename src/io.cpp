#include "io.h"

#include "pins.h"

namespace IO {
namespace {

struct Button {
  Button(int p, bool momentary) : pin(p), momentary(momentary), armed(!momentary) {}
  int pin;
  bool momentary;  // push button; false = level input (pad connector, switch)
  bool stable = false;  // pressed?
  bool lastRaw = false;
  uint32_t changedAt = 0;
  uint32_t pressedAt = 0;
  bool pressEvent = false;
  bool longEvent = false;
  bool longFired = false;
  // No events until the button has been seen released: the one that woke us
  // from deep sleep may still be held, and an input stuck at GND (wiring,
  // DIP switches) must not fire presses and long presses on its own.
  bool armed;
  bool stuckLogged = false;

  void begin() {
    if (pin < 0) return;
    // 34-39 have no internal pulls; the Audio Kit has 10k pull-ups on its keys
    pinMode(pin, pin >= 34 ? INPUT : INPUT_PULLUP);
    delayMicroseconds(50);
    // Buttons count as held until update() sees them released
    stable = lastRaw = momentary || digitalRead(pin) == LOW;
  }
  void update(uint32_t now, uint16_t debounceMs = 30) {
    if (pin < 0) return;
    bool raw = digitalRead(pin) == LOW;
    if (raw != lastRaw) {
      lastRaw = raw;
      changedAt = now;
    }
    if (!armed) {
      if (!raw && now - changedAt >= debounceMs) {
        armed = true;
        stable = false;
      } else if (raw && !stuckLogged && now > 5000) {
        stuckLogged = true;
        log_w("GPIO%d held LOW since boot: button stuck or input wired to GND? Ignored until released", pin);
      }
      return;
    }
    if (raw != stable && now - changedAt >= debounceMs) {
      stable = raw;
      log_d("GPIO%d %s", pin, stable ? "LOW (active)" : "HIGH");
      if (stable) {
        pressEvent = true;
        pressedAt = now;
        longFired = false;
      }
    }
    if (stable && !longFired && now - pressedAt > 2000) {
      longFired = true;
      longEvent = true;
    }
  }
  bool take(bool &flag) {
    bool f = flag;
    flag = false;
    return f;
  }
};

Button power(PIN_BTN_POWER, true);
Button power2(PIN_BTN_POWER2, true);
Button shock(PIN_BTN_SHOCK, true);
Button pads(PIN_PADS_DETECT, false);
Button mode(PIN_MODE_SWITCH, false);

int8_t padsOverride = -1;

const int ledPins[] = {PIN_LED_POWER, PIN_LED_SHOCK, PIN_LED_PADS, PIN_LED_CPR};
Pattern ledPat[(int)Led::COUNT] = {};
uint16_t metronomeMs = 60000 / 110;
uint32_t metronomeStart = 0;

int potLast = -100;

bool patternLevel(Pattern p, uint32_t now) {
  switch (p) {
    case Pattern::On: return true;
    case Pattern::Slow: return (now / 500) % 2 == 0;
    case Pattern::Fast: return (now / 150) % 2 == 0;
    case Pattern::Metronome: return (now - metronomeStart) % metronomeMs < 120;
    default: return false;
  }
}

}  // namespace

void begin() {
  power.begin();
  power2.begin();
  shock.begin();
  pads.begin();
  mode.begin();
  for (int p : ledPins)
    if (p >= 0) {
      pinMode(p, OUTPUT);
      digitalWrite(p, LOW);
    }
#if PIN_VOLUME_POT >= 0
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_VOLUME_POT, ADC_11db);
#endif
}

void update() {
  uint32_t now = millis();
  power.update(now);
  power2.update(now);
  shock.update(now);
  pads.update(now, 150);  // connector contacts bounce a lot
  mode.update(now, 100);
  for (int i = 0; i < (int)Led::COUNT; i++)
    if (ledPins[i] >= 0) digitalWrite(ledPins[i], patternLevel(ledPat[i], now));
}

bool powerPressed() {
  bool a = power.take(power.pressEvent), b = power2.take(power2.pressEvent);
  return a || b;
}
bool shockPressed() { return shock.take(shock.pressEvent); }
bool shockLongPressed() { return shock.take(shock.longEvent); }

bool padsPhysical() { return pads.stable; }
bool padsConnected() { return padsOverride >= 0 ? padsOverride : pads.stable; }
void setPadsVirtual(int8_t v) { padsOverride = v; }
int8_t padsVirtual() { return padsOverride; }

bool hasModeSwitch() { return PIN_MODE_SWITCH >= 0; }
bool modeSwitchAuto() { return mode.stable; }

bool volumeChanged(uint8_t &percent) {
#if PIN_VOLUME_POT >= 0
  static uint32_t lastRead = 0;
  if (millis() - lastRead < 50) return false;
  lastRead = millis();
  // Light smoothing, then a dead band so noise doesn't spam updates
  static float avg = analogRead(PIN_VOLUME_POT);
  avg = avg * 0.8f + analogRead(PIN_VOLUME_POT) * 0.2f;
  int v = constrain(map((int)avg, 80, 4000, 0, 100), 0, 100);
  if (abs(v - potLast) < 3) return false;
  potLast = v;
  percent = v;
  return true;
#else
  (void)percent;
  return false;
#endif
}

void setLed(Led led, Pattern p) {
  if (p == Pattern::Metronome && ledPat[(int)led] != Pattern::Metronome) metronomeStart = millis();
  ledPat[(int)led] = p;
}

Pattern ledPattern(Led led) { return ledPat[(int)led]; }

void setMetronomeBpm(uint16_t bpm) { metronomeMs = 60000 / bpm; }

}  // namespace IO
