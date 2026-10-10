<p align="center">
  <img src="docs/header.svg" alt="Humanitas Firenze Nord" width="100%">
</p>

<h1 align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/logo-wordmark-dark.svg">
    <img src="docs/logo-wordmark.svg" alt="OpenAEDTrainer" height="72">
  </picture>
</h1>

**OpenAEDTrainer is an open source AED trainer board.** Use it to build an AED
trainer from scratch, or to bring back a commercial trainer whose
electronics have failed by swapping in a new board and wiring it to the
original buttons, lights, speaker and pad connector.

It runs on the low-cost **Ai-Thinker ESP32 Audio Kit v2.2 A436**
(ESP32-A1S, ES8388 codec, 4 MB flash). The reference build is an
XFT-120C+ trainer shell. The voice and the instructor web UI are in
Italian, and other languages can be added.

It simulates a semi-automatic or fully automatic AED with these stages:

1. Power on.
2. Apply pads.
3. Rhythm analysis.
4. Charge.
5. Shock: the flashing button, or automatic.
6. CPR: 4 cycles of 30:2 (about 90 s) with a 110 bpm metronome.
7. Re-analysis, then repeat from step 4 or 6.

It includes ten built-in training scenarios and a phone web UI for the instructor.

## Quick start

```bash
tools/setup.sh                                  # one-time: platformio, ffmpeg, Kokoro TTS (+ Rosetta on Apple Silicon)
tools/.venv/bin/python tools/gen_audio.py       # render voice + sounds into sdcard/ (see Voice prompts)
pio run -t uploadfs                             # flash the audio into internal flash
pio run -t upload                               # flash the firmware
pio device monitor                              # serial log + test console
```

Then join the open WiFi network `OpenAEDTrainer-XXXX` (set `WIFI_PASSWORD` in `src/main.cpp` for a password) and open
<http://192.168.4.1> (also `http://openaedtrainer.local`). Most phones open it automatically
through the captive portal.

## Project layout

| Path | What |
|---|---|
| `src/aed.cpp` | AED state machine: prompts, timings, LEDs, display |
| `src/scenarios.cpp` | Scenario table. **Add new scenarios here** (one line each) |
| `src/audio.cpp` | MP3 prompt player (own FreeRTOS task, queue + loop) |
| `src/io.cpp` | Buttons, pad detect, optional mode switch, LEDs, volume pot |
| `src/display.cpp` | Single-digit 7-segment driver (TM1637) |
| `src/power.cpp` | Deep sleep, wake-up buttons, auto-off timers |
| `src/web.cpp` + `web/index.html` | WiFi AP, captive portal, WebSocket UI (gzipped into the firmware at build time) |
| `include/pins.h` | **Pin map**. Set any pin to `-1` to disable that feature |
| `audio/prompts.json` | Prompt texts. Edit and re-run `gen_audio.py` |
| `tools/gen_audio.py` | Renders the prompts: Kokoro, Piper or macOS `say` |
| `tools/kokoro_tts.py` | Kokoro helper, runs in its own Python 3.12 environment |
| `sdcard/` | Rendered audio, about 0.7 MB, flashed by `uploadfs`. Generated locally, not in the repo |

## Hardware

### DIP switches (on the Audio Kit)

The DIP switches live on the Audio Kit board itself, labelled 1–5.

| Switch | Default build (audio in flash) | `audiokit-sd` build (audio on SD) |
|---|---|---|
| 1 | **ON**: KEY2 → GPIO13 (shock) | ON |
| 2 | OFF | OFF |
| 3 | OFF | ON (SD CMD) |
| 4 | ON only if the shock button is wired to the JTAG header IO13 | OFF |
| 5 | ON only if the CPR LED is wired to the JTAG header IO15 | OFF |

### Wiring

All buttons and switches connect **pin → GND**; the inputs are active low.
LEDs connect **pin → 330 Ω → LED → GND**.

