#include "audio.h"

#include <sys/stat.h>

#include <deque>

#include "AudioTools.h"
#include "AudioTools/AudioCodecs/CodecMP3Helix.h"
#include "AudioTools/AudioLibs/AudioBoardStream.h"
#include "pins.h"

#include <LittleFS.h>

namespace Audio {
namespace {

AudioBoardStream kit(AudioKitEs8388V1);

// The ES8388 auto-mutes on all-zero data, and its output DC level shifts
// when it does: the amplifier turns every mute/unmute into a soft tick
// (between files, and inside them, e.g. between metronome beats). Replacing
// exact zeros with a ±1 LSB pattern (~-90 dB, inaudible) keeps it unmuted.
class NoZeroOutput : public AudioOutput {
 public:
  size_t write(const uint8_t *data, size_t len) override {
    int16_t buf[256];
    size_t done = 0;
    while (done < len) {
      size_t n = min(len - done, sizeof(buf));
      memcpy(buf, data + done, n);
      for (size_t i = 0; i < n / 2; i++)
        if (buf[i] == 0) buf[i] = (phase++ & 2) ? -1 : 1;
      kit.write((const uint8_t *)buf, n);
      done += n;
    }
    bytesOut += len;
    return len;
  }
  int availableForWrite() override { return kit.availableForWrite(); }
  void setAudioInfo(AudioInfo info) override {
    AudioOutput::setAudioInfo(info);
    kit.setAudioInfo(info);
  }

 public:
  uint32_t bytesOut = 0;

