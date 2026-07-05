#if defined(ESP32)
  #include <WiFi.h>
  #include <mbedtls/base64.h>
#elif defined(ESP8266)
  #include <ESP8266WiFi.h>
  #include <base64.h>
#endif

#include <ArduinoWebsockets.h>
#include <ArduinoJson.h>
#include <LittleFS.h>

#include "secrets.h"

using namespace websockets;
WebsocketsClient client;

String getContentType(String filename) {
  if (filename.endsWith(".html")) return "text/html";
  else if (filename.endsWith(".css")) return "text/css";
  else if (filename.endsWith(".js")) return "application/javascript";
  else if (filename.endsWith(".json")) return "application/json";
  else if (filename.endsWith(".png")) return "image/png";
  else if (filename.endsWith(".gif")) return "image/gif";
  else if (filename.endsWith(".jpg") || filename.endsWith(".jpeg")) return "image/jpeg";
  else if (filename.endsWith(".ico")) return "image/x-icon";
  else if (filename.endsWith(".svg")) return "image/svg+xml";
  else if (filename.endsWith(".xml")) return "text/xml";
  return "text/plain";
}

void setup() {
  Serial.begin(115200);

  if(!LittleFS.begin(true)){
    Serial.println("An Error has occurred while mounting LittleFS");
  } else {
    Serial.println("LittleFS mounted successfully");
  }

  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nConnected!");

  client.onMessage([&](WebsocketsMessage message) {
    JsonDocument doc;
    deserializeJson(doc, message.data());
    String reqId = doc["reqId"];
    String action = doc["action"];

    if (action == "getFile") {
      String path = doc["path"].as<String>();
      Serial.println("Requested file: " + path);
      
      if (path == "/" || path == "") {
        path = "/index.html";
      }

      bool isGzip = false;
      String actualPath = path;

      if (LittleFS.exists(path + ".gz")) {
        actualPath = path + ".gz";
        isGzip = true;
      } else if (!LittleFS.exists(actualPath)) {
        if (LittleFS.exists("/index.html.gz")) {
          actualPath = "/index.html.gz";
          isGzip = true;
          path = "/index.html";
        } else if (LittleFS.exists("/index.html")) {
          actualPath = "/index.html";
          path = "/index.html";
        } else {
          JsonDocument replyDoc;
          replyDoc["reqId"] = reqId;
          replyDoc["status"] = 404;
          String replyString;
          serializeJson(replyDoc, replyString);
          client.send(replyString);
          Serial.println("File not found: " + path);
          return;
        }
      }

      File file = LittleFS.open(actualPath, "r");
      if (!file) {
        JsonDocument replyDoc;
        replyDoc["reqId"] = reqId;
        replyDoc["status"] = 500;
        String replyString;
        serializeJson(replyDoc, replyString);
        client.send(replyString);
        return;
      }

      String contentType = getContentType(path);

      {
        JsonDocument replyDoc;
        replyDoc["reqId"] = reqId;
        replyDoc["status"] = 200;
        replyDoc["action"] = "fileStart";
        replyDoc["contentType"] = contentType;
        replyDoc["isGzip"] = isGzip;
        String replyString;
        serializeJson(replyDoc, replyString);
        client.send(replyString);
      }

      // Read and send in chunks
      const size_t bufferSize = 1023;
      uint8_t *buffer = (uint8_t*)malloc(bufferSize);
      if(buffer) {
        while(file.available()) {
          size_t bytesRead = file.read(buffer, bufferSize);
          
          JsonDocument replyDoc;
          replyDoc["reqId"] = reqId;
          replyDoc["action"] = "fileChunk";
        #if defined(ESP32)
          size_t outputLength;
          mbedtls_base64_encode(nullptr, 0, &outputLength, buffer, bytesRead);
          unsigned char* base64Buffer = (unsigned char*)malloc(outputLength + 1);
          
          if(base64Buffer) {
            mbedtls_base64_encode(base64Buffer, outputLength, &outputLength, buffer, bytesRead);
            base64Buffer[outputLength] = '\0';
            replyDoc["chunk"] = (char*)base64Buffer;
            
            String replyString;
            serializeJson(replyDoc, replyString);
            client.send(replyString);
            free(base64Buffer);
          }
        #elif defined(ESP8266)
          String base64Chunk = base64::encode(buffer, bytesRead);
          replyDoc["chunk"] = base64Chunk;
          
          String replyString;
          serializeJson(replyDoc, replyString);
          client.send(replyString);
        #endif
          client.poll();
          delay(2);
        }
      }
      file.close();

      {
        JsonDocument replyDoc;
        replyDoc["reqId"] = reqId;
        replyDoc["action"] = "fileEnd";
        String replyString;
        serializeJson(replyDoc, replyString);
        client.send(replyString);
      }
      
      Serial.println("Sent file successfully: " + actualPath);
    }
  });

  Serial.print("Connecting to Cloudflare Worker... ");
  String connection_url = String(worker_url) + "&key=" + ESP_SECURE_KEY;
  bool connected = client.connect(connection_url.c_str());
  if (connected) {
    Serial.println("Connected successfully!");
  } else {
    Serial.println("Failed to connect.");
  }
}

void loop() {
  if (client.available()) {
    client.poll();
  } else {
    Serial.println("Disconnected! Reconnecting...");
    String connection_url = String(worker_url) + "&key=" + ESP_SECURE_KEY;
    client.connect(connection_url.c_str());
    delay(3000);
  }
}