| Function | GPIO | Where | Notes |
|---|---|---|---|
| Power button | 14 | JTAG header (MTMS) | Also wakes from deep sleep. Onboard KEY1 (GPIO36) works too |
| Shock button | 13 | JTAG header (MTCK) | DIP 4 ON. Onboard KEY2 works too with DIP 1 ON |
| Pad connector detect | 5 | 2×7 header | The connector shorts it to GND when plugged in. Onboard KEY6 simulates it |
| Volume pot (optional) | 39 | | 10 k pot across 3V3/GND, wiper to 39. Set `PIN_VOLUME_POT 39`. Keep the headphone jack empty |
| Power LED | 22 | header | Also lights onboard D4 |
| Shock-button LED | 23 | header | Don't press onboard KEY4 |
| Pad-connector LED | 12 | JTAG header (MTDI) | Blinks while waiting for pads. **Strapping pin: LED + resistor to GND only** (disabled in the SD build) |
| CPR / metronome LED | 15 | JTAG header (MTDO) | DIP 5 ON (disabled in the SD build) |
| 7-segment digit (via TM1637) CLK / DIO | 18 / 19 | header | The trainer's single digit, driven by a TM1637. Onboard D5 flickers, which is harmless |
| Speaker | — | LOUT or ROUT (white JST, right edge) | Onboard 3 W class-D amplifier, same audio on both |

### Board map

Top view, as printed on the board (ESP32 Audio Kit V2.2 A436):

```
 ┌────────────────────────────────────────────────────────────────────┐
 │  JTAG ▪▪▪▪           ┌───────────────┐        2×7 header ▪▪▪▪▪▪▪   │
 │                      │   ESP32-A1S   │                   ▪▪▪▪▪▪▪   │
 │  ┌─────┐             │    module     │          D4 D5  EARPHONES ◖ │
 │  │µSD  │             └───────────────┘                 LINEIN    ◖ │
 │  └─────┘                                                           │
 │  UART  USB ▭  (data + power)     ┌─────────┐              ROUT ▭   │
 │                                  │ DIP 1-5 │              LOUT ▭   │
 │  POWER USB ▭  (power only)       └─────────┘                       │
 │  BAT+/BAT− ▭  (battery)                                            │
 │                                                                    │
 │   RST   BOOT   KEY1   KEY2   KEY3   KEY4   KEY5   KEY6             │
 │                POWER  SHOCK                       pads             │
 └────────────────────────────────────────────────────────────────────┘
```

- **KEY1** (third button from the left, after RST and BOOT) also works as
  POWER on the bench; the trainer's real POWER button goes to the JTAG
  header (IO14).
- **BAT+/BAT−**: battery connector next to the POWER-only micro-USB, with
  charge-status LEDs D1–D3. Check the board's charger specs before
  connecting a 1-cell LiPo (3.7 V).

### Connection diagram

Drawn from photos of the A436 board: the pin order matches the silkscreen.

**2×7 header** (the one next to the D4/D5 LEDs), seen from the component side
with the ESP32 module on the **left** and the header at the top edge:

```
           GND    IO0    RST    TX0    RX0    3V3    3V3
   top     [●]    [ ]    [ ]    [ ]    [ ]    [●]    [ ]
            │      └──────┴──────┴──────┴─ leave free (boot / USB serial)
            │                                │
            └─ common GND                    └─ TM1637 VCC
   bottom  [✕]    [●]    [●]    [●]    [●]    [●]    [●]
           IO21   IO22   IO19   IO23   IO18   IO5    GND
            │      │      │      │      │      │      │
            │      │      │      │      │      │      └─ common GND
            │      │      │      │      │      └─ PAD CONNECTOR ──┤ GND
            │      │      │      │      └─ TM1637 CLK
            │      │      │      └─ [330Ω]──▶|── SHOCK button light ── GND
            │      │      └─ TM1637 DIO
            │      └─ [330Ω]──▶|── POWER button light ── GND
            └─ DO NOT USE: speaker amplifier enable
```

