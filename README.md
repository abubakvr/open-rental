This is a [Next.js](https://nextjs.org) app (**Open Rental**) with MQTT telemetry from an ESP and a small dashboard.

## Local development

```bash
npm install
npm run dev
```

Open [http://localhost:3909](http://localhost:3909). Run an MQTT broker locally (for example Mosquitto on port 1883), copy `.env.example` to `.env.local`, set `MQTT_URL` (for example `mqtt://127.0.0.1:1883`), then hit `/api/esp/telemetry` once or open `/dashboard` so the server subscribes.

## Docker Compose (app + Eclipse Mosquitto)

From the project root:

```bash
docker compose up --build
```

- **Web app:** [http://localhost:3909](http://localhost:3909) (host port **3909** → container **3000**)
- **MQTT broker:** port **1883** on the host (`localhost:1883` from your PC).

Inside Compose, the Next.js container uses `MQTT_URL=mqtt://mosquitto:1883`. Your **ESP is not on the Docker network**, so it must use your **computer’s LAN IP** (for example `192.168.1.50`) and port **1883**, not the hostname `mosquitto`.

### Topics (defaults)

| Direction | Topic | Who |
|-----------|--------|-----|
| ESP → broker → backend | `open-rental/esp/telemetry` | ESP **publishes** JSON telemetry; the backend **subscribes** |
| Backend → broker → ESP | `open-rental/esp/commands` | Backend **publishes** events; ESP **subscribes** |

Override with `MQTT_TOPIC_TELEMETRY` and `MQTT_TOPIC_COMMANDS` in Compose or `.env.local`.

### HTTP endpoints for the ESP or other clients

- `GET /api/esp/config` — topic names, paths, and hints (no secrets).
- `GET /api/esp/telemetry` — last telemetry JSON stored from MQTT.
- `POST /api/esp/publish` — body `{ "payload": { ... } }` publishes JSON to the commands topic (optional `"topic": "custom/topic"`).

Example publish (from your LAN, replace the IP):

```bash
curl -s -X POST http://192.168.1.50:3909/api/esp/publish \
  -H "Content-Type: application/json" \
  -d '{"payload":{"type":"ping","sentAt":"2026-05-10T12:00:00Z"}}'
```

### ESP firmware checklist

1. Connect MQTT to `<YOUR_PC_LAN_IP>:1883` (same Wi‑Fi as the broker).
2. **Publish** telemetry JSON to `open-rental/esp/telemetry` (fields such as `lat`, `lng`, `uptimeSec`, `rssi`).
3. **Subscribe** to `open-rental/esp/commands` to receive events from the backend.
4. Optionally call `GET http://<YOUR_PC_LAN_IP>:3909/api/esp/config` to read topic names and paths at runtime.

### Mosquitto configuration

Dev-friendly settings live in `docker/mosquitto/mosquitto.conf` (anonymous access enabled). **Do not expose port 1883 to the internet** without TLS and authentication.

## Learn More

- [Next.js Documentation](https://nextjs.org/docs)
- [Eclipse Mosquitto](https://mosquitto.org/)
