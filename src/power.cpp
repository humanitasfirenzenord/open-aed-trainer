#include "power.h"

#include <WiFi.h>
#include <driver/rtc_io.h>
#include <esp_sleep.h>

#include "aed.h"
#include "audio.h"
#include "display.h"
#include "io.h"
#include "pins.h"

namespace Power {
namespace {

constexpr uint32_t STANDBY_MS = 5 * 60 * 1000UL;   // off, nobody using it -> sleep
constexpr uint32_t AUTO_OFF_MS = 15 * 60 * 1000UL; // on, untouched -> power off + sleep
constexpr uint32_t QUIET_TIMEOUT_MS = 5000;         // max wait for the last sound

uint32_t lastActivity = 0;
bool sleepPending = false;
uint32_t sleepRequestedAt = 0;

void sleepNow() {
  log_i("going to deep sleep");
  Display::clear();
  for (int i = 0; i < (int)IO::Led::COUNT; i++) IO::setLed((IO::Led)i, IO::Pattern::Off);
  IO::update();
  Audio::shutdown();
  WiFi.mode(WIFI_OFF);
  Serial.flush();

  // Keep the speaker amplifier disabled while asleep
  pinMode(21, OUTPUT);
  digitalWrite(21, LOW);
  gpio_hold_en(GPIO_NUM_21);
  gpio_deep_sleep_hold_en();

  // Wake on POWER (ext0) or SHOCK (ext1 with a single pin = "this pin low").
  // Both are active-low and get the RTC pull-up while asleep.
#if PIN_BTN_POWER >= 0
#if PIN_BTN_POWER < 34  // 34-39 have no internal pulls (GPIO36 has the board's)
  rtc_gpio_pullup_en((gpio_num_t)PIN_BTN_POWER);
  rtc_gpio_pulldown_dis((gpio_num_t)PIN_BTN_POWER);
#endif
  esp_sleep_enable_ext0_wakeup((gpio_num_t)PIN_BTN_POWER, 0);
#endif
#if PIN_BTN_SHOCK >= 0
  rtc_gpio_pullup_en((gpio_num_t)PIN_BTN_SHOCK);
  rtc_gpio_pulldown_dis((gpio_num_t)PIN_BTN_SHOCK);
  esp_sleep_enable_ext1_wakeup(1ULL << PIN_BTN_SHOCK, ESP_EXT1_WAKEUP_ALL_LOW);
#endif
  esp_deep_sleep_start();
}

}  // namespace

Wake begin() {
  gpio_hold_dis(GPIO_NUM_21);  // release the amplifier pin held during sleep
  lastActivity = millis();
  switch (esp_sleep_get_wakeup_cause()) {
    case ESP_SLEEP_WAKEUP_EXT0: return Wake::PowerButton;
    case ESP_SLEEP_WAKEUP_EXT1: return Wake::ShockButton;
    case ESP_SLEEP_WAKEUP_UNDEFINED: return Wake::ColdBoot;
    default: return Wake::Other;
  }
}

void activity() { lastActivity = millis(); }

void sleepWhenQuiet() {
  sleepPending = true;
  sleepRequestedAt = millis();
}

void update(bool trainerOn) {
  uint32_t idle = millis() - lastActivity;

  if (trainerOn) {
    sleepPending = false;  // turned back on before we got to sleep
    if (idle > AUTO_OFF_MS) {
      log_i("no activity for %u min: auto power off", AUTO_OFF_MS / 60000);
      Aed::power();
      sleepWhenQuiet();
    }
    return;
  }

  if (!sleepPending && idle > STANDBY_MS) {
    log_i("standby for %u min: sleeping", STANDBY_MS / 60000);
    Audio::play("sfx/power_off");
    sleepWhenQuiet();
  }
  if (sleepPending && (Audio::quiet() || millis() - sleepRequestedAt > QUIET_TIMEOUT_MS))
    sleepNow();
}

}  // namespace Power
