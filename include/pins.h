#pragma once
// Pin map for the Ai-Thinker ESP32 Audio Kit v2.2 (A436, ES8388 codec).
//
// Already used by the board (do not reuse):
//   0 MCLK, 25 WS, 26 DOUT, 27 BCLK, 35 DIN   I2S to the ES8388
//   32 SCL, 33 SDA                             codec I2C (addr 0x10)
//   21 PA_EN                                   speaker amplifier enable
//   2/14/15 (+13 in 4-bit)                     SD card, only in the audiokit-sd env
//   6-11 flash, 16/17 PSRAM
//
// Everything the trainer needs is on the two pin headers, so nothing has to
// be soldered on the board itself:
//   2x7 header next to LEDs D4/D5: IO5/18/19/22/23, GND, 3V3
//   JTAG header: IO12/13/14/15
// The onboard keys still work on the bench:
//   KEY1 = POWER, KEY2 = SHOCK (DIP 1 ON), KEY6 = simulated pad connector.
//
// Set a pin to -1 to disable that feature.

// ---- inputs (active LOW: switch/button to GND) ---------------------------
#ifdef AUDIO_FROM_SD
#define PIN_BTN_POWER    36  // 14 is the SD clock in this build: use KEY1
#define PIN_BTN_POWER2   -1
#else
#define PIN_BTN_POWER    14  // JTAG header MTMS. Also wakes from deep sleep
#define PIN_BTN_POWER2   36  // onboard KEY1, same function (cannot wake from sleep)
#endif
#define PIN_BTN_SHOCK    13  // JTAG header MTCK (DIP 4 ON), and KEY2 (DIP 1 ON)
#define PIN_PADS_DETECT   5  // 2x7 header IO5, and KEY6. Connector shorts it to GND
#define PIN_MODE_SWITCH  -1  // Semi/fully-automatic: set from the web UI
#define PIN_VOLUME_POT   -1  // e.g. 39 (ADC1_CH3). Wiper of a 10k pot between 3V3
                             // and GND. 39 is also headphone-detect: leave jack empty.

// ---- outputs (active HIGH: GPIO -> 330R -> LED -> GND) -------------------
#define PIN_LED_POWER    22  // Also lights onboard LED D4
#define PIN_LED_SHOCK    23  // Shock button light (KEY4 pad: don't press KEY4)

#ifdef AUDIO_FROM_SD
#define PIN_LED_PADS     -1  // 2/14/15 are SD lines in this build
#define PIN_LED_CPR      -1
#else
// GPIO12 is a strapping pin that must be LOW at reset: an LED + resistor to
// GND is safe there (it holds the pin low), a button with pull-up is not.
#define PIN_LED_PADS     12  // JTAG MTDI. "Plug connector" light, blinks while waiting
#define PIN_LED_CPR      15  // JTAG MTDO (DIP 5 ON). Metronome / CPR light
#endif

// ---- 7-segment display (TM1637 4-digit module) ---------------------------
#define PIN_TM1637_CLK   18  // KEY5 pad
#define PIN_TM1637_DIO   19  // KEY3 pad, also onboard LED D5 (flickers, harmless)

// ---- audio ---------------------------------------------------------------
#define AUDIO_SAMPLE_RATE 22050
