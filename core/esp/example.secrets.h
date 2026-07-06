/**
  Save this file as secrets.h and fill in your credentials.
*/

#ifndef SECRETS_H
#define SECRETS_H

// ── Wi-Fi credentials ───────────────────────────────────────────────────────
const char* ssid     = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";

// ── Cloudflare Worker WebSocket ─────────────────────────────────────────────
// Your secure key (must match env.ESP_SECURE_KEY on the Worker)
const char* ESP_SECURE_KEY = "your-secret-key-here";

// Full ws:// URL including the Durable Object query param
const char* worker_url = "ws://your-worker-name.your-account.workers.dev/?device=esp32";

// ── Duino-Coin miner credentials ────────────────────────────────────────────
// Your Duino-Coin username (https://wallet.duinocoin.com)
const char* DUCO_USER      = "YOUR_DUCO_USERNAME";

// Your Duino-Coin mining key (set in wallet → Settings → Mining key)
// If you haven't set one, leave it as "None"
const char* DUCO_MINER_KEY = "None";

// Custom miner identifier shown in the wallet  — set to "Auto" to use chip ID
const char* DUCO_RIG_ID    = "Auto";

#endif
