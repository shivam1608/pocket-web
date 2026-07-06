#pragma GCC optimize("-Ofast")

#if defined(ESP32)
  #include <esp_task_wdt.h>
  #include <WiFi.h>
  #include <mbedtls/base64.h>
  #include <HTTPClient.h>
  #include <WiFiClientSecure.h>
#elif defined(ESP8266)
  #include <ESP8266WiFi.h>
  #include <base64.h>
  #include <ESP8266HTTPClient.h>
#endif

#include <ArduinoWebsockets.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <WiFiClient.h>
#include <Ticker.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "DSHA1.h"
#include "Counter.h"

#include "secrets.h"

using namespace websockets;

#define DUCO_SOFTWARE_VERSION "4.3"

volatile unsigned int  mine_hashrate      = 0;
volatile unsigned long mine_share_count   = 0;
volatile unsigned long mine_accepted      = 0;
volatile unsigned int  mine_ping          = 0;
volatile unsigned int  mine_difficulty    = 0;
String                 mine_node_id       = "";
String                 mine_wallet_id     = "";

String duco_host = "";
int    duco_port = 0;

const char base36Chars[36] PROGMEM = {
    '0','1','2','3','4','5','6','7','8','9',
    'A','B','C','D','E','F','G','H','I','J','K','L','M',
    'N','O','P','Q','R','S','T','U','V','W','X','Y','Z'
};
const uint8_t base36CharValues[75] PROGMEM{
    0,1,2,3,4,5,6,7,8,9,0,0,0,0,0,0,0,
    10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,0,0,0,0,0,0,
    10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35
};

static WiFiClient   duco_client;
static String       duco_client_buffer = "";
static String       last_block_hash    = "";
static String       expected_hash_str  = "";
static uint8_t      expected_hash[20];
static uint8_t      hashArray[20];
static DSHA1       *dsha1_ctx         = nullptr;
static String       duco_chip_id      = "";

#if defined(ESP32)
  static String MINER_BANNER = "Official ESP32 Miner";
#else
  static String MINER_BANNER = "Official ESP8266 Miner";
#endif

WebsocketsClient ws_client;

String getContentType(const String &filename) {
  if (filename.endsWith(".html"))                return "text/html";
  if (filename.endsWith(".css"))                 return "text/css";
  if (filename.endsWith(".js"))                  return "application/javascript";
  if (filename.endsWith(".json"))                return "application/json";
  if (filename.endsWith(".png"))                 return "image/png";
  if (filename.endsWith(".gif"))                 return "image/gif";
  if (filename.endsWith(".jpg") || filename.endsWith(".jpeg")) return "image/jpeg";
  if (filename.endsWith(".ico"))                 return "image/x-icon";
  if (filename.endsWith(".svg"))                 return "image/svg+xml";
  if (filename.endsWith(".xml"))                 return "text/xml";
  return "text/plain";
}


static uint8_t *hexToUint8(const String &hex, uint8_t *out, uint32_t len) {
  const char *h = hex.c_str();
  for (uint32_t i = 0; i < len; i++) {
    out[i] = (pgm_read_byte(base36CharValues + h[i*2]   - '0') << 4)
           +  pgm_read_byte(base36CharValues + h[i*2+1] - '0');
  }
  return out;
}

static void duco_waitForData() {
  duco_client_buffer = "";
  unsigned long t = millis();
  while (duco_client.connected()) {
    if (duco_client.available()) {
      duco_client_buffer = duco_client.readStringUntil('\n');
      break;
    }
    if (millis() - t > 60000) {
      Serial.println("[MINER] Timeout waiting for server data. Restarting...");
      ESP.restart();
    }
    delay(1);
  }
}

static bool duco_parse() {
  char *buf = strdup(duco_client_buffer.c_str());
  if (!buf) return false;
  String tokens[3];
  char *tok = strtok(buf, ",");
  for (int i = 0; tok && i < 3; i++) { tokens[i] = tok; tok = strtok(nullptr, ","); }
  free(buf);
  last_block_hash   = tokens[0];
  expected_hash_str = tokens[1];
  hexToUint8(expected_hash_str, expected_hash, 20);
  mine_difficulty   = tokens[2].toInt() * 100 + 1;
  return true;
}

static void duco_connectToNode() {
  if (duco_client.connected()) return;
  Serial.println("[MINER] Connecting to Duino-Coin node " + duco_host + ":" + String(duco_port));
  unsigned long t = millis();
  while (!duco_client.connect(duco_host.c_str(), duco_port)) {
    if (millis() - t > 30000) { ESP.restart(); }
    delay(100);
  }
  duco_waitForData();
  Serial.println("[MINER] Node version: " + duco_client_buffer);
}

static void duco_askForJob() {
  duco_client.print(String("JOB,") + DUCO_USER + "," + "ESP32" + "," + DUCO_MINER_KEY + "\n");
  duco_waitForData();
  duco_parse();
}

