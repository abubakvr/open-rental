# ESP32 → Open Rental (MQTT & HTTP)

This guide matches your deployed stack at **[http://51.91.97.149:3909/](http://51.91.97.149:3909/)** and the topic names from `.env` / Docker Compose.

## 1. Network checklist (VM / firewall)

The modem must reach your server over the internet.

| Port | Service        | Must be open on the VM & cloud security group |
|------|----------------|--------------------------------------------------|
| **3909** | Next.js (HTTP) | Yes                                              |
| **1883** | Mosquitto MQTT | Yes (plain MQTT; see security note below)      |

Test from a PC:

```bash
curl -s http://51.91.97.149:3909/api/esp/config
```

You should get JSON with `telemetryTopic`, `commandsTopic`, and `http` paths.

---

## 2. Constants to put in firmware

Use these literals (or fetch once from `GET /api/esp/config` and parse).

| Setting | Value |
|--------|--------|
| **HTTP base URL** | `http://51.91.97.149:3909` |
| **MQTT broker host** | `51.91.97.149` |
| **MQTT broker port** | `1883` |
| **MQTT protocol** | Plain TCP MQTT (`mqtt://`), **not** TLS in the default Docker setup |
| **Telemetry topic** (ESP **publishes** here) | `open-rental/esp/telemetry` |
| **Commands topic** (ESP **subscribes** here) | `open-rental/esp/commands` |

The backend **subscribes** to telemetry and **publishes** commands/events to the commands topic.

---

## 3. Telemetry JSON (what to publish)

Publish **UTF-8 JSON** on `open-rental/esp/telemetry`. The server accepts these fields (aliases in parentheses):

| Field | Type | Required | Notes |
|-------|------|----------|--------|
| `lat` (`latitude`) | number | Yes | Decimal degrees; **north positive, south negative** |
| `lng` (`lon`, `longitude`) | number | Yes | Decimal degrees; **east positive, west negative** |
| `uptimeSec` (`uptime`, `uptime_seconds`) | integer | Recommended | Seconds since reboot |
| `rssi` (`signal`, `wifiRssi`, `signalStrength`) | number | Optional | e.g. cellular/Wi‑Fi RSSI in dBm |

**Example:**

```json
{
  "lat": 9.0765,
  "lng": 7.3986,
  "uptimeSec": 3600,
  "rssi": -85
}
```

**GPS note for your `+CGNSSINFO` parser:** you currently read `latitude`, `lat_dir`, `longitude`, `lon_dir`. Convert to signed decimals before publishing, for example:

- Latitude: if `lat_dir` is `'S'`, use `-fabs(latitude)`; if `'N'`, use `+fabs(latitude)`.
- Longitude: if `lon_dir` is `'W'`, use `-fabs(longitude)`; if `'E'`, use `+fabs(longitude)`.

**CSQ note:** `AT+CSQ` returns a **0–31** (or `99` unknown) RSSI **report**, not true dBm. If you pass it through as `rssi`, pick one convention (raw CSQ or a mapped dBm) and stay consistent; the dashboard only displays the number.

---

## 4. HTTP endpoints (optional but useful)

Base: **`http://51.91.97.149:3909`**

| Method | Path | Purpose |
|--------|------|---------|
| GET | `/api/esp/config` | Topics, port hint, relative paths (good for provisioning) |
| GET | `/api/esp/telemetry` | Last cached telemetry (same data the dashboard uses) |
| POST | `/api/esp/publish` | Body `{"payload":{...}}` publishes JSON to **`open-rental/esp/commands`** (ESP should subscribe there to receive it). Optional `"topic":"..."` overrides the topic. |
| POST | `/api/esp/led` | Body `{"on":true}` or `{"on":false}` sends `{"cmd":"led","on":…}` on the commands topic (use with firmware that handles `cmd: led`). |

**Examples:**

```bash
curl -s http://51.91.97.149:3909/api/esp/config
```

```bash
curl -s -X POST http://51.91.97.149:3909/api/esp/publish \
  -H "Content-Type: application/json" \
  -d '{"payload":{"type":"ping","note":"from server"}}'
```

```bash
curl -s -X POST http://51.91.97.149:3909/api/esp/led \
  -H "Content-Type: application/json" \
  -d '{"on":true}'
```

Reference firmware (ESP-IDF, A7670E UART + `AT+CMQTT*`): see the [`firmware/esp32-a7670-open-rental/`](../firmware/esp32-a7670-open-rental/README.md) folder in this repo.

---

## 5. Relating this to your ESP32 + A7670 (UART) code

Your project talks to the modem with **AT commands** over UART (`modem_uart_init`, `send_at_command`, GNSS via `AT+CGNSSINFO`). MQTT is **not** in that snippet yet; you typically add it in one of these ways:

### Option A — MQTT on the ESP32 (ESP-IDF `mqtt_client`)

1. Bring the modem to a **packet data** PDP context (you already probe `AT+CGATT?`, `AT+CGACT=1,1`, `AT+CGPADDR=1`).
2. Put the link in a mode where the **ESP32** has IP connectivity (often **PPP over UART** to the modem, or vendor-specific “USB/WWAN” bridges). Then use **[ESP-IDF MQTT](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/protocols/mqtt.html)** with `mqtt://51.91.97.149:1883`, subscribe to `open-rental/esp/commands`, publish JSON to `open-rental/esp/telemetry`.

### Option B — MQTT inside the modem (SIMCOM `AT+CMQTT*`)

An ESP-IDF sample using **SIM7600-style `AT+CMQTTSTART` / `AT+CMQTTCONNECT` / `AT+CMQTTSUB` / `AT+CMQTTPUB`** (common on A7670-family firmware) is in [`firmware/esp32-a7670-open-rental/`](../firmware/esp32-a7670-open-rental/README.md). Adjust AT strings to match your module’s **A76XX MQTT** PDF if needed.

### Option C — HTTPS only (no MQTT on device)

Less ideal for bidirectional push, but you could **POST** telemetry to a small API you add later; today the stock backend expects **MQTT for incoming telemetry**. Prefer MQTT for real-time updates.

---

## 6. Minimal integration loop (recommended behavior)

1. After GNSS fix and PDP active: **subscribe** to `open-rental/esp/commands`.
2. On a timer (e.g. every GNSS poll): build JSON with signed `lat`/`lng`, `uptimeSec`, optional `rssi`, **publish** to `open-rental/esp/telemetry`.
3. On incoming MQTT message on the commands topic: parse JSON and act (for example `{"cmd":"led","on":true}` to drive a GPIO).

---

## 7. Security (production)

Default Mosquitto in this repo allows **anonymous** MQTT on **1883** without TLS. That is convenient for lab and first bring-up on a VPS; for anything exposed on the public internet you should move to **TLS (e.g. 8883), passwords or certs**, and restrict firewall sources. Plan that before relying on MQTT for sensitive control.

---

## 8. Quick reference URL list

- App / dashboard: [http://51.91.97.149:3909/](http://51.91.97.149:3909/)
- Config JSON: [http://51.91.97.149:3909/api/esp/config](http://51.91.97.149:3909/api/esp/config)
- Telemetry JSON: [http://51.91.97.149:3909/api/esp/telemetry](http://51.91.97.149:3909/api/esp/telemetry)
- MQTT broker (TCP): `51.91.97.149:1883`
