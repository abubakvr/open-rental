/*
 * ESP32 + SIMCom A7670E — cellular MQTT (SIM7600-style AT+CMQTT*), GNSS telemetry,
 * and LED control via backend MQTT commands.
 *
 * Backend publishes: {"cmd":"led","on":true|false} on MQTT_TOPIC_COMMANDS.
 * This device subscribes and toggles LED_GPIO_NUM.
 *
 * MQTT AT syntax follows SIM7500/SIM7600 MQTT AT Command Manual (many A7670 builds are compatible).
 * If a command fails, compare your module’s “A76XX MQTT” PDF and adjust the strings below.
 *
 * Build: ESP-IDF v5.x, target esp32 — see ../README.md in this firmware folder.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "OPEN_RENTAL";

/* --------- User configuration --------- */

#define MQTT_BROKER_HOST "51.91.97.149"
#define MQTT_BROKER_PORT 1883

#define MQTT_TOPIC_TELEMETRY "open-rental/esp/telemetry"
#define MQTT_TOPIC_COMMANDS "open-rental/esp/commands"

#define MQTT_CLIENT_ID "esp32-open-rental"

/** Replace with your carrier APN (quotes inside string only where AT needs them). */
#define CELLULAR_APN "internet"

#define MODEM_UART_NUM UART_NUM_2
#define MODEM_TX_PIN GPIO_NUM_17
#define MODEM_RX_PIN GPIO_NUM_16
#define MODEM_BAUDRATE 115200

#define MODEM_PWRKEY GPIO_NUM_4

/** Onboard LED on many ESP32 devkits; change if your LED is elsewhere. */
#define LED_GPIO_NUM GPIO_NUM_2

#define GNSS_POLL_INTERVAL_MS 5000

#define MQTT_CLIENT_INDEX 0
#define MQTT_QOS_SUBSCRIBE 1
#define MQTT_QOS_PUBLISH 0
#define MQTT_PUBLISH_TIMEOUT_SEC 60
#define MQTT_KEEPALIVE_SEC 120

/* --------- Modem UART --------- */

static void modem_send_line(const char *cmd)
{
  uart_write_bytes(MODEM_UART_NUM, cmd, strlen(cmd));
  uart_write_bytes(MODEM_UART_NUM, "\r\n", 2);
}

static void uart_flush_rx(void)
{
  uint8_t tmp[256];
  while (uart_read_bytes(MODEM_UART_NUM, tmp, sizeof(tmp), pdMS_TO_TICKS(50)) > 0) {
  }
}

/* Line splitter for interleaved URCs (MQTT push, etc.) */
static char s_line_acc[1536];
static size_t s_line_pos;

static void handle_mqtt_json_command(const char *json);

static void finish_line(void)
{
  if (s_line_pos == 0) {
    return;
  }
  s_line_acc[s_line_pos] = '\0';

  /* Trim trailing CR */
  while (s_line_pos > 0 &&
         (s_line_acc[s_line_pos - 1] == '\r' || s_line_acc[s_line_pos - 1] == '\n')) {
    s_line_acc[--s_line_pos] = '\0';
  }

  if (s_line_pos > 0) {
    ESP_LOGD(TAG, "LINE: %s", s_line_acc);

    /* After +CMQTTRXPAYLOAD: len — next line is raw JSON body (short payloads). */
    static bool s_expect_payload_body;
    if (s_expect_payload_body) {
      s_expect_payload_body = false;
      if (s_line_acc[0] == '{') {
        handle_mqtt_json_command(s_line_acc);
      }
      goto reset;
    }

    if (strncmp(s_line_acc, "+CMQTTRXPAYLOAD:", 16) == 0) {
      s_expect_payload_body = true;
    }
  }

reset:
  s_line_pos = 0;
}

static void feed_modem_bytes(const uint8_t *data, size_t len)
{
  for (size_t i = 0; i < len; i++) {
    char c = (char)data[i];
    if (c == '\r') {
      continue;
    }
    if (c == '\n') {
      finish_line();
      continue;
    }
    if (s_line_pos + 1 < sizeof(s_line_acc)) {
      s_line_acc[s_line_pos++] = c;
    }
  }
}

