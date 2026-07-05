import offlinePageHtml from "./pages/offline.html";
import timeoutPageHtml from "./pages/timeout.html";

export default {
  async fetch(request, env) {
    const id = env.ESP_HUB.idFromName("ESP_INSTANCE");
    const stub = env.ESP_HUB.get(id);

    return await stub.fetch(request);
  }
};

export class EspHub {
  constructor(state, env) {
    this.state = state;
    this.env = env;
    this.espSocket = null;
    this.pendingRequests = new Map();
  }

  async fetch(request) {
    const url = new URL(request.url);
    const upgradeHeader = request.headers.get('Upgrade');

    if (upgradeHeader === 'websocket') {
      if (url.searchParams.get("device") === "esp32") {
        const expectedKey = this.env.ESP_CONNECTION_KEY;
        if (!expectedKey) {
          console.error("Missing ESP_CONNECTION_KEY in environment variables!");
          return new Response("Server Configuration Error", { status: 500 });
        }

        const providedKey = url.searchParams.get("key");

        if (providedKey !== expectedKey) {
          console.warn("Unauthorized ESP32 connection attempt");
          return new Response("Unauthorized", { status: 401 });
        }

        const [client, server] = Object.values(new WebSocketPair());
        server.accept();

        this.espSocket = server;
        console.log("ESP32 successfully connected.");

        server.addEventListener('message', (event) => {
          try {
            const data = JSON.parse(event.data);
            const reqData = this.pendingRequests.get(data.reqId);
            if (!reqData) return;

            if (data.status === 404 || data.status === 500) {
              reqData.resolve(new Response(data.status === 404 ? "Not Found" : "Server Error", { status: data.status }));
              this.pendingRequests.delete(data.reqId);
              return;
            }

            if (data.action === "fileStart") {
              reqData.chunks = [];
              reqData.contentType = data.contentType;
              reqData.isGzip = data.isGzip;
            } else if (data.action === "fileChunk") {
              reqData.chunks.push(data.chunk);
            } else if (data.action === "fileEnd") {
              const completeBase64 = reqData.chunks.join('');
              const binaryStr = atob(completeBase64);
              const buffer = new Uint8Array(binaryStr.length);
              for (let i = 0; i < binaryStr.length; i++) {
                buffer[i] = binaryStr.charCodeAt(i);
              }

              reqData.resolve(new Response(buffer, {
                status: 200,
                headers: {
                  'Content-Type': reqData.contentType,
                  ...(reqData.isGzip && { 'Content-Encoding': 'gzip' })
                }
              }));
              this.pendingRequests.delete(data.reqId);
            } else if (data.html) {
              reqData.resolve(new Response(data.html, {
                status: 200,
                headers: { 'Content-Type': 'text/html' }
              }));
              this.pendingRequests.delete(data.reqId);
            }
          } catch (e) {
            console.error("Failed to process message:", e); if (reqData) reqData.resolve(new Response(e.toString(), { status: 500 }));
          }
        });

        server.addEventListener('close', () => {
          if (this.espSocket === server) {
            this.espSocket = null;
            console.log("ESP32 severed WebSocket connection.");
          }
        });

        return new Response(null, { status: 101, webSocket: client });
      }

      return new Response("Bad Request: Expected device=esp32", { status: 400 });
    }

    // HTTP REQUESTS HANDLER
    if (!this.espSocket) {
      return new Response(offlinePageHtml, {
        status: 503,
        headers: { 'Content-Type': 'text/html' }
      });
    }

    // Normalize path for Astro static output:
    //   /        → /index.html
    //   /why     → /why/index.html
    //   /how     → /how/index.html
    //   /style.css, /_astro/… → unchanged (has an extension)
    let filePath = url.pathname;
    if (!filePath.includes('.')) {
      filePath = filePath.replace(/\/?$/, '/index.html');
    }

    const reqId = crypto.randomUUID();

    const fileResponse = new Promise((resolve) => {
      this.pendingRequests.set(reqId, { resolve });

      this.espSocket.send(JSON.stringify({
        reqId,
        action: "getFile",
        path: filePath,
      }));

      setTimeout(() => {
        if (this.pendingRequests.has(reqId)) {
          this.pendingRequests.delete(reqId);
          resolve(new Response(timeoutPageHtml, {
            status: 504,
            headers: { 'Content-Type': 'text/html' }
          }));
        }
      }, 15000);
    });

    return await fileResponse;
  }
}
