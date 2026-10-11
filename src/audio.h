#pragma once
#include <Arduino.h>
#include <FS.h>

#include <vector>

// Prompt player. Runs in its own FreeRTOS task so that the state machine,
// web server and WiFi never starve the I2S output.
//
// Sounds are addressed by a short name:
//   "it/analysing"  -> voice prompt, /custom/it/analysing.mp3 if the user
//                      uploaded one, otherwise /it/analysing.mp3
//   "sfx/shock"     -> sound effect, synthesised on the fly (no file)
// Use voice("analysing") to get the name for the current language.
namespace Audio {

bool begin();                       // codec, filesystem, task
bool storageOk();                   // filesystem mounted and files found

void play(const char *name);        // enqueue a one-shot sound
void pause(uint32_t ms);            // enqueue a silent gap between sounds
void tone(uint16_t hz);             // continuous tone while nothing else plays, 0 = off
void playNow(const char *name);     // stop everything, then play
// Sequence to repeat when the queue is empty. Items are sound names or:
String beats(uint16_t count, uint16_t bpm);       // metronome clicks, exact tempo
String slot(const String &name, uint32_t ms);     // play name, then silence up to ms
void loop(const std::vector<String> &items);
uint32_t loopsCompleted();          // full plays of the loop sequence since loop()
void clearLoop();
void stop();                        // clear queue + loop, silence now

bool busy();                        // one-shot sounds still queued or playing
void setVolume(uint8_t percent);    // 0-100, codec hardware volume
// Speaker amplifier: kept on while true, otherwise switched on when something
// plays (with a 1 s warm-up) and off again 1 s after the last sound.
void keepAmplifierOn(bool on);
bool quiet();      // nothing playing or queued and the amplifier is off
void shutdown();   // before deep sleep: stop the task, power down the codec

String voice(const char *id);       // "<lang>/<id>"
void setLanguage(const char *lang);
const String &language();

// Voice prompt files, for the web UI's upload / revert
fs::FS &storage();
String defaultPath(const String &name);  // "/it/analysing.mp3"
String customPath(const String &name);   // "/custom/it/analysing.mp3"
bool exists(const String &path);       // quiet, unlike FS::exists()
uint64_t freeBytes();

}  // namespace Audio
