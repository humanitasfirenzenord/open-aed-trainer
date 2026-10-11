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
volatile bool trainerOn = false;  // custom prompts can only be changed while off

// Custom voice prompts (uploaded MP3s, already converted by the browser).
// One upload at a time: there is one instructor.
constexpr size_t MAX_UPLOAD = 256 * 1024;  // ~30 s at 64 kbps, the UI caps far lower
File upload;
String uploadErr;

// "it/analysing": a voice prompt that exists in the default set
bool validName(const String &n) {
  int slash = n.indexOf('/');
  if (slash < 1 || n.indexOf('/', slash + 1) >= 0 || n.length() > 48) return false;
  for (size_t i = 0; i < n.length(); i++) {
    char c = n[i];
    if (!(islower(c) || isdigit(c) || c == '_' || c == '/')) return false;
  }
  return Audio::exists(Audio::defaultPath(n));
}

String promptsJson() {
  fs::FS &fs = Audio::storage();
  const String &lang = Audio::language();
  JsonDocument doc;
  doc["lang"] = lang;
  doc["storage"] = Audio::storageOk();
  doc["free"] = Audio::storageOk() ? Audio::freeBytes() : 0;
  doc["locked"] = trainerOn;
  JsonArray arr = doc["prompts"].to<JsonArray>();
  if (Audio::storageOk()) {
    File dir = fs.open("/" + lang);
    for (File f = dir ? dir.openNextFile() : File(); f; f = dir.openNextFile()) {
      String n = f.name();
      n = n.substring(n.lastIndexOf('/') + 1);
      if (f.isDirectory() || !n.endsWith(".mp3")) continue;
      String id = n.substring(0, n.length() - 4);
      JsonObject o = arr.add<JsonObject>();
      o["id"] = id;
      o["custom"] = Audio::exists(Audio::customPath(lang + "/" + id));
    }
  }
  String out;
  serializeJson(doc, out);
  return out;
}

void onUpload(AsyncWebServerRequest *req, const String &, size_t index, uint8_t *data, size_t len, bool final) {
  fs::FS &fs = Audio::storage();
  String name = req->arg("name");
  String tmp = Audio::customPath(name) + ".tmp";
  if (index == 0) {
    if (upload) upload.close();  // a previous upload that never finished
    uploadErr = "";
    if (trainerOn) uploadErr = "Spegnere il trainer prima di modificare i messaggi";
    else if (!Audio::storageOk() || !validName(name)) uploadErr = "Messaggio sconosciuto";
    else if (req->contentLength() > MAX_UPLOAD) uploadErr = "File troppo grande";
    else if (req->contentLength() + 16 * 1024 > Audio::freeBytes()) uploadErr = "Spazio esaurito";
    if (uploadErr.length()) return;
    String dir = "/custom";
    fs.mkdir(dir);
    fs.mkdir(dir + "/" + name.substring(0, name.indexOf('/')));
    upload = fs.open(tmp, FILE_WRITE);
    if (!upload) {
      uploadErr = "Impossibile scrivere il file";
      return;
    }
  }
  if (uploadErr.length() || !upload) return;
  if (len && upload.write(data, len) != len) {
    upload.close();
    fs.remove(tmp);
    uploadErr = "Scrittura non riuscita (spazio esaurito?)";
    return;
  }
  if (final) {
    upload.close();
    String dst = Audio::customPath(name);
    if (trainerOn) {  // switched on during the upload: don't touch a file it may be playing
      fs.remove(tmp);
      uploadErr = "Spegnere il trainer prima di modificare i messaggi";
      return;
    }
    if (Audio::exists(dst)) fs.remove(dst);
    if (!fs.rename(tmp, dst)) uploadErr = "Impossibile salvare il file";
    else log_i("custom prompt %s: %u bytes", name.c_str(), (unsigned)(index + len));
  }
}

String configJson() {
  JsonDocument doc;
  doc["type"] = "config";
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
  server.on("/lame.min.js", HTTP_GET, [](AsyncWebServerRequest *req) {
    auto *res = req->beginResponse(200, "text/javascript", WEB_LAME_GZ, WEB_LAME_GZ_LEN);
    res->addHeader("Content-Encoding", "gzip");
    res->addHeader("Cache-Control", "max-age=86400");
    req->send(res);
  });

  // Custom voice prompts: list, upload, revert to default, listen
  server.on("/api/prompts", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->send(200, "application/json", promptsJson());
  });
  server.on("/api/upload", HTTP_POST, [](AsyncWebServerRequest *req) {
    if (uploadErr.length()) req->send(400, "text/plain", uploadErr);
    else req->send(200, "application/json", promptsJson());
  }, onUpload);
  server.on("/api/prompts", HTTP_DELETE, [](AsyncWebServerRequest *req) {
    String name = req->arg("name");
    if (trainerOn) return req->send(409, "text/plain", "Spegnere il trainer prima di modificare i messaggi");
    if (!Audio::storageOk() || !validName(name)) return req->send(400, "text/plain", "Messaggio sconosciuto");
    fs::FS &fs = Audio::storage();
    if (Audio::exists(Audio::customPath(name))) fs.remove(Audio::customPath(name));
    log_i("custom prompt %s removed", name.c_str());
    req->send(200, "application/json", promptsJson());
  });
  server.on("/prompt.mp3", HTTP_GET, [](AsyncWebServerRequest *req) {
    String name = req->arg("name");
    if (!Audio::storageOk() || !validName(name)) return req->send(404);
    fs::FS &fs = Audio::storage();
    String path = Audio::customPath(name);
    if (req->hasArg("default") || !Audio::exists(path)) path = Audio::defaultPath(name);
    auto *res = req->beginResponse(fs, path, "audio/mpeg");
    res->addHeader("Cache-Control", "no-store");
    req->send(res);
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
    trainerOn = Aed::status().on;
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
