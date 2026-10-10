#include "aed.h"

#include "audio.h"
#include "display.h"
#include "io.h"
#include "scenarios.h"

using IO::Led;
using IO::Pattern;

namespace Aed {
namespace {

// Timings (ms). Real AEDs: analysis < 10 s, charge 6-12 s, shock button
// disarms after ~30 s, pad prompts repeat every ~8 s.
constexpr uint32_t ANALYSIS_MS = 8000;
constexpr uint32_t CHARGE_MS = 6000;  // minimum, silent after the prompts
constexpr uint32_t PROMPT_GAP_MS = 1000;  // between consecutive prompts
constexpr uint32_t MOTION_AFTER_MS = 2500;
// Armed: one prompt, then a continuous tone until the shock button is
// pressed. No reminders, no automatic disarm (the instructor can "skip").
constexpr uint16_t ARMED_TONE_HZ = 1200;
constexpr uint32_t PADS_REPEAT_MS = 8000;
constexpr uint32_t SELFTEST_MS = 1000;
constexpr uint32_t POWERON_PAUSE_MS = 1000;  // after the power-on tone

State state = State::Off;
uint32_t stateAt = 0;    // when the current state was entered
uint32_t idleSince = 0;  // when the last one-shot prompt finished
uint32_t poweredAt = 0;
uint32_t scenarioShownAt = 0;
uint32_t rev = 1;

uint8_t scenarioIdx = 0;
bool autoMode = false;
uint16_t shocks = 0;
uint16_t analyses = 0;
bool reanalyse = false;    // re-entering Analyze after motion: same analysis number
bool padsWereOn = false;   // pads have been connected at least once this session
bool cprLoopStarted = false;

// Faults still to happen in this session (analysis number, 0 = none)
uint8_t poorPadsAt = 0;
uint8_t motionAt = 0;
int8_t forced = -1;

std::function<void(const String &)> logCb;

const Scenario &sc() { return Scenarios::get(scenarioIdx); }

String v(const char *id) { return Audio::voice(id); }
void say(const char *id) { Audio::play(v(id).c_str()); }
void sfx(const char *id) { Audio::play((String("sfx/") + id).c_str()); }

uint32_t since(uint32_t t) { return millis() - t; }

void log(const String &msg) {
  uint32_t s = state == State::Off ? 0 : since(poweredAt) / 1000;
  char ts[12];
  snprintf(ts, sizeof ts, "[%02lu:%02lu] ", (unsigned long)(s / 60), (unsigned long)(s % 60));
  String line = String(ts) + msg;
  log_i("%s", line.c_str());
  if (logCb) logCb(line);
}

const char *label(State s) {
  switch (s) {
    case State::Off: return "Spento";
    case State::PowerOn: return "Accensione";
    case State::WaitPads: return "Applicare gli elettrodi";
    case State::PadsCheck: return "Controllare gli elettrodi";
    case State::Analyze: return "Analisi del ritmo";
    case State::Motion: return "Movimento rilevato";
    case State::Charge: return "Shock consigliato – carica";
    case State::Armed: return "Premere il pulsante SHOCK";
    case State::AutoShock: return "Shock automatico";
    case State::Shock: return "Shock erogato";
    case State::Disarm: return "Shock annullato";
    case State::NoShock: return "Shock non consigliato";
    case State::Cpr: return "RCP in corso";
    case State::CprEnd: return "Fine RCP";
  }
  return "";
}

// The instructor's "force next rhythm" survives power-on on purpose: it is
// usually set up before starting. It is cleared once an analysis uses it.
void resetFaults() {
  poorPadsAt = sc().poorPadsAt;
  motionAt = sc().motionAt;
}

void enter(State s);

// Current 30:2 cycle (1-based) while in CPR
uint32_t cprCycle() { return min<uint32_t>(Audio::loopsCompleted() + 1, sc().cprCycles); }

// After the pads are (re)connected: fault check or straight to analysis
void padsReady() {
  IO::setLed(Led::Pads, Pattern::Off);
  padsWereOn = true;
  if (poorPadsAt && poorPadsAt <= analyses + 1)
    enter(State::PadsCheck);
  else
    enter(State::Analyze);
}

void enter(State s) {
  State prev = state;
  uint32_t prevAt = stateAt;
  if (s == State::Off && prev != State::Off) log("Spento");
  state = s;
  stateAt = millis();
  rev++;

  // Leaving armed/charging without a shock: the shock light and tone go off
  if (s != State::Armed && s != State::AutoShock) IO::setLed(Led::Shock, Pattern::Off);
  if (s != State::Armed) Audio::tone(0);
  if (s != State::Cpr) IO::setLed(Led::Cpr, Pattern::Off);

  switch (s) {
    case State::Off:
      Audio::stop();
      if (prev != State::Off) sfx("power_off");
      Audio::keepAmplifierOn(false);  // off 1 s after the tone has finished
      for (int i = 0; i < (int)Led::COUNT; i++) IO::setLed((Led)i, Pattern::Off);
      Display::clear();
      return;

    case State::PowerOn:
      poweredAt = millis();
      shocks = analyses = 0;
      padsWereOn = false;
      reanalyse = false;
      resetFaults();
      Audio::stop();
      Audio::keepAmplifierOn(true);  // the first sound waits 1 s for the amp
      IO::setLed(Led::Power, Pattern::On);
      log(String("Acceso – scenario ") + (scenarioIdx + 1) + ": " + sc().name);
      sfx("power_on");
      if (sc().lowBattery) {
        sfx("alert");
        say("low_battery");
      }
      break;

    case State::WaitPads:
      Audio::stop();
      IO::setLed(Led::Pads, Pattern::Fast);
      if (padsWereOn) {
        sfx("alert");
        say("check_pads");
        say("plug_connector");
      } else {
        say("expose_chest");
        say("apply_pads");
        say("plug_connector");
      }
      log(padsWereOn ? "Elettrodi scollegati" : "In attesa degli elettrodi");
      break;

    case State::PadsCheck:
      Audio::stop();
      IO::setLed(Led::Pads, Pattern::Slow);
      sfx("alert");
      say("check_pads");
      log("Contatto elettrodi scarso – reinserire il connettore");
      break;

    case State::Analyze:
      if (!reanalyse) analyses++;
      reanalyse = false;
      Audio::stop();
      say("analysing");
      log(String("Analisi n. ") + analyses);
      break;

    case State::Motion:
      Audio::stop();
      sfx("alert");
      say("motion");
      log("Movimento rilevato durante l'analisi");
      break;

    case State::Charge:
      say("shock_advised");
      Audio::pause(PROMPT_GAP_MS);
      say("charging");
      Audio::pause(PROMPT_GAP_MS);
      say("charging");
      log("Shock consigliato – carica in corso");
      break;

    case State::Armed:
      // Prompt, then a continuous tone and the shock button light until the
      // button is pressed (see update()). Never disarms on its own.
      say("press_shock");
      Audio::tone(ARMED_TONE_HZ);
      log("Pronto allo shock – attesa pulsante");
      break;

    case State::AutoShock:
      IO::setLed(Led::Shock, Pattern::Fast);
      say("auto_shock");
      log("Shock automatico in 3, 2, 1");
      break;

    case State::Shock:
      shocks++;
      Audio::stop();
      sfx("shock");
      say("shock_delivered");
      log(String("Shock n. ") + shocks + " erogato" +
          (prev == State::Armed ? String(" dopo ") + since(prevAt) / 1000 + " s di attesa" : ""));
      break;

    case State::Disarm:
      Audio::stop();
      sfx("disarm");
      say("shock_not_pressed");
      say("shock_cancelled");
      log("Shock annullato dall'istruttore");
      break;

    case State::NoShock:
      say("no_shock");
      Audio::pause(PROMPT_GAP_MS);
      say("continue_cpr");
      log("Shock non consigliato");
      break;

    case State::Cpr:
      cprLoopStarted = false;
      // After "no shock" the trainee was already told "Riprendere RCP"
      if (prev != State::NoShock) say("safe_touch");
      Audio::loop(v("cpr_cycle").c_str());
      log(String("RCP: ") + sc().cprCycles + " cicli 30:2");
      break;

    case State::CprEnd:
      // No "stop CPR" prompt: the analysis prompt ("Non toccare il
      // paziente...") that follows immediately is the cue to stop.
      Audio::stop();
      break;
  }
}

bool armedOrCharging() {
  return state == State::Charge || state == State::Armed || state == State::AutoShock;
}

void updateDisplay() {
  char buf[8];
  uint32_t t = since(stateAt);
  bool blink = (millis() / 400) % 2;
  if (since(scenarioShownAt) < 2000) {
    snprintf(buf, sizeof buf, "SC%2u", scenarioIdx + 1);
    Display::show(buf);
    return;
  }
  switch (state) {
    case State::Off:
      Display::clear();
      break;
    case State::PowerOn:
      Display::show(t < SELFTEST_MS ? "8.8.:8.8." : "----");
      break;
    case State::WaitPads:
    case State::PadsCheck:
      snprintf(buf, sizeof buf, "%c %2u", blink ? 'P' : ' ', scenarioIdx + 1);
      Display::show(buf);
      break;
    case State::Analyze:
    case State::Motion: {
      // rotating segment chase on the first digit, analysis number on the right
      snprintf(buf, sizeof buf, "A%c%2u", "-_ "[(millis() / 250) % 3], analyses);
      Display::show(buf);
      break;
    }
    case State::Charge:
    case State::Armed:
    case State::AutoShock:
      snprintf(buf, sizeof buf, "%s%2u", (state == State::Charge || blink) ? "S " : "  ", shocks + 1);
      Display::show(buf);
      break;
    case State::Cpr:
      // "C2-4": CPR cycle 2 of 4
      snprintf(buf, sizeof buf, "C%u-%u", min<uint32_t>(cprCycle(), 9), min<uint32_t>(sc().cprCycles, 9));
      Display::show(buf);
      break;
    default:
      snprintf(buf, sizeof buf, "S %2u", shocks);
      Display::show(buf);
      break;
  }
}

}  // namespace

void begin(uint8_t scenario, bool autoMd) {
  scenarioIdx = scenario < Scenarios::count() ? scenario : 0;
  autoMode = autoMd;
  state = State::Off;
  Display::clear();
}

void update() {
  uint32_t now = millis();
  bool busy = Audio::busy();
  if (busy) idleSince = now;
  uint32_t idle = busy ? 0 : now - idleSince;
  uint32_t t = since(stateAt);
  bool pads = IO::padsConnected();

  // Pads pulled off in the middle of a session
  if (!pads && padsWereOn) {
    switch (state) {
      case State::Analyze: case State::Motion: case State::Charge: case State::Armed:
      case State::AutoShock: case State::NoShock: case State::Cpr: case State::CprEnd:
      case State::Shock: case State::Disarm:
        if (armedOrCharging()) sfx("disarm");
        enter(State::WaitPads);
        return;
      default: break;
    }
  }

  switch (state) {
    case State::Off:
      break;

    case State::PowerOn:
      if (t > SELFTEST_MS && !busy && idle > POWERON_PAUSE_MS) {
        if (pads) padsReady();
        else enter(State::WaitPads);
      }
      break;

    case State::WaitPads:
      if (pads) {
        Audio::stop();
        log("Elettrodi collegati");
        padsReady();
      } else if (idle > PADS_REPEAT_MS) {
        say("apply_pads");
        say("plug_connector");
      }
      break;

    case State::PadsCheck:
      if (!pads) {
        // trainee re-seats the connector: fault resolved, ask to plug back in
        poorPadsAt = 0;
        enter(State::WaitPads);
      } else if (idle > PADS_REPEAT_MS) {
        say("check_pads");
      }
      break;

    case State::Analyze:
      if (motionAt && motionAt <= analyses && t > MOTION_AFTER_MS) {
        motionAt = 0;
        enter(State::Motion);
      } else if (t > ANALYSIS_MS && !busy) {
        bool shock = forced >= 0 ? forced == 1 : Scenarios::shockable(sc(), analyses);
        if (forced >= 0) log(String("Ritmo forzato dall'istruttore: ") + (forced ? "defibrillabile" : "non defibrillabile"));
        forced = -1;
        enter(shock ? State::Charge : State::NoShock);
      }
      break;

    case State::Motion:
      if (!busy) {
        reanalyse = true;
        enter(State::Analyze);
      }
      break;

    case State::Charge:
      if (t > CHARGE_MS && !busy) enter(autoMode ? State::AutoShock : State::Armed);
      break;

    case State::Armed:
      if (!busy && IO::ledPattern(Led::Shock) == Pattern::Off)
        IO::setLed(Led::Shock, Pattern::Fast);  // after the prompt
      break;

    case State::AutoShock:
      if (!busy) enter(State::Shock);
      break;

    case State::Shock:
    case State::Disarm:
    case State::NoShock:
      if (!busy) enter(State::Cpr);
      break;

    case State::Cpr:
      if (!cprLoopStarted && !busy) {
        cprLoopStarted = true;  // prompts done, metronome loop starts now
        IO::setLed(Led::Cpr, Pattern::Metronome);
      }
      // Analyse again after N complete 30:2 cycles (end of the breaths)
      if (Audio::loopsCompleted() >= sc().cprCycles) enter(State::CprEnd);
      break;

    case State::CprEnd:
      if (!busy) enter(State::Analyze);
      break;
  }

  static uint32_t lastDisp = 0;
  if (now - lastDisp > 100) {
    lastDisp = now;
    updateDisplay();
  }
}

void power() {
  if (state == State::Off) enter(State::PowerOn);
  else enter(State::Off);
}

void shockButton() {
  if (state == State::Armed) {
    enter(State::Shock);
  } else if (state != State::Off) {
    log("Pulsante SHOCK premuto fuori tempo");
  }
}

void shockLongPress() {
  // With the trainer off, holding SHOCK cycles through the scenarios so it
  // can be set up without the web UI. Never while on: a trainee pressing
  // early must not change the scenario.
  if (state != State::Off) return;
  selectScenario((scenarioIdx + 1) % Scenarios::count());
  sfx("beep");
}

void skip() {
  switch (state) {
    case State::Cpr: enter(State::CprEnd); break;
    case State::PadsCheck: poorPadsAt = 0; enter(State::Analyze); break;
    case State::Armed: enter(State::Disarm); break;
    default: break;
  }
}

void selectScenario(uint8_t i) {
  if (i >= Scenarios::count()) return;
  scenarioIdx = i;
  scenarioShownAt = millis();
  if (analyses == 0) resetFaults();
  log(String("Scenario ") + (i + 1) + ": " + sc().name);
  rev++;
}

void setAutoMode(bool on, bool announce) {
  if (on == autoMode) return;
  autoMode = on;
  rev++;
  log(on ? "Modalità automatica" : "Modalità semiautomatica");
  if (announce && state != State::Off) say(on ? "mode_auto" : "mode_semi");
}

void forceNextRhythm(int8_t s) {
  forced = s;
  rev++;
  if (s >= 0) log(String("Prossima analisi: ") + (s ? "defibrillabile" : "non defibrillabile"));
}

void injectMotion() {
  if (state == State::Analyze) {
    motionAt = 0;
    enter(State::Motion);
  } else {
    motionAt = analyses + 1;  // at the next analysis
    log("Movimento programmato alla prossima analisi");
  }
  rev++;
}

void injectPoorPads() {
  if (state == State::Off || state == State::PowerOn || state == State::WaitPads) {
    poorPadsAt = analyses + 1;
    log("Contatto scarso programmato");
  } else {
    if (armedOrCharging()) sfx("disarm");
    poorPadsAt = analyses + 1;
    enter(State::PadsCheck);
  }
  rev++;
}

void injectLowBattery() {
  if (state == State::Off) return;
  sfx("alert");
  say("low_battery");
  log("Batteria scarica");
}

Status status() {
  Status s{};
  s.on = state != State::Off;
  s.state = state;
  s.label = label(state);
  s.scenario = scenarioIdx;
  s.shocks = shocks;
  s.analyses = analyses;
  s.elapsed = s.on ? since(poweredAt) / 1000 : 0;
  s.timer = -1;
  s.timerLabel = "";
  uint32_t t = since(stateAt);
  if (state == State::Cpr) {
    s.timer = t / 1000;
    s.timerLabel = "RCP";
  } else if (state == State::Armed) {
    s.timer = t / 1000;  // how long the trainee has been hesitating
    s.timerLabel = "attesa";
  }
  s.armed = state == State::Armed;
  s.pads = IO::padsConnected();
  s.padsVirtual = IO::padsVirtual();
  s.autoMode = autoMode;
  s.forced = forced;
  if (s.on) {
    s.detail = String("Analisi: ") + analyses;
    uint16_t next = analyses + ((state == State::Analyze || state == State::Motion) ? 0 : 1);
    bool nextShock = forced >= 0 ? forced == 1 : Scenarios::shockable(sc(), next);
    s.detail += String(" · prossima: ") + (nextShock ? "defibrillabile" : "non defibrillabile");
    if (state == State::Cpr) s.detail = String("Ciclo RCP ") + cprCycle() + " di " + sc().cprCycles + " · " + s.detail;
  }
  return s;
}

uint32_t revision() { return rev; }

void onLog(std::function<void(const String &)> cb) { logCb = cb; }

}  // namespace Aed
