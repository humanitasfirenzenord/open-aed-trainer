#include "audio.h"

#include <deque>

#include "AudioTools.h"
#include "AudioTools/AudioCodecs/CodecMP3Helix.h"
#include "AudioTools/AudioLibs/AudioBoardStream.h"
#include "pins.h"

#ifdef AUDIO_FROM_SD
#include <SD_MMC.h>
#else
#include <LittleFS.h>
#endif

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
String loopName;
bool curOneShot = false;
volatile bool stopReq = false;
volatile int pendingVol = -1;
volatile bool keepAmp = false;
volatile uint16_t toneHz = 0;  // continuous tone, 0 = off
volatile uint32_t loopsDone = 0;  // complete plays of the loop file
bool curIsLoop = false;

// Audio-task only.
File cur;
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

// Silence, through NoZeroOutput so it never reaches the codec as zeros
void writeSilenceMs(uint32_t ms) {
  static const int16_t zeros[256] = {};
  size_t bytes = (size_t)AUDIO_SAMPLE_RATE * 4 * ms / 1000;  // 16-bit stereo
  bytes -= bytes % 4;
  while (bytes) {
    size_t n = min(bytes, sizeof(zeros));
    out.write((const uint8_t *)zeros, n);
    bytes -= n;
  }
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

constexpr const char *PAUSE_PREFIX = "@pause:";

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

bool openFile(const String &name) {
  String path = "/" + name + ".mp3";
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
        pending = !queue.empty() || loopName.length() || toneHz || toneLevel > 0;
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
        fileWaiting = !queue.empty() || loopName.length();
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
        } else if (loopName.length()) {
          next = loopName;
          curIsLoop = true;
        }
      }
      if (next.isEmpty()) {
        vTaskDelay(pdMS_TO_TICKS(10));
        continue;
      }
      if (next.startsWith(PAUSE_PREFIX)) {
        // Silence between prompts, interruptible by stop()
        uint32_t end = millis() + next.substring(strlen(PAUSE_PREFIX)).toInt();
        while (!stopReq && (int32_t)(end - millis()) > 0) writeSilenceMs(10);
        Lock l;
        curOneShot = false;
        continue;
      }
      if (!fsOk || !openFile(next)) {
        closeCur();
        // Pretend it played for a moment so prompt timings stay sane
        vTaskDelay(pdMS_TO_TICKS(300));
        continue;
      }
    }
    if (copier.copy() == 0 && !cur.available()) {
      if (curIsLoop) loopsDone++;  // played to the end, not stopped
      closeCur();
    }
  }
}

}  // namespace

bool begin() {
  mtx = xSemaphoreCreateMutex();

  auto cfg = kit.defaultConfig(TX_MODE);
  cfg.sd_active = false;  // we mount the card ourselves (SD_MMC 1-bit)
  cfg.sample_rate = AUDIO_SAMPLE_RATE;
  cfg.channels = 2;
  cfg.bits_per_sample = 16;
  cfg.buffer_count = 8;
  cfg.buffer_size = 512;
  if (!kit.begin(cfg)) log_e("codec init failed (ES8388 not found on I2C 32/33?)");
  kit.setSpeakerActive(false);  // switched on on demand, see amp()
  kit.setVolume(0.7f);
  dec.begin();

#ifdef AUDIO_FROM_SD
  fsOk = SD_MMC.begin("/sdcard", true);  // 1-bit mode: CLK 14, CMD 15, D0 2
  fsys = &SD_MMC;
#else
  fsOk = LittleFS.begin(false);
  fsys = &LittleFS;
#endif
  if (fsOk && !fsys->exists("/sfx/beep.mp3")) {
    log_w("%s mounted but audio files missing", storageName());
    fsOk = false;
  }
  log_i("audio storage %s: %s", storageName(), fsOk ? "ok" : "NOT AVAILABLE");

  // Core 1 alongside loop(), but higher priority. WiFi lives on core 0.
  xTaskCreatePinnedToCore(task, "audio", 8192, nullptr, 5, &taskHandle, 1);
  return fsOk;
}

bool storageOk() { return fsOk; }

const char *storageName() {
#ifdef AUDIO_FROM_SD
  return "SD";
#else
  return "LittleFS";
#endif
}

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

void loop(const char *name) {
  Lock l;
  loopName = name;
  loopsDone = 0;
}

uint32_t loopsCompleted() { return loopsDone; }

void clearLoop() {
  Lock l;
  loopName = "";
}

void stop() {
  toneHz = 0;
  {
    Lock l;
    queue.clear();
    loopName = "";
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

}  // namespace Audio
