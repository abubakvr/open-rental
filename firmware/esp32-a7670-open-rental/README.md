# ESP32 + SIMCom A7670E → Open Rental (MQTT + GNSS + LED)

This is a standalone **ESP-IDF** project. It uses the modem’s **SIM7600-style `AT+CMQTT*`** stack (common on A7670-family firmware; confirm against your module’s *A76XX MQTT* manual).

## Configure

Edit `main/open_rental_main.c` top-of-file macros:

- `MQTT_BROKER_HOST` / `MQTT_BROKER_PORT`
- `MQTT_TOPIC_TELEMETRY` / `MQTT_TOPIC_COMMANDS`
- `CELLULAR_APN` — **required** for your SIM (`"internet"`, `"hologram"`, etc.)
- UART pins / `LED_GPIO_NUM` (default `GPIO_NUM_2`, common devkit LED)

`parse_gps_response()` understands **A7670E `+CGNSSINFO`** extended lines such as  
`3,14,,02,01,6.5275178,N,3.3793924,E,...` as well as the shorter legacy layout. If coordinates never update on the web app, check the serial log for **`GNSS fix:`** and **`Telemetry MQTT publish OK`** after flashing.

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

## Troubleshooting

- **`+CMQTTSTART: 1`** — LTE/EPS packet data was not ready when MQTT started; you often see **`+CGEV: EPS PDN ACT 1`** on the UART **after** the first attempt. The firmware polls **`AT+CGPADDR=1`** until a real IPv4 appears, waits a few seconds, then runs **`CMQTTSTOP` / `CMQTTSTART`** with retries. If it still fails, verify `CELLULAR_APN` and firewall **`1883`** on the broker host.
- **`AT+CGNSSINFO` → `ERROR`** right after power-on — wait for **`+CGNSSPWR: READY`** (the code spends ~10s draining UART after `AT+CGNSSPWR=1`). Antenna outdoors helps first fix.
- **`No > prompt after: AT+CMQTTTOPIC`** — Usually after **`CSQ: 99`**, **`CGNSSINFO` ERROR**, or **`*ATREADY` / `SMS DONE`** when the radio stack recovers: the modem’s **CMQTT session can die** while plain **`AT`** still works. Firmware **publishes MQTT telemetry before each GNSS poll**, spaces out publishes (**~8 s** after success, **~3.5 s** after failure), and **forces a full MQTT reconnect** after **two** failed publishes (then retries every **~20 s** instead of 90 s).

## References

- SIM7500/SIM7600 MQTT AT flow (compatible pattern): [SIM7600 Series MQTT AT Command Manual](https://simcom.ee/documents/SIM7X00/SIM7500_SIM7600_SIM7800%20Series_MQTT_AT%20Command%20Manual_V1.00.pdf)
