#include "web.h"

#include <ArduinoJson.h>
#include <AsyncTCP.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#include <esp_wifi.h>

#include "aed.h"
#include "audio.h"
#include "scenarios.h"
#include "web_index.h"

namespace Web {
namespace {

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
DNSServer dns;
QueueHandle_t cmdQueue;
uint8_t volume = 70;
uint32_t lastRev = 0;
uint32_t lastPush = 0;

String configJson() {
  JsonDocument doc;
  doc["type"] = "config";
  doc["storage"] = Audio::storageName();
  JsonArray arr = doc["scenarios"].to<JsonArray>();
  for (size_t i = 0; i < Scenarios::count(); i++) {
    JsonObject o = arr.add<JsonObject>();
    o["name"] = Scenarios::get(i).name;
    o["desc"] = Scenarios::get(i).desc;
  }
  String out;
  serializeJson(doc, out);
  return out;
}

String stateJson() {
  Aed::Status s = Aed::status();
  JsonDocument doc;
  doc["type"] = "state";
  doc["on"] = s.on;
  doc["state"] = (int)s.state;
  doc["label"] = s.label;
  doc["detail"] = s.detail;
  doc["scenario"] = s.scenario;
  doc["shocks"] = s.shocks;
  doc["analyses"] = s.analyses;
  doc["elapsed"] = s.elapsed;
  doc["timer"] = s.timer;
  doc["timerLabel"] = s.timerLabel;
  doc["armed"] = s.armed;
  doc["pads"] = s.pads;
  doc["padsVirtual"] = s.padsVirtual;
  doc["auto"] = s.autoMode;
  doc["forced"] = s.forced;
  doc["volume"] = volume;
  doc["storage"] = Audio::storageOk();
  String out;
  serializeJson(doc, out);
  return out;
}

void push(Cmd c, int16_t v = 0) {
  Command cmd{c, v};
  xQueueSend(cmdQueue, &cmd, 0);
}

void onWsMessage(AsyncWebSocketClient *client, uint8_t *data, size_t len) {
  JsonDocument doc;
  if (deserializeJson(doc, data, len)) return;
  String c = doc["cmd"] | "";
  JsonVariant v = doc["value"];
  if (c == "power") push(Cmd::Power);
  else if (c == "shock") push(Cmd::Shock);
  else if (c == "skip") push(Cmd::Skip);
  else if (c == "pads") push(Cmd::Pads, v.isNull() ? -1 : (v.as<bool>() ? 1 : 0));
  else if (c == "scenario") push(Cmd::Scenario, v | 0);
  else if (c == "auto") push(Cmd::Auto, v.as<bool>() ? 1 : 0);
  else if (c == "volume") push(Cmd::Volume, v | 70);
  else if (c == "force") push(Cmd::Force, v | -1);
  else if (c == "motion") push(Cmd::Motion);
  else if (c == "poorpads") push(Cmd::PoorPads);
  else if (c == "lowbat") push(Cmd::LowBattery);
}

void onWsEvent(AsyncWebSocket *, AsyncWebSocketClient *client, AwsEventType type, void *arg,
               uint8_t *data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    client->text(configJson());
    lastRev = 0;  // force a state push for the new client
  } else if (type == WS_EVT_DATA) {
    auto *info = (AwsFrameInfo *)arg;
    if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT)
      onWsMessage(client, data, len);
  }
}

}  // namespace

void begin(const char *ssid, const char *password) {
  cmdQueue = xQueueCreate(16, sizeof(Command));

  WiFi.mode(WIFI_AP);
  WiFi.softAP(ssid, password);
  WiFi.setSleep(false);
  // Every beacon is a TX burst whose current spike leaks into the speaker
  // amplifier as a tick. Fewer, weaker beacons = fewer, softer ticks; the
  // phone is a few metres away at most.
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  wifi_config_t conf;
  esp_wifi_get_config(WIFI_IF_AP, &conf);
  conf.ap.beacon_interval = 300;  // TU (~1.024 ms), default 100
  esp_wifi_set_config(WIFI_IF_AP, &conf);
  dns.start(53, "*", WiFi.softAPIP());  // captive portal: any name -> us
  MDNS.begin("openaedtrainer");
  MDNS.addService("http", "tcp", 80);

  ws.onEvent(onWsEvent);
  server.addHandler(&ws);
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *req) {
    auto *res = req->beginResponse(200, "text/html", WEB_INDEX_GZ, WEB_INDEX_GZ_LEN);
    res->addHeader("Content-Encoding", "gzip");
    res->addHeader("Cache-Control", "no-cache");
    req->send(res);
  });
  server.on("/api/state", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->send(200, "application/json", stateJson());
  });
  // Captive portal probes (Android/iOS/Windows) and anything else -> UI
  server.onNotFound([](AsyncWebServerRequest *req) {
    req->redirect("http://" + WiFi.softAPIP().toString() + "/");
  });
  server.begin();
  log_i("AP '%s' up, UI at http://%s/ or http://openaedtrainer.local/", ssid, WiFi.softAPIP().toString().c_str());
}

void update() {
  dns.processNextRequest();
  uint32_t now = millis();
  // Push on change, and every second anyway for the timers
  if (Aed::revision() != lastRev || now - lastPush > 1000) {
    lastRev = Aed::revision();
    lastPush = now;
    ws.cleanupClients();
    if (ws.count()) ws.textAll(stateJson());
  }
}

bool nextCommand(Command &out) { return xQueueReceive(cmdQueue, &out, 0) == pdTRUE; }

void log(const String &line) {
  if (!ws.count()) return;
  JsonDocument doc;
  doc["type"] = "log";
  doc["msg"] = line;
  String out;
  serializeJson(doc, out);
  ws.textAll(out);
}

void setVolume(uint8_t percent) {
  volume = percent;
  lastRev = 0;
}

String ip() { return WiFi.softAPIP().toString(); }

}  // namespace Web
