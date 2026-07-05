/**
Save this file as secrets.h with all credentials as mentioned.
*/

#ifndef SECRETS_H
#define SECRETS_H

// Wi-Fi credentials
const char* ssid = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";

// Your secure key for connecting to the Cloudflare Worker (must match env.ESP_SECURE_KEY)
const char* ESP_SECURE_KEY = "command: openssl rand -hex 32";

// The full ws:// URL including the query parameter for the Durable Object instance
const char* worker_url = "ws://your-worker-name.your-account.workers.dev/?device=esp32";

#endif
