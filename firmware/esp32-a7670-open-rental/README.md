# ESP32 + SIMCom A7670E → Open Rental (MQTT + GNSS + LED)

This is a standalone **ESP-IDF** project. It uses the modem’s **SIM7600-style `AT+CMQTT*`** stack (common on A7670-family firmware; confirm against your module’s *A76XX MQTT* manual).

## Configure

Edit `main/open_rental_main.c` top-of-file macros:

- `MQTT_BROKER_HOST` / `MQTT_BROKER_PORT`
- `MQTT_TOPIC_TELEMETRY` / `MQTT_TOPIC_COMMANDS`
- `CELLULAR_APN` — **required** for your SIM (`"internet"`, `"hologram"`, etc.)
- UART pins / `LED_GPIO_NUM` (default `GPIO_NUM_2`, common devkit LED)

## Build & flash

```bash
cd firmware/esp32-a7670-open-rental
idf.py set-target esp32
idf.py build flash monitor
```

## Backend LED API

With Docker/backend running, the dashboard or:

```bash
curl -s -X POST http://YOUR_HOST:3909/api/esp/led \
  -H "Content-Type: application/json" \
  -d '{"on":true}'
```

publishes `{"cmd":"led","on":true}` on `MQTT_TOPIC_COMMANDS`. This firmware subscribes and toggles the GPIO.

## References

- SIM7500/SIM7600 MQTT AT flow (compatible pattern): [SIM7600 Series MQTT AT Command Manual](https://simcom.ee/documents/SIM7X00/SIM7500_SIM7600_SIM7800%20Series_MQTT_AT%20Command%20Manual_V1.00.pdf)