static char s_drain_buf[3072];

static bool modem_drain_until(const char *needle, int timeout_ms, bool *error_token)
{
  size_t total = 0;
  s_drain_buf[0] = '\0';
  *error_token = false;
  int64_t deadline_ms = esp_timer_get_time() / 1000 + timeout_ms;

  while (esp_timer_get_time() / 1000 < deadline_ms) {
    uint8_t chunk[256];
    int n = uart_read_bytes(MODEM_UART_NUM, chunk, sizeof(chunk), pdMS_TO_TICKS(80));
    if (n <= 0) {
      continue;
    }
    feed_modem_bytes(chunk, (size_t)n);

    if (total + (size_t)n >= sizeof(s_drain_buf)) {
      n = (int)(sizeof(s_drain_buf) - 1 - total);
    }
    if (n <= 0) {
      continue;
    }
    memcpy(s_drain_buf + total, chunk, (size_t)n);
    total += (size_t)n;
    s_drain_buf[total] = '\0';

    if (strstr(s_drain_buf, needle)) {
      return true;
    }
    if (strstr(s_drain_buf, "\r\nERROR\r\n") || strstr(s_drain_buf, "\nERROR\r\n")) {
      *error_token = true;
      ESP_LOGW(TAG, "Modem ERROR in session");
      return false;
    }
  }

  s_drain_buf[total] = '\0';
  return strstr(s_drain_buf, needle) != NULL;
}

static bool modem_wait_prompt(const char *needle, int timeout_ms)
{
  size_t total = 0;
  s_drain_buf[0] = '\0';
  int64_t deadline_ms = esp_timer_get_time() / 1000 + timeout_ms;

  while (esp_timer_get_time() / 1000 < deadline_ms) {
    uint8_t chunk[128];
    int n = uart_read_bytes(MODEM_UART_NUM, chunk, sizeof(chunk), pdMS_TO_TICKS(80));
    if (n <= 0) {
      continue;
    }
    feed_modem_bytes(chunk, (size_t)n);

    if (total + (size_t)n >= sizeof(s_drain_buf)) {
      n = (int)(sizeof(s_drain_buf) - 1 - total);
    }
    if (n <= 0) {
      continue;
    }
    memcpy(s_drain_buf + total, chunk, (size_t)n);
    total += (size_t)n;
    s_drain_buf[total] = '\0';

    if (strstr(s_drain_buf, needle)) {
      return true;
    }
    if (strstr(s_drain_buf, "\r\nERROR\r\n")) {
      return false;
    }
  }
  return strstr(s_drain_buf, needle) != NULL;
}

static bool modem_interactive_body(const char *at_header, const char *body)
{
  uart_flush_rx();
  modem_send_line(at_header);

  if (!modem_wait_prompt(">", 8000)) {
    ESP_LOGE(TAG, "No > prompt after: %s", at_header);
    return false;
  }

  uart_write_bytes(MODEM_UART_NUM, body, strlen(body));
  uart_write_bytes(MODEM_UART_NUM, "\r\n", 2);

  bool err = false;
  if (!modem_drain_until("OK", 15000, &err)) {
    ESP_LOGE(TAG, "No OK after interactive body");
    return false;
  }
  return !err;
}

/* --------- LED / JSON command --------- */

static void led_apply(bool on)
{
  gpio_set_level(LED_GPIO_NUM, on ? 1 : 0);
  ESP_LOGI(TAG, "LED %s", on ? "ON" : "OFF");
}

