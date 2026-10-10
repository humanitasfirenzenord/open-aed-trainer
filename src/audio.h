#pragma once
#include <Arduino.h>

// Prompt player. Runs in its own FreeRTOS task so that the state machine,
// web server and WiFi never starve the I2S output.
//
// Files are MP3s addressed by a short name:
//   "it/analysing"  -> /it/analysing.mp3
//   "sfx/shock"     -> /sfx/shock.mp3
// Use voice("analysing") to get the path for the current language.
namespace Audio {

bool begin();                       // codec, filesystem, task
bool storageOk();                   // filesystem mounted and files found
const char *storageName();          // "LittleFS" or "SD"

void play(const char *name);        // enqueue a one-shot file
void pause(uint32_t ms);            // enqueue a silent gap between files
void tone(uint16_t hz);             // continuous tone while nothing else plays, 0 = off
void playNow(const char *name);     // stop everything, then play
void loop(const char *name);        // file to repeat when the queue is empty
uint32_t loopsCompleted();          // full plays of the loop file since loop()
void clearLoop();
void stop();                        // clear queue + loop, silence now

bool busy();                        // one-shot files still queued or playing
void setVolume(uint8_t percent);    // 0-100, codec hardware volume
// Speaker amplifier: kept on while true, otherwise switched on when something
// plays (with a 1 s warm-up) and off again 1 s after the last sound.
void keepAmplifierOn(bool on);
bool quiet();      // nothing playing or queued and the amplifier is off
void shutdown();   // before deep sleep: stop the task, power down the codec

String voice(const char *id);       // "<lang>/<id>"
void setLanguage(const char *lang);

}  // namespace Audio