static void duco_submit(unsigned long result, float hashrate, float elapsed_s) {
  duco_client.print(String(result) + "," + String(hashrate) + ","
                    + MINER_BANNER + " " + DUCO_SOFTWARE_VERSION + ","
                    + DUCO_RIG_ID + ","
                    + "DUCOID" + duco_chip_id + ","
                    + mine_wallet_id + "\n");
  unsigned long ping_start = millis();
  duco_waitForData();
  mine_ping = millis() - ping_start;
  if (duco_client_buffer == "GOOD") mine_accepted++;
  Serial.printf("[MINER] %s | share #%lu | result %lu | %.2f kH/s | ping %ums\n",
                duco_client_buffer.c_str(), mine_share_count,
                result, hashrate / 1000.0, mine_ping);
}

static void duco_mine_once() {
  duco_connectToNode();
  duco_askForJob();

  dsha1_ctx->reset().write((const unsigned char *)last_block_hash.c_str(), last_block_hash.length());

  unsigned long start_us = micros();
  unsigned long last_wdt_reset = micros();

  for (Counter<10> counter; counter < mine_difficulty; ++counter) {
    DSHA1 ctx = *dsha1_ctx;
    ctx.write((const unsigned char *)counter.c_str(), counter.strlen()).finalize(hashArray);

    // Feed the Task Watchdog every ~100ms so IDLE0 doesn't starve
    unsigned long now_us = micros();
    if ((now_us - last_wdt_reset) > 100000UL) {
      last_wdt_reset = now_us;
      esp_task_wdt_reset();
      vTaskDelay(1); // briefly yield to IDLE task
    }

    if (memcmp(expected_hash, hashArray, 20) == 0) {
      unsigned long elapsed_us = micros() - start_us;
      float elapsed_s = elapsed_us * 0.000001f;
      mine_share_count++;
      mine_hashrate = (unsigned int)(counter / elapsed_s);
      duco_submit(counter, (float)mine_hashrate, elapsed_s);
      break;
    }
  }
}

static String httpGetString(const String &url) {
  String payload = "";
#if defined(ESP32)
  WiFiClientSecure sc;
  sc.setInsecure();
  HTTPClient https;
  https.begin(sc, url);
  https.addHeader("Accept", "*/*");
  int code = https.GET();
  if (code == HTTP_CODE_OK || code == HTTP_CODE_MOVED_PERMANENTLY)
    payload = https.getString();
  else
    Serial.printf("[MINER] Pool picker HTTP error: %d\n", code);
  https.end();
#endif
  return payload;
}

static void selectDucoNode() {
  String input = "";
  int wait = 1;
  while (input == "") {
    Serial.println("[MINER] Fetching Duino-Coin node in " + String(wait) + "s...");
    delay(wait * 1000);
    input = httpGetString("https://server.duinocoin.com/getPool");
    wait *= 2;
    if (wait > 32) { Serial.println("[MINER] Pool unavailable, restarting"); ESP.restart(); }
  }
  JsonDocument doc;
  deserializeJson(doc, input);
  duco_host    = doc["ip"].as<String>();
  duco_port    = doc["port"].as<int>();
  mine_node_id = doc["name"].as<String>();
  Serial.println("[MINER] Pool: " + mine_node_id + " @ " + duco_host + ":" + String(duco_port));
}

static void generateChipId() {
#if defined(ESP8266)
  duco_chip_id = String(ESP.getChipId(), HEX);
#else
  uint64_t mac = ESP.getEfuseMac();
  uint16_t hi  = (uint16_t)(mac >> 32);
  char buf[23];
  snprintf(buf, 23, "%04X%08X", hi, (uint32_t)mac);
  duco_chip_id = String(buf);
#endif
  duco_chip_id.toUpperCase();

  if (String(DUCO_RIG_ID) == "Auto") {
    String autoId = "ESP32-" + duco_chip_id;
    autoId.toUpperCase();
    Serial.println("[MINER] Auto rig ID: " + autoId);
  }
}

#if defined(ESP32)

void minerTask(void *param) {
  Serial.println("[MINER] Task started on core " + String(xPortGetCoreID()));

  // Subscribe THIS task to the Task Watchdog Timer so esp_task_wdt_reset() works
  esp_task_wdt_add(NULL);

  dsha1_ctx = new DSHA1();
  dsha1_ctx->warmup();
  mine_wallet_id = String(random(0, 2811));

  selectDucoNode();

  while (true) {
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[MINER] WiFi lost, waiting...");
      esp_task_wdt_reset();
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }
    duco_mine_once();
    // vTaskDelay lets IDLE0 run between shares — keeps Core 0 WDT happy
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}


TaskHandle_t minerTaskHandle = nullptr;

#endif 