**JTAG header** (1×4, next to the micro-SD slot), seen from the component
side with the ESP32 module on the **right**:

```
           MTDO   MTCK   MTDI   MTMS
           IO15   IO13   IO12   IO14
           [●]    [●]    [●]    [●]
            │      │      │      └─ POWER button ──┤ GND
            │      │      └─ [330Ω]──▶|── pad-connector light ── GND
            │      └─ SHOCK button ──┤ GND          (needs DIP 4 ON)
            └─ [330Ω]──▶|── CPR / metronome light ── GND   (needs DIP 5 ON)
```

Everything is on the two headers: nothing needs to be soldered on the board.
The semi-automatic / fully automatic mode is set from the web UI and
remembered.

**Speaker:** a 4–8 Ω speaker (up to 3 W) on **LOUT** or **ROUT**, the white
2-pin JST connectors on the right edge. Both carry the same audio.

**DIP switches** (functions printed on the board next to them):

| # | Silkscreen | Setting | Why |
|---|---|---|---|
| 1 | KEY2 / 13 | ON | onboard KEY2 = SHOCK as well |
| 2 | DATA3 / 13 | OFF | SD 4-bit only |
| 3 | CMD / 15 | OFF | SD only (ON in the `audiokit-sd` build) |
| 4 | MTCK / 13 | ON | SHOCK on the JTAG header |
| 5 | MTDO / 15 | ON | CPR light on the JTAG header |

Notes:
- **All inputs are active-low.** Buttons and switches short their GPIO to GND.
  Never wire them to 3V3.
- **IO12 (MTDI) is a boot strapping pin** that must be low when the chip
  resets. The pad-connector light (LED + resistor to GND) is safe there;
  never put a button with a pull-up, or anything else that can pull it
  high, on IO12, or the board won't boot.
- **Button lights:** a GPIO gives 3.3 V and about 10 mA at most. If the
  XFT's buttons have 5 V/12 V lamps, drive them through a small N-MOSFET or
  NPN transistor instead.
- **SD build:** if you use the SD card (`audiokit-sd`), IO2/IO14/IO15 belong to
  the card. POWER moves back to KEY1 (GPIO36) and the pad and CPR lights are
  disabled.

On the bench, the onboard keys do the same jobs as the external buttons:
- KEY1 is power.
- KEY2 is shock.
- Holding KEY6 simulates connected pads.

The XFT's original LEDs and buttons must be rewired to these GPIOs. Its
single-digit 7-segment display is driven through a TM1637 chip or module
(2 wires): the digit's segments go to SEG1–SEG8 and its common pin to GRID1.
The TM1637 suits common-cathode digits, so check the XFT digit's polarity
first. The display is optional; set its pins to `-1` to disable it.

## How the trainer behaves

```
OFF ─power─> POWER ON (tone + 1 s self-test)
               │ pads already plugged? ──────────────┐
               ▼                                     │
            WAIT PADS ("Applicare gli elettrodi…", repeats every 8 s)
               │ connector plugged                   │
               ▼                                     ▼
          [PADS CHECK]* ──re-plug──> ANALYSIS ("Non toccare il paziente…", 8 s)
                                       │      └─[MOTION]* → re-analyse
                      shockable ◄──────┴──────► non-shockable
                          ▼                          ▼
     CHARGE ("Shock consigliato. Carica in corso…")   "Shock non consigliato.
                                                     Riprendere RCP."
          ▼ semi                ▼ auto               │
   ARMED: prompt, then a    "Erogazione shock…"      │
   continuous tone + light          │                │
   until the press                  │                │
     │press        │instructor skip │                │
     ▼             ▼                ▼                │
  SHOCK         DISARM           SHOCK               │
     └─────────────┴───────┬────────┴────────────────┘
                           ▼
                CPR, 4 cycles of: 30 beats @110/min, "Dare respiro" ×2 (3 s apart)
                           ▼
                ANALYSIS ("Non toccare il paziente…") …
```
`*` = injected by the scenario or by the instructor.