static void handle_mqtt_json_command(const char *json)
{
  /* Accept {"cmd":"led","on":true} from POST /api/esp/led */
  const bool wants_led =
      strstr(json, "\"cmd\"") && strstr(json, "\"led\"");
  if (!wants_led) {
    ESP_LOGW(TAG, "Unknown MQTT JSON (ignored): %s", json);
    return;
  }

  if (strstr(json, "\"on\":true") || strstr(json, "\"on\": true")) {
    led_apply(true);
    return;
  }
  if (strstr(json, "\"on\":false") || strstr(json, "\"on\": false")) {
    led_apply(false);
    return;
  }

  /* Optional: "state":"on" / "off" */
  if (strstr(json, "\"state\"") && strstr(json, "\"on\"")) {
    led_apply(true);
    return;
  }
  if (strstr(json, "\"state\"") && strstr(json, "\"off\"")) {
    led_apply(false);
    return;
  }
}

static void led_gpio_init(void)
{
  gpio_reset_pin(LED_GPIO_NUM);
  gpio_set_direction(LED_GPIO_NUM, GPIO_MODE_OUTPUT);
  gpio_set_level(LED_GPIO_NUM, 0);
}

/* --------- Modem bring-up (from your sketch) --------- */

static void modem_power_on(void)
{
  gpio_config_t io_conf = {
      .pin_bit_mask = (1ULL << MODEM_PWRKEY),
      .mode = GPIO_MODE_OUTPUT,
  };
  gpio_config(&io_conf);

  gpio_set_level(MODEM_PWRKEY, 0);
  vTaskDelay(pdMS_TO_TICKS(100));
  gpio_set_level(MODEM_PWRKEY, 1);
  vTaskDelay(pdMS_TO_TICKS(1000));
  gpio_set_level(MODEM_PWRKEY, 0);

  ESP_LOGI(TAG, "Modem PWRKEY pulse done");
}

static void modem_uart_init(void)
{
  uart_config_t uart_config = {
      .baud_rate = MODEM_BAUDRATE,
      .data_bits = UART_DATA_8_BITS,
      .parity = UART_PARITY_DISABLE,
      .stop_bits = UART_STOP_BITS_1,
      .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
      .source_clk = UART_SCLK_DEFAULT,
  };

  uart_driver_install(MODEM_UART_NUM, 4096, 0, 0, NULL, 0);
  uart_param_config(MODEM_UART_NUM, &uart_config);
  uart_set_pin(MODEM_UART_NUM, MODEM_TX_PIN, MODEM_RX_PIN, UART_PIN_NO_CHANGE,
               UART_PIN_NO_CHANGE);
}

static void send_at_command(const char *cmd)
{
  modem_send_line(cmd);
}

static void read_response_log(void)
{
  uint8_t data[320];
  int len = uart_read_bytes(MODEM_UART_NUM, data, sizeof(data) - 1,
                             pdMS_TO_TICKS(800));
  if (len > 0) {
    data[len] = '\0';
    ESP_LOGI(TAG, "AT rsp: %s", (char *)data);
    feed_modem_bytes(data, (size_t)len);
  }
}

/* --------- GNSS + telemetry --------- */

static float s_lat;
static float s_lng;
static bool s_have_fix;

static int s_last_csq = 99;

static void parse_gps_response(const char *response)
{
  const char *start = strstr(response, "+CGNSSINFO:");
  if (start == NULL) {
    return;
  }

  start += strlen("+CGNSSINFO:");
  while (*start == ' ') {
    start++;
  }

  int status, num_sats;
  float latitude, longitude;
  char lat_dir, lon_dir;

  int result =
      sscanf(start, "%d,%d,%*[^,],%*[^,],%*[^,],%f,%c,%f,%c", &status, &num_sats,
             &latitude, &lat_dir, &longitude, &lon_dir);

  if (result == 6 && status > 0) {
    float lat = latitude;
    float lng = longitude;
    if (lat_dir == 'S' || lat_dir == 's') {
      lat = -fabsf(lat);
    }
    if (lon_dir == 'W' || lon_dir == 'w') {
      lng = -fabsf(lng);
    }
    s_lat = lat;
    s_lng = lng;
    s_have_fix = true;
    ESP_LOGI(TAG, "GNSS fix: %.7f, %.7f | sats=%d", lat, lng, num_sats);
  }
}

