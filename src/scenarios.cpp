#include "scenarios.h"

// Modelled on the scenario sets of the XFT-120C+, Laerdal AED Trainer 3 and
// Philips HeartStart trainers.
static const Scenario TABLE[] = {
  // name                                   description                                                                      rhythm   pads motion lowBat  cpr cycles
  {"FV – 1 shock",                        "Ritmo defibrillabile, dopo 1 shock diventa non defibrillabile.",              "SN",    0, 0, false, 4},
  {"FV – 2 shock",                       "Ritmo defibrillabile per 2 analisi, poi non defibrillabile.",                    "SSN",   0, 0, false, 4},
  {"FV – 3 shock",                       "Ritmo defibrillabile per 3 analisi, poi non defibrillabile.",                    "SSSN",  0, 0, false, 4},
  {"FV – 4 shock",                       "Ritmo defibrillabile per 4 analisi, poi non defibrillabile.",                    "SSSSN", 0, 0, false, 4},
  {"Ritmo non defibrillabile",              "Asistolia/PEA: nessuno shock, solo RCP.",                                      "N",     0, 0, false, 4},
  {"FV persistente",                        "Ritmo sempre defibrillabile (FV refrattaria).",                                  "S",     0, 0, false, 4},
  {"Elettrodi: contatto scarso",            "\"Controllare gli elettrodi\" finché il connettore non viene reinserito, poi FV – 1 shock.", "SN", 1, 0, false, 4},
  {"Movimento durante l'analisi",           "Batteria scarica all'accensione, movimento alla prima analisi, poi FV – 1 shock.", "SN", 0, 1, true, 4},
  {"Non defibrillabile → FV",               "Prima analisi non defibrillabile, alla seconda FV – 1 shock.",                 "NSN",   0, 0, false, 4},
  {"Rifibrillazione",                       "FV – 1 shock, poi non defibrillabile, poi di nuovo FV – 1 shock.",           "SNSN",  0, 0, false, 4},
};

namespace Scenarios {

size_t count() { return sizeof(TABLE) / sizeof(TABLE[0]); }

const Scenario &get(size_t i) { return TABLE[i < count() ? i : 0]; }

bool shockable(const Scenario &s, uint16_t n) {
  size_t len = strlen(s.rhythm);
  if (len == 0 || n == 0) return false;
  size_t idx = min<size_t>(n, len) - 1;
  return s.rhythm[idx] == 'S';
}

}  // namespace Scenarios