 private:
  uint32_t phase = 0;
} out;

MP3DecoderHelix mp3;
EncodedAudioOutput dec(&out, &mp3);
StreamCopy copier;

fs::FS *fsys = nullptr;
bool fsOk = false;
String lang = "it";

// Shared between the caller (loop/web) and the audio task.
SemaphoreHandle_t mtx;
std::deque<String> queue;
std::vector<String> loopItems;
size_t loopPos = 0;    // next item of loopItems
uint32_t loopGen = 0;  // bumped by loop()/stop(): a finishing old item must not advance the new loop
bool curOneShot = false;
volatile bool stopReq = false;
volatile int pendingVol = -1;
volatile bool keepAmp = false;
volatile uint16_t toneHz = 0;  // continuous tone, 0 = off
volatile uint32_t loopsDone = 0;  // complete plays of the loop sequence

// Audio-task only.
File cur;
bool curIsLoop = false;
uint32_t curGen = 0;
uint32_t curSlotMs = 0;  // pad the current file with silence up to this length
volatile bool ampOn = false;
TaskHandle_t taskHandle = nullptr;
uint32_t ampOnAt = 0;
uint32_t idleAt = 0;
bool idle = true;

// The NS4150 amplifier pops and swallows the start of the sound when it is
// switched on, so after enabling it we feed silence for a while before
// playing anything. It is switched off again after a quiet period, unless
// the trainer is on (keepAmp).
constexpr uint32_t AMP_WARMUP_MS = 1000;
constexpr uint32_t AMP_COOLDOWN_MS = 1000;
constexpr uint32_t LEAD_IN_MS = 100;  // silence before a sound after a pause
constexpr uint32_t TAIL_MS = 200;     // silence after a sound, drains the DMA buffers

constexpr uint32_t msToFrames(uint32_t ms) { return (uint64_t)AUDIO_SAMPLE_RATE * ms / 1000; }

// Silence, through NoZeroOutput so it never reaches the codec as zeros.
// Returns false if cut short by stop().
bool writeSilenceFrames(uint32_t frames, bool interruptible = false) {
  static const int16_t zeros[256] = {};  // 128 frames, 16-bit stereo
  while (frames) {
    if (interruptible && stopReq) return false;
    uint32_t n = min<uint32_t>(frames, 128);
    out.write((const uint8_t *)zeros, n * 4);
    frames -= n;
  }
  return true;
}

void writeSilenceMs(uint32_t ms) { writeSilenceFrames(msToFrames(ms)); }

// Sound effects: tones and sweeps modelled on real AED sounds, synthesised
// like the continuous tone below instead of being stored as files.
struct Seg {
  uint16_t f0, f1;  // Hz, f1 != f0 = linear sweep, 0 = silence
  uint16_t ms;
  float vol;        // peak, 0..1 of full scale
  float h2, h3;     // 2nd and 3rd harmonic, relative to the fundamental
};
constexpr Seg T(uint16_t hz, uint16_t ms, float vol = 0.8f, float h2 = 0, float h3 = 0) {
  return {hz, hz, ms, vol, h2, h3};
}
constexpr Seg SWEEP(uint16_t f0, uint16_t f1, uint16_t ms, float vol) { return {f0, f1, ms, vol, 0, 0}; }
constexpr Seg GAP(uint16_t ms) { return {0, 0, ms, 0, 0, 0}; }

struct Sfx {
  const char *id;
  Seg seg[6];  // unused entries have ms = 0
};

const Sfx SFX[] = {
  {"beep", {T(880, 150, 0.8f, 0.2f)}},  // UI beep, button acknowledge
  {"power_on", {T(660, 120), GAP(40), T(990, 200)}},
  {"power_off", {T(990, 120), GAP(40), T(660, 250)}},
  {"shock", {SWEEP(180, 60, 180, 1.0f), T(1200, 250, 0.7f, 0.5f, 0.3f)}},  // thump + burst
  {"disarm", {SWEEP(1200, 300, 600, 0.5f)}},  // internal discharge
  {"alert", {T(600, 200), GAP(100), T(600, 200), GAP(100), T(600, 200), GAP(100)}},
};
constexpr Seg CLICK = T(1000, 80, 0.9f, 0.3f);  // metronome
constexpr uint32_t SFX_GAP_MS = 250;

constexpr float TWO_PI_F = 2 * (float)PI;  // float: the ESP32 FPU has no double

// One segment, 5 ms attack and 20 ms release so the edges never click
bool writeSeg(const Seg &s) {
  uint32_t n = msToFrames(s.ms);
  if (!s.f0) return writeSilenceFrames(n, true);
  constexpr uint32_t ATTACK = AUDIO_SAMPLE_RATE * 5 / 1000;
  constexpr uint32_t RELEASE = AUDIO_SAMPLE_RATE * 20 / 1000;
  // 0.68: the old MP3 effects went through a -3.4 dB mono -> stereo upmix,
  // keep the same balance with the voice prompts
  const float scale = 0.68f * s.vol * 32000 / (1 + s.h2 + s.h3);
  int16_t buf[128 * 2];
  float phase = 0;
  uint32_t i = 0;
  while (i < n) {
    if (stopReq) return false;
    int k = 0;
    for (; k < 128 && i < n; k++, i++) {
      float f = s.f0 + ((float)s.f1 - s.f0) * i / n;
      phase += TWO_PI_F * f / AUDIO_SAMPLE_RATE;
      if (phase > TWO_PI_F) phase -= TWO_PI_F;
      float env = i < ATTACK ? (float)i / ATTACK : n - i < RELEASE ? (float)(n - i) / RELEASE : 1.0f;
      float v = sinf(phase);
      if (s.h2) v += s.h2 * sinf(2 * phase);
      if (s.h3) v += s.h3 * sinf(3 * phase);
      buf[2 * k] = buf[2 * k + 1] = (int16_t)(v * scale * env);
    }
    out.write((const uint8_t *)buf, k * 4);
  }
  return true;
}

bool writeSfx(const char *id) {
  for (const Sfx &sfx : SFX) {
    if (strcmp(sfx.id, id)) continue;
    for (const Seg &s : sfx.seg)
      if (s.ms && !writeSeg(s)) return false;
    // Gap before the next sound, as the old MP3 effects had (e.g. shock
    // tone -> "shock delivered")
    return writeSilenceFrames(msToFrames(SFX_GAP_MS), true);
  }
  log_w("unknown sound effect %s", id);
  return true;
}

// Clicks on an exact grid (no drift over the 30 compressions)
bool writeBeats(uint16_t count, uint16_t bpm) {
  const double period = 60.0 * AUDIO_SAMPLE_RATE / bpm;  // frames
  uint32_t written = 0;
  for (uint16_t b = 0; b < count; b++) {
    if (!writeSeg(CLICK)) return false;
    written += msToFrames(CLICK.ms);
    uint32_t next = lround((b + 1) * period);
    if (next > written && !writeSilenceFrames(next - written, true)) return false;
    written = max(next, written);
  }
  return true;
}

// Continuous sine tone generated on the fly (no MP3 loop seams). The level
// ramps over ~5 ms so starting, stopping or ducking it never clicks.
float toneLevel = 0;  // 0..1, current ramp position
float tonePhase = 0;
uint16_t toneLastHz = 1000;

void writeToneChunk(bool on) {
  constexpr int FRAMES = 220;  // 10 ms
  constexpr float PEAK = 0.35f * 32767;
  constexpr float STEP = 1.0f / (AUDIO_SAMPLE_RATE * 0.005f);
  static int16_t buf[FRAMES * 2];
  if (toneHz) toneLastHz = toneHz;
  float inc = 2 * PI * toneLastHz / AUDIO_SAMPLE_RATE;
  for (int i = 0; i < FRAMES; i++) {
    toneLevel = on ? min(1.0f, toneLevel + STEP) : max(0.0f, toneLevel - STEP);
    int16_t v = (int16_t)(sinf(tonePhase) * PEAK * toneLevel);
    buf[2 * i] = buf[2 * i + 1] = v;
    tonePhase += inc;
    if (tonePhase > 2 * PI) tonePhase -= 2 * PI;
  }
  out.write((const uint8_t *)buf, sizeof(buf));
}

void amp(bool on) {
  if (on == ampOn) return;
  kit.setSpeakerActive(on);
  ampOn = on;
  if (on) ampOnAt = millis();
  log_d("amplifier %s", on ? "on" : "off");
}

constexpr const char *PAUSE_PREFIX = "@pause:";  // @pause:<ms>
constexpr const char *BEATS_PREFIX = "@beats:";  // @beats:<count>:<bpm>
constexpr const char *SLOT_PREFIX = "@slot:";    // @slot:<ms>:<name>
constexpr const char *SFX_PREFIX = "sfx/";

struct Lock {
  Lock() { xSemaphoreTake(mtx, portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(mtx); }
};

NullStream discard;

uint32_t curStart = 0;
String curName;

void closeCur() {
  if (cur) {
    log_d("played %s: %u bytes PCM = %u ms audio in %u ms", curName.c_str(), out.bytesOut,
          out.bytesOut / (AUDIO_SAMPLE_RATE * 4 / 1000), millis() - curStart);
    cur.close();
    // Reset the decoder, throwing away what it still buffers: flushing the
    // last (partial) frame produces a click. The files end with silence
    // padding, so nothing audible is lost.
    mp3.setOutput(discard);
    mp3.end();
    mp3.setOutput(out);
  }
  Lock l;
  curOneShot = false;
}

// The current item has finished (or was cut short): advance the loop
void itemDone(bool completed) {
  Lock l;
  curOneShot = false;
  if (curIsLoop && completed && curGen == loopGen && !loopItems.empty() &&
      ++loopPos >= loopItems.size()) {
    loopPos = 0;
    loopsDone++;
  }
  curIsLoop = false;
}

bool openFile(const String &name) {
  String path = customPath(name);  // uploaded from the web UI
  if (!exists(path)) path = defaultPath(name);
  cur = fsys->open(path, "r");
  if (!cur) {
    log_w("missing audio file %s", path.c_str());
    return false;
  }
  mp3.begin();
  copier.begin(dec, cur);
  out.bytesOut = 0;
  curStart = millis();
  curName = name;
  return true;
}

void task(void *) {
  for (;;) {
    if (pendingVol >= 0) {
      kit.setVolume(pendingVol / 100.0f);
      pendingVol = -1;
    }
    if (stopReq) {
      stopReq = false;
      closeCur();
      writeSilenceMs(25);  // flush what's left in the DMA buffers
    }
    if (keepAmp && !ampOn) amp(true);
    if (!cur) {
      bool pending;
      {
        Lock l;
        pending = !queue.empty() || !loopItems.empty() || toneHz || toneLevel > 0;
      }
      if (!pending) {
        if (!idle) {
          idle = true;
          idleAt = millis();
          writeSilenceMs(TAIL_MS);
        }
        if (ampOn && !keepAmp && millis() - idleAt > AMP_COOLDOWN_MS) amp(false);
        if (ampOn) {
          // Keep the I2S stream running with silence: an underrun makes the
          // output jump and the amplifier turns that into a click.
          writeSilenceMs(20);  // blocks until there is room in the DMA buffers
        } else {
          vTaskDelay(pdMS_TO_TICKS(10));
        }
        continue;
      }
      if (!ampOn) amp(true);
      if (millis() - ampOnAt < AMP_WARMUP_MS) {
        writeSilenceMs(20);  // keep the codec fed while the amp settles
        continue;
      }
      if (idle) {
        idle = false;
        writeSilenceMs(LEAD_IN_MS);
      }
      // Tone: plays when nothing else is queued, fades out before anything else
      bool fileWaiting;
      {
        Lock l;
        fileWaiting = !queue.empty() || !loopItems.empty();
      }
      if (toneHz && !fileWaiting) {
        writeToneChunk(true);
        continue;
      }
      if (toneLevel > 0) {
        writeToneChunk(false);
        continue;
      }
      String next;
      {
        Lock l;
        if (!queue.empty()) {
          next = queue.front();
          queue.pop_front();
          curOneShot = true;
          curIsLoop = false;
        } else if (!loopItems.empty()) {
          next = loopItems[loopPos % loopItems.size()];
          curIsLoop = true;
          curGen = loopGen;
        }
      }
      if (next.isEmpty()) {
        vTaskDelay(pdMS_TO_TICKS(10));
        continue;
      }
      curSlotMs = 0;
      if (next.startsWith(SLOT_PREFIX)) {
        int sep = next.indexOf(':', strlen(SLOT_PREFIX));
        curSlotMs = next.substring(strlen(SLOT_PREFIX), sep).toInt();
        next = next.substring(sep + 1);
      }
      if (next.startsWith(PAUSE_PREFIX)) {
        // Silence between prompts, interruptible by stop()
        itemDone(writeSilenceFrames(msToFrames(next.substring(strlen(PAUSE_PREFIX)).toInt()), true));
        continue;
      }
      if (next.startsWith(BEATS_PREFIX)) {
        int sep = next.indexOf(':', strlen(BEATS_PREFIX));
        itemDone(writeBeats(next.substring(strlen(BEATS_PREFIX), sep).toInt(), next.substring(sep + 1).toInt()));
        continue;
      }
      if (next.startsWith(SFX_PREFIX)) {
        itemDone(writeSfx(next.c_str() + strlen(SFX_PREFIX)));
        continue;
      }
      if (!fsOk || !openFile(next)) {
        closeCur();
        // Pretend it played for a moment so prompt timings stay sane
        itemDone(writeSilenceFrames(msToFrames(max<uint32_t>(curSlotMs, 300)), true));
        continue;
      }
    }
    if (copier.copy() == 0 && !cur.available()) {
      uint32_t played = out.bytesOut / 4;  // frames, 16-bit stereo
      closeCur();
      uint32_t slot = msToFrames(curSlotMs);
      itemDone(slot <= played || writeSilenceFrames(slot - played, true));  // false if stopped
    }
  }
}

}  // namespace

bool begin() {
  mtx = xSemaphoreCreateMutex();

  auto cfg = kit.defaultConfig(TX_MODE);
  cfg.sd_active = false;  // no SD card: audio lives in LittleFS
  cfg.sample_rate = AUDIO_SAMPLE_RATE;
  cfg.channels = 2;
  cfg.bits_per_sample = 16;
  cfg.buffer_count = 8;
  cfg.buffer_size = 512;
  if (!kit.begin(cfg)) log_e("codec init failed (ES8388 not found on I2C 32/33?)");
  kit.setSpeakerActive(false);  // switched on on demand, see amp()
  kit.setVolume(0.7f);
  dec.begin();

  fsOk = LittleFS.begin(false);
  fsys = &LittleFS;
  if (fsOk && !exists(defaultPath(voice("analysing")))) {
    log_w("LittleFS mounted but audio files missing (pio run -t uploadfs)");
    fsOk = false;
  }
  log_i("audio storage LittleFS: %s", fsOk ? "ok" : "NOT AVAILABLE");

  // Core 1 alongside loop(), but higher priority. WiFi lives on core 0.
  xTaskCreatePinnedToCore(task, "audio", 8192, nullptr, 5, &taskHandle, 1);
  return fsOk;
}

bool storageOk() { return fsOk; }


void play(const char *name) {
  Lock l;
  queue.emplace_back(name);
}

void tone(uint16_t hz) { toneHz = hz; }

void pause(uint32_t ms) { play((String(PAUSE_PREFIX) + ms).c_str()); }

void playNow(const char *name) {
  stop();
  play(name);
}

String beats(uint16_t count, uint16_t bpm) { return String(BEATS_PREFIX) + count + ":" + bpm; }

String slot(const String &name, uint32_t ms) { return String(SLOT_PREFIX) + ms + ":" + name; }

void loop(const std::vector<String> &items) {
  Lock l;
  loopItems = items;
  loopPos = 0;
  loopGen++;
  loopsDone = 0;
}

uint32_t loopsCompleted() { return loopsDone; }

void clearLoop() {
  Lock l;
  loopItems.clear();
  loopGen++;
}

void stop() {
  toneHz = 0;
  {
    Lock l;
    queue.clear();
    loopItems.clear();
    loopGen++;
  }
  stopReq = true;
}

bool busy() {
  Lock l;
  return !queue.empty() || curOneShot;
}

void keepAmplifierOn(bool on) { keepAmp = on; }

bool quiet() { return !busy() && !ampOn; }

void shutdown() {
  stop();
  keepAmp = false;
  delay(50);  // let the task notice the stop
  if (taskHandle) vTaskSuspend(taskHandle);
  kit.setSpeakerActive(false);
  ampOn = false;
  kit.end();  // ES8388 power down + I2S off
}

void setVolume(uint8_t percent) { pendingVol = min<int>(percent, 100); }

String voice(const char *id) { return lang + "/" + id; }

void setLanguage(const char *l) { lang = l; }

const String &language() { return lang; }

fs::FS &storage() { return *fsys; }

// stat() instead of FS::exists(): that logs an error for every missing
// file, i.e. for every prompt without a custom version
bool exists(const String &path) {
  String full = "/littlefs" + path;
  struct stat st;
  return stat(full.c_str(), &st) == 0;
}

String defaultPath(const String &name) { return "/" + name + ".mp3"; }

String customPath(const String &name) { return "/custom/" + name + ".mp3"; }

uint64_t freeBytes() {
  return LittleFS.totalBytes() - LittleFS.usedBytes();
}

}  // namespace Audio