static void parse_csq_response(const char *response)
{
  const char *p = strstr(response, "+CSQ:");
  if (!p) {
    return;
  }
  int rssi = 99, ber = 99;
  if (sscanf(p, "+CSQ: %d,%d", &rssi, &ber) >= 1) {
    s_last_csq = rssi;
  }
}

/** Rough GSM CSQ (0–31) → dBm estimate; 99 = unknown */
static int csq_to_dbm_approx(int csq)
{
  if (csq == 99 || csq < 0) {
    return -100;
  }
  if (csq > 31) {
    csq = 31;
  }
  return -113 + 2 * csq;
}

static bool mqtt_publish_json_to_topic(const char *topic, const char *json_payload)
{
  char hdr[80];

  snprintf(hdr, sizeof(hdr), "AT+CMQTTTOPIC=%d,%d", MQTT_CLIENT_INDEX,
           (int)strlen(topic));
  if (!modem_interactive_body(hdr, topic)) {
    return false;
  }

  snprintf(hdr, sizeof(hdr), "AT+CMQTTPAYLOAD=%d,%d", MQTT_CLIENT_INDEX,
           (int)strlen(json_payload));
  if (!modem_interactive_body(hdr, json_payload)) {
    return false;
  }

  char pub[48];
  snprintf(pub, sizeof(pub), "AT+CMQTTPUB=%d,%d,%d", MQTT_CLIENT_INDEX,
           MQTT_QOS_PUBLISH, MQTT_PUBLISH_TIMEOUT_SEC);
  modem_send_line(pub);

  bool err = false;
  if (!modem_drain_until("+CMQTTPUB:", 20000, &err)) {
    ESP_LOGE(TAG, "CMQTTPUB timeout");
    return false;
  }
  if (strstr(s_drain_buf, "+CMQTTPUB: 0,0") == NULL) {
    ESP_LOGW(TAG, "CMQTTPUB unexpected: %s", s_drain_buf);
  }
  return !err;
}

static void telemetry_publish_if_ready(void)
{
  if (!s_have_fix) {
    ESP_LOGD(TAG, "Skip telemetry (no GNSS fix yet)");
    return;
  }

  int64_t uptime_sec = esp_timer_get_time() / 1000000;

  char body[256];
  snprintf(body, sizeof(body),
           "{\"lat\":%.7f,\"lng\":%.7f,\"uptimeSec\":%lld,\"rssi\":%d}", s_lat,
           s_lng, (long long)uptime_sec, csq_to_dbm_approx(s_last_csq));

  ESP_LOGI(TAG, "MQTT pub telemetry: %s", body);

  if (!mqtt_publish_json_to_topic(MQTT_TOPIC_TELEMETRY, body)) {
    ESP_LOGE(TAG, "Telemetry publish failed");
  }
}

/* --------- MQTT connect / subscribe (SIM7600-style CMQTT) --------- */