If the pads are pulled off at any point, the unit disarms and prompts
"Controllare gli elettrodi".

Pressing SHOCK when the unit is not armed does nothing, but it is noted in
the instructor log ("Pulsante SHOCK premuto fuori tempo"). To pick a
scenario without a phone, **hold SHOCK for 2 s while the trainer is off**.
Each hold moves to the next scenario; the display shows its number and it beeps.

**Display (single digit):**

| Shows | When |
|---|---|
| `8.` | Self-test at power on |
| `1`–`9`, `0` | Waiting for pads: the scenario number (`0` = scenario 10). Also shown for 2 s after changing scenario |
| `F` | Check pads (poor contact) |
| spinning segment | Analysis in progress |
| `-` | Motion detected |
| `C` | Charging |
| `n`, blinking | Armed, shock number n ready to deliver |
| `n` | After a shock / no shock: shocks delivered so far |
| `n.` | CPR, cycle n (the dot tells it apart from the shock count) |
| `L` | Battery low (3 s) |

### Scenarios

| # | Name | Rhythm per analysis |
|---|---|---|
| 1 | FV – 1 shock | S N… |
| 2 | FV – 2 shock | S S N… |
| 3 | FV – 3 shock | S S S N… |
| 4 | FV – 4 shock | S S S S N… |
| 5 | Ritmo non defibrillabile | N… |
| 6 | FV persistente | S… |
| 7 | Elettrodi: contatto scarso | poor contact until the connector is re-plugged, then S N… |
| 8 | Movimento durante l'analisi | low battery + motion at the 1st analysis, then S N… |
| 9 | Non defibrillabile → FV | N S N… |
| 10 | Rifibrillazione | S N S N… |

To add a scenario, add one line to `src/scenarios.cpp`:

```cpp
{"Nome", "Descrizione", "SSN", /*poorPadsAt*/0, /*motionAt*/2, /*lowBattery*/false, /*cprCycles*/4},
```

The instructor can also override things live from the web UI:
- Force the next analysis to shockable or non-shockable.
- Inject motion, poor contact or low battery.
- Skip CPR.
- Simulate the pads.
- Switch between semi-automatic and fully automatic.
- Set the volume.

## Power and battery

When the trainer is off and idle, the ESP32 goes into **deep sleep**: WiFi,
codec and amplifier off, CPU ~10 µA. A wake-up is a fresh boot (~1 s).

| Event | Behaviour |
|---|---|
| Asleep, **POWER** pressed | Wakes and switches the trainer on directly |
| Asleep, **SHOCK** pressed | Wakes into standby (beep, WiFi on) so the instructor can set up from the phone |
| Switched off with the POWER button | Power-off tone, then deep sleep |
| Switched off from the web UI | Stays in standby; sleeps after 5 min without activity |
| On, no button / web / pad activity for 15 min | Switches itself off and sleeps |

Only RTC-capable pins can wake the ESP32: POWER (GPIO14) and SHOCK
(GPIO13) are the wake buttons. Onboard KEY1 works as POWER while awake but
cannot wake the board; use KEY2 (SHOCK) for that on the bench. The timeouts are at the top of `src/power.cpp`.

The Audio Kit itself still draws a few mA while the ESP32 sleeps (power LED,
AMS1117 regulator, codec). For storage of weeks or more, cut the battery
with a real switch, or a soft-latch circuit driven by the POWER button.

## Voice prompts

The repository contains the **texts** of the prompts (`audio/prompts.json`)
and a script that renders them (`tools/gen_audio.py`), not the audio. Each
builder generates their own voice once, before flashing:

```bash
tools/.venv/bin/python tools/gen_audio.py     # writes sdcard/ (~0.7 MB)
pio run -t uploadfs
```

The firmware plays whatever MP3s it finds, matched only by file name
(`sdcard/it/<prompt_id>.mp3`). Any voice works, however it was made, as
long as the names match.

### Ways to get a voice

| Option | How | Licence of the result |
|---|---|---|
| **Kokoro** (default) | Open-weight neural TTS, runs offline. Installed by `tools/setup.sh`. Italian voices: `if_sara`, `im_nicola` | Apache-2.0 model: the audio can be shared |
| **Piper** | Open-source offline TTS, also on Linux. `tools/setup.sh piper`, then `--engine piper` | Depends on the voice: check its model card |
| **Human recording** | A volunteer reads the prompts. Save one MP3 (or WAV converted with ffmpeg) per prompt in `sdcard/it/` | Yours to decide, e.g. CC-BY. Most authentic for training |
| **Cloud TTS** (Azure, Google, ElevenLabs…) | Generate with the provider's own tools and save the files with the prompt names | Per the provider's terms |
| **macOS `say`** | `--engine say`. Uses the system voice when `"say"` is empty | Apple's licence: personal, non-commercial use only; recordings may not be published or redistributed |

### Choosing the engine and voice

The engine and voice settings live in `audio/prompts.json` (`engine` and
`languages.it`). To use different settings on your machine without
changing the shared file, create `audio/voice.local.json` (not committed):

```json
{
  "engine": "piper",
  "languages": { "it": { "kokoro": { "voice": "im_nicola", "speed": 0.95 } } }
}
```

`--engine` on the command line overrides both.

### Changing a sentence

Edit `audio/prompts.json`, then re-render just that prompt and flash:

```bash
tools/.venv/bin/python tools/gen_audio.py --only shock_advised --force --play
pio run -t uploadfs
```

The sound effects (beeps, metronome, shock tone) are synthesised by the
script itself and are part of the project.

## Serial test console

Use `pio device monitor` at 115200. Each command is a single line:

| Command | Action |
|---|---|
| `p` | Power |
| `s` | Shock |
| `k` | Skip |
| `m` | Motion |
| `c` | Poor contact |
| `b` | Low battery |
| `d1` / `d0` / `d-` | Pads on / off / follow the connector |
| `f1` / `f0` / `f-` | Force the next rhythm |
| `a1` / `a0` | Fully automatic / semi-automatic |
| `n3` | Select scenario 3 |
| `v60` | Volume 60% |
| `?` | Print the status |

## Notes

- **Audio in flash or on SD:** the default build keeps the audio in a 2 MB
  LittleFS partition, so no card is needed and there are no pin conflicts.
  `pio run -e audiokit-sd -t upload` reads the same files from a micro-SD card
  instead: copy the contents of `sdcard/` to the root of the card. That build
  uses GPIO 2/14/15.
- **No OTA:** the partition table gives the app 1.9 MB and the audio 2 MB.
- The prompts follow ERC 2021 (30:2, 100–120/min, re-analysis after each CPR block) and the
  wording of Italian AEDs (CardiAid, HeartSine). This is a training device only.

## License

OpenAEDTrainer is free software, released under the
[GNU General Public License v3.0](LICENSE). You can use, study, modify and
share it, including commercially. If you distribute it or a modified
version, including firmware flashed on a board you give away or sell, it
must stay under GPLv3 with its source code available.

It builds on [arduino-audio-tools](https://github.com/pschatzmann/arduino-audio-tools)
and [arduino-audio-driver](https://github.com/pschatzmann/arduino-audio-driver)
by Phil Schatzmann (GPLv3), the Helix MP3 decoder, ESPAsyncWebServer and
ArduinoJson.

The Humanitas Firenze Nord name and logo in `docs/header.svg` belong to
P.A. Humanitas Firenze Nord and are not covered by the GPL.