void onWsMessage(WebsocketsMessage message) {
  JsonDocument doc;
  deserializeJson(doc, message.data());
  String reqId  = doc["reqId"].as<String>();
  String action = doc["action"].as<String>();

  if (action != "getFile") return;

  String path = doc["path"].as<String>();
  Serial.println("[WS] Requested file: " + path);

  if (path == "/" || path == "") path = "/index.html";

  bool   isGzip    = false;
  String actualPath = path;

  if (LittleFS.exists(path + ".gz")) {
    actualPath = path + ".gz";
    isGzip = true;
  } else if (!LittleFS.exists(actualPath)) {
    if (LittleFS.exists("/index.html.gz")) {
      actualPath = "/index.html.gz"; isGzip = true; path = "/index.html";
    } else if (LittleFS.exists("/index.html")) {
      actualPath = "/index.html"; path = "/index.html";
    } else {
      JsonDocument rep; rep["reqId"] = reqId; rep["status"] = 404;
      String s; serializeJson(rep, s); ws_client.send(s);
      Serial.println("[WS] 404: " + path);
      return;
    }
  }

  File file = LittleFS.open(actualPath, "r");
  if (!file) {
    JsonDocument rep; rep["reqId"] = reqId; rep["status"] = 500;
    String s; serializeJson(rep, s); ws_client.send(s);
    return;
  }

  {
    JsonDocument rep;
    rep["reqId"]       = reqId;
    rep["status"]      = 200;
    rep["action"]      = "fileStart";
    rep["contentType"] = getContentType(path);
    rep["isGzip"]      = isGzip;
    String s; serializeJson(rep, s); ws_client.send(s);
  }

  const size_t BUF = 1023;
  uint8_t *buf = (uint8_t *)malloc(BUF);
  if (buf) {
    while (file.available()) {
      size_t bytesRead = file.read(buf, BUF);
      JsonDocument rep;
      rep["reqId"]  = reqId;
      rep["action"] = "fileChunk";
#if defined(ESP32)
      size_t outLen;
      mbedtls_base64_encode(nullptr, 0, &outLen, buf, bytesRead);
      unsigned char *b64 = (unsigned char *)malloc(outLen + 1);
      if (b64) {
        mbedtls_base64_encode(b64, outLen, &outLen, buf, bytesRead);
        b64[outLen] = '\0';
        rep["chunk"] = (char *)b64;
        String s; serializeJson(rep, s); ws_client.send(s);
        free(b64);
      }
#elif defined(ESP8266)
      rep["chunk"] = base64::encode(buf, bytesRead);
      String s; serializeJson(rep, s); ws_client.send(s);
#endif
      ws_client.poll();
      delay(2);
    }
    free(buf);
  }
  file.close();

  {
    JsonDocument rep;
    rep["reqId"]  = reqId;
    rep["action"] = "fileEnd";
    String s; serializeJson(rep, s); ws_client.send(s);
  }
  Serial.println("[WS] Sent: " + actualPath);
}


void setup() {
  Serial.begin(115200);
  Serial.println("\n\n[BOOT] ESP32 Dual-Core: WebSocket Server + Duino-Coin Miner");

  if (!LittleFS.begin(true)) {
    Serial.println("[FS] LittleFS mount failed!");
  } else {
    Serial.println("[FS] LittleFS mounted OK");
  }

#if defined(ESP32)
  WiFi.setSleep(false);
#elif defined(ESP8266)
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
#endif
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.print("[WiFi] Connecting");
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.println("\n[WiFi] Connected — IP: " + WiFi.localIP().toString());

#if defined(ESP32)
  setCpuFrequencyMhz(240);
  Serial.println("[CPU] Clock set to 240 MHz");
#endif

  generateChipId();
  Serial.println("[MINER] Chip ID: " + duco_chip_id);

  ws_client.onMessage(onWsMessage);

  String ws_url = String(worker_url) + "&key=" + ESP_SECURE_KEY;
  Serial.print("[WS] Connecting to Worker... ");
  if (ws_client.connect(ws_url.c_str())) {
    Serial.println("OK");
  } else {
    Serial.println("FAILED (will retry in loop)");
  }

#if defined(ESP32)
  xTaskCreatePinnedToCore(
    minerTask,        // function
    "DUCOMiner",      // name
    16384,            // stack (bytes) — mining needs a bit of stack
    nullptr,          // param
    1,                // priority
    &minerTaskHandle, // handle
    0                 // pin to Core 0
  );
  Serial.println("[MINER] Task pinned to Core 0");
#endif
}
void loop() {
  if (ws_client.available()) {
    ws_client.poll();
  } else {
    Serial.println("[WS] Disconnected. Reconnecting...");
    String ws_url = String(worker_url) + "&key=" + ESP_SECURE_KEY;
    ws_client.connect(ws_url.c_str());
    delay(3000);
  }
}