static bool mqtt_stack_start_and_connect(void)
{
  bool err = false;

  if (CELLULAR_APN[0] != '\0') {
    char apn_cmd[96];
    snprintf(apn_cmd, sizeof(apn_cmd), "AT+CGDCONT=1,\"IP\",\"%s\"",
             CELLULAR_APN);
    uart_flush_rx();
    modem_send_line(apn_cmd);
    modem_drain_until("OK", 5000, &err);
  }

  uart_flush_rx();
  modem_send_line("AT+CMQTTSTART");
  if (!modem_drain_until("+CMQTTSTART:", 45000, &err)) {
    ESP_LOGE(TAG, "CMQTTSTART failed / timeout");
    return false;
  }
  if (strstr(s_drain_buf, "+CMQTTSTART: 0") == NULL) {
    ESP_LOGW(TAG, "CMQTTSTART buf: %s", s_drain_buf);
  }

  char accq[80];
  snprintf(accq, sizeof(accq), "AT+CMQTTACCQ=%d,\"%s\"", MQTT_CLIENT_INDEX,
           MQTT_CLIENT_ID);
  uart_flush_rx();
  modem_send_line(accq);
  if (!modem_drain_until("OK", 10000, &err)) {
    ESP_LOGE(TAG, "CMQTTACCQ failed");
    return false;
  }

  char conn[192];
  snprintf(conn, sizeof(conn),
           "AT+CMQTTCONNECT=%d,\"tcp://%s:%d\",%d,1", MQTT_CLIENT_INDEX,
           MQTT_BROKER_HOST, MQTT_BROKER_PORT, MQTT_KEEPALIVE_SEC);

  uart_flush_rx();
  modem_send_line(conn);
  if (!modem_drain_until("+CMQTTCONNECT:", 60000, &err)) {
    ESP_LOGE(TAG, "CMQTTCONNECT timeout");
    return false;
  }
  if (strstr(s_drain_buf, "+CMQTTCONNECT: 0,0") == NULL) {
    ESP_LOGE(TAG, "CMQTTCONNECT failed: %s", s_drain_buf);
    return false;
  }

  /* Subscribe to backend commands */
  char subhdr[64];
  snprintf(subhdr, sizeof(subhdr), "AT+CMQTTSUB=%d,%d,%d", MQTT_CLIENT_INDEX,
           (int)strlen(MQTT_TOPIC_COMMANDS), MQTT_QOS_SUBSCRIBE);

  if (!modem_interactive_body(subhdr, MQTT_TOPIC_COMMANDS)) {
    ESP_LOGE(TAG, "CMQTTSUB failed");
    return false;
  }

  bool sub_ok = false;
  modem_drain_until("+CMQTTSUB:", 10000, &err);
  if (strstr(s_drain_buf, "+CMQTTSUB: 0,0")) {
    sub_ok = true;
  }
  if (!sub_ok) {
    ESP_LOGW(TAG, "CMQTTSUB response: %s", s_drain_buf);
  }

  ESP_LOGI(TAG, "MQTT connected + subscribed to %s", MQTT_TOPIC_COMMANDS);
  return true;
}

static void poll_uart_background(uint32_t window_ms)
{
  int64_t end = esp_timer_get_time() / 1000 + (int64_t)window_ms;
  while (esp_timer_get_time() / 1000 < end) {
    uint8_t b[256];
    int n = uart_read_bytes(MODEM_UART_NUM, b, sizeof(b), pdMS_TO_TICKS(40));
    if (n > 0) {
      feed_modem_bytes(b, (size_t)n);
    }
  }
}

void app_main(void)
{
  led_gpio_init();
  modem_uart_init();
  modem_power_on();

  vTaskDelay(pdMS_TO_TICKS(5000));

  uart_flush_rx();
  send_at_command("AT");
  read_response_log();

  send_at_command("AT+CSQ");
  read_response_log();

  send_at_command("AT+CREG?");
  read_response_log();

  send_at_command("AT+CGATT?");
  read_response_log();

  send_at_command("AT+CGACT=1,1");
  read_response_log();

  send_at_command("AT+CGPADDR=1");
  read_response_log();

  if (!mqtt_stack_start_and_connect()) {
    ESP_LOGE(TAG, "MQTT setup aborted — fix APN / SIM / broker reachability");
  }

  send_at_command("AT+CGNSSPWR=1");
  read_response_log();
  vTaskDelay(pdMS_TO_TICKS(3000));

  while (1) {
    poll_uart_background(80);

    send_at_command("AT+CSQ");
    read_response_log();

    send_at_command("AT+CGNSSINFO");
    uint8_t data[384];
    int len = uart_read_bytes(MODEM_UART_NUM, data, sizeof(data) - 1,
                              pdMS_TO_TICKS(1200));
    if (len > 0) {
      data[len] = '\0';
      ESP_LOGI(TAG, "GNSS rsp: %s", (char *)data);
      parse_gps_response((char *)data);
      parse_csq_response((char *)data);
      feed_modem_bytes(data, (size_t)len);
    }

    telemetry_publish_if_ready();

    poll_uart_background(200);
    vTaskDelay(pdMS_TO_TICKS(GNSS_POLL_INTERVAL_MS));
  }
}
