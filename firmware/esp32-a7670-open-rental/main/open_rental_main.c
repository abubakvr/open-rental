/*
 * ESP32 + SIMCom A7670E — cellular MQTT (SIM7600-style AT+CMQTT*), GNSS telemetry,
 * and LED control via backend MQTT commands.
 *
 * Backend publishes: {"cmd":"led","on":true|false}, {"cmd":"ussd","code":"*310#","requestId":"..."},
 * {"cmd":"sms","to":"...","text":"...","requestId":"..."} on MQTT_TOPIC_COMMANDS.
 * USSD/SMS replies are published as JSON on MQTT_TOPIC_REPLIES.
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
#include "freertos/portmacro.h"

static const char *TAG = "OPEN_RENTAL";

/* --------- User configuration --------- */

#define MQTT_BROKER_HOST "51.91.97.149"
#define MQTT_BROKER_PORT 1883

#define MQTT_TOPIC_TELEMETRY "open-rental/esp/telemetry"
#define MQTT_TOPIC_COMMANDS "open-rental/esp/commands"
#define MQTT_TOPIC_REPLIES "open-rental/esp/replies"

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

/** Set true only after subscribe completes; telemetry publish checks this. */
static bool s_mqtt_pipeline_ok;
/** Count consecutive modem MQTT publish failures (dead CMQTT session after RF resets). */
static int s_mqtt_pub_fail_streak;
/** Rate-limit telemetry AT publishes so GNSS/radio URC storms don’t wedge CMQTT. */
static int64_t s_next_telemetry_attempt_ms;

#ifndef TELEMETRY_MIN_OK_INTERVAL_MS
#define TELEMETRY_MIN_OK_INTERVAL_MS 8000
#endif
#ifndef TELEMETRY_RETRY_AFTER_FAIL_MS
#define TELEMETRY_RETRY_AFTER_FAIL_MS 3500
#endif

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

  if (!modem_wait_prompt(">", 20000)) {
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

typedef enum {
  PENDING_NONE = 0,
  PENDING_USSD,
  PENDING_SMS,
} pending_kind_t;

typedef struct {
  pending_kind_t kind;
  char request_id[48];
  char ussd_code[48];
  char sms_to[32];
  char sms_text[280];
} pending_modem_t;

static pending_modem_t s_pending;
static portMUX_TYPE s_pending_mux = portMUX_INITIALIZER_UNLOCKED;

static bool json_extract_str(const char *json, const char *key, char *out,
                             size_t out_len)
{
  char pat[56];
  snprintf(pat, sizeof(pat), "\"%s\":\"", key);
  const char *p = strstr(json, pat);
  if (!p) {
    return false;
  }
  p += strlen(pat);
  size_t i = 0;
  while (*p && *p != '"' && i + 1 < out_len) {
    out[i++] = *p++;
  }
  out[i] = '\0';
  return i > 0;
}

static void sanitize_json_token_inplace(char *s)
{
  for (; *s; s++) {
    if (*s == '"' || *s == '\\' || *s < 0x20) {
      *s = '_';
    }
  }
}

static void queue_pending_ussd(const char *req_id, const char *code)
{
  taskENTER_CRITICAL(&s_pending_mux);
  if (s_pending.kind != PENDING_NONE) {
    taskEXIT_CRITICAL(&s_pending_mux);
    ESP_LOGW(TAG, "USSD ignored (modem job already queued)");
    return;
  }
  memset(&s_pending, 0, sizeof(s_pending));
  snprintf(s_pending.request_id, sizeof(s_pending.request_id), "%s", req_id);
  snprintf(s_pending.ussd_code, sizeof(s_pending.ussd_code), "%s", code);
  s_pending.kind = PENDING_USSD;
  taskEXIT_CRITICAL(&s_pending_mux);
}

static void queue_pending_sms(const char *req_id, const char *to,
                              const char *text)
{
  taskENTER_CRITICAL(&s_pending_mux);
  if (s_pending.kind != PENDING_NONE) {
    taskEXIT_CRITICAL(&s_pending_mux);
    ESP_LOGW(TAG, "SMS ignored (modem job already queued)");
    return;
  }
  memset(&s_pending, 0, sizeof(s_pending));
  snprintf(s_pending.request_id, sizeof(s_pending.request_id), "%s", req_id);
  snprintf(s_pending.sms_to, sizeof(s_pending.sms_to), "%s", to);
  snprintf(s_pending.sms_text, sizeof(s_pending.sms_text), "%s", text);
  s_pending.kind = PENDING_SMS;
  taskEXIT_CRITICAL(&s_pending_mux);
}

static void handle_mqtt_json_command(const char *json)
{
  const bool wants_led =
      strstr(json, "\"cmd\"") && strstr(json, "\"led\"");
  if (wants_led) {
    if (strstr(json, "\"on\":true") || strstr(json, "\"on\": true")) {
      led_apply(true);
      return;
    }
    if (strstr(json, "\"on\":false") || strstr(json, "\"on\": false")) {
      led_apply(false);
      return;
    }
    if (strstr(json, "\"state\"") && strstr(json, "\"on\"")) {
      led_apply(true);
      return;
    }
    if (strstr(json, "\"state\"") && strstr(json, "\"off\"")) {
      led_apply(false);
      return;
    }
    ESP_LOGW(TAG, "LED JSON missing on/state: %s", json);
    return;
  }

  char req_id[48] = "unknown";
  if (!json_extract_str(json, "requestId", req_id, sizeof(req_id))) {
    snprintf(req_id, sizeof(req_id), "unknown");
  }

  if (strstr(json, "\"cmd\"") && strstr(json, "\"ussd\"")) {
    char code[48];
    if (json_extract_str(json, "code", code, sizeof(code))) {
      queue_pending_ussd(req_id, code);
    }
    return;
  }

  if (strstr(json, "\"cmd\"") && strstr(json, "\"sms\"")) {
    char to[32];
    char text[280];
    if (json_extract_str(json, "to", to, sizeof(to)) &&
        json_extract_str(json, "text", text, sizeof(text))) {
      queue_pending_sms(req_id, to, text);
    }
    return;
  }

  ESP_LOGW(TAG, "Unknown MQTT JSON (ignored): %s", json);
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
  while (*start == ' ' || *start == '\t') {
    start++;
  }

  /* No fix yet — SIMCom often reports commas only */
  if (*start == ',' || *start == '\0' || *start == '\r' || *start == '\n') {
    return;
  }

  int status = 0;
  int num_sats = 0;
  float latitude = 0.0f;
  float longitude = 0.0f;
  char lat_dir = 0;
  char lon_dir = 0;

  /*
   * A7670E extended fix line looks like:
   *   3,14,,02,01,6.5275178,N,3.3793924,E,100526,210935.00,...
   * Older / shorter formats used three dummy fields before lat/lon.
   */
  int n = sscanf(start, "%d,%d,,%*[^,],%*[^,],%f,%c,%f,%c", &status, &num_sats,
                 &latitude, &lat_dir, &longitude, &lon_dir);

  if (n != 6) {
    n = sscanf(start, "%d,%d,%*[^,],%*[^,],%*[^,],%f,%c,%f,%c", &status,
               &num_sats, &latitude, &lat_dir, &longitude, &lon_dir);
  }

  if (n != 6 || status <= 0) {
    return;
  }

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
  ESP_LOGI(TAG, "GNSS fix: %.7f, %.7f | status=%d sats=%d", lat, lng, status,
           num_sats);
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

/** Read AT+CSQ response and update s_last_csq (plain read_response_log does not). */
static void read_response_log_csq(void)
{
  uint8_t data[320];
  int len = uart_read_bytes(MODEM_UART_NUM, data, sizeof(data) - 1,
                             pdMS_TO_TICKS(800));
  if (len > 0) {
    data[len] = '\0';
    ESP_LOGI(TAG, "AT rsp: %s", (char *)data);
    feed_modem_bytes(data, (size_t)len);
    parse_csq_response((char *)data);
  }
}

static void send_at_csq_query(void)
{
  uart_flush_rx();
  modem_send_line("AT+CSQ");
  read_response_log_csq();
}

/** Rough GSM CSQ (0–31) → dBm estimate; 99 or invalid → treat as unknown (-100 legacy). */
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

/** Bring modem back to command mode after GNSS errors / IP events */
static bool modem_sync_at_ok(void)
{
  bool err = false;
  uart_flush_rx();
  modem_send_line("AT");
  return modem_drain_until("OK", 4000, &err) && !err;
}

static bool mqtt_publish_json_to_topic(const char *topic, const char *json_payload)
{
  uart_flush_rx();

  for (int attempt = 0; attempt < 2; attempt++) {
    if (attempt > 0) {
      ESP_LOGW(TAG, "MQTT publish retry after modem sync");
      vTaskDelay(pdMS_TO_TICKS(400));
    }

    if (!modem_sync_at_ok()) {
      ESP_LOGW(TAG, "AT sync failed before MQTT publish");
    }

    char hdr[80];

    snprintf(hdr, sizeof(hdr), "AT+CMQTTTOPIC=%d,%d", MQTT_CLIENT_INDEX,
             (int)strlen(topic));
    if (!modem_interactive_body(hdr, topic)) {
      continue;
    }

    snprintf(hdr, sizeof(hdr), "AT+CMQTTPAYLOAD=%d,%d", MQTT_CLIENT_INDEX,
             (int)strlen(json_payload));
    if (!modem_interactive_body(hdr, json_payload)) {
      continue;
    }

    char pub[48];
    snprintf(pub, sizeof(pub), "AT+CMQTTPUB=%d,%d,%d", MQTT_CLIENT_INDEX,
             MQTT_QOS_PUBLISH, MQTT_PUBLISH_TIMEOUT_SEC);
    modem_send_line(pub);

    bool err = false;
    if (!modem_drain_until("+CMQTTPUB:", 25000, &err)) {
      ESP_LOGE(TAG, "CMQTTPUB timeout");
      continue;
    }
    if (strstr(s_drain_buf, "+CMQTTPUB: 0,0") == NULL) {
      ESP_LOGW(TAG, "CMQTTPUB unexpected: %s", s_drain_buf);
    }
    return !err;
  }

  return false;
}

static void json_escape_detail(const char *in, char *out, size_t out_sz)
{
  size_t j = 0;
  for (size_t i = 0; in[i] && j + 1 < out_sz; i++) {
    unsigned char c = (unsigned char)in[i];
    if (c == '"' || c == '\\') {
      if (j + 2 >= out_sz) {
        break;
      }
      out[j++] = '\\';
      out[j++] = (char)c;
    } else if (c == '\r' || c == '\n') {
      if (j + 1 >= out_sz) {
        break;
      }
      out[j++] = ' ';
    } else if (c < 0x20) {
      if (j + 1 >= out_sz) {
        break;
      }
      out[j++] = '?';
    } else {
      out[j++] = (char)c;
    }
  }
  out[j] = '\0';
}

static void publish_modem_reply(const char *request_id, const char *kind,
                                bool ok, const char *detail_raw)
{
  if (!s_mqtt_pipeline_ok) {
    ESP_LOGW(TAG, "Skip modem reply (MQTT pipeline down)");
    return;
  }

  char rid_safe[48];
  snprintf(rid_safe, sizeof(rid_safe), "%s",
           request_id != NULL ? request_id : "unknown");
  sanitize_json_token_inplace(rid_safe);

  char esc[640];
  json_escape_detail(detail_raw != NULL ? detail_raw : "", esc, sizeof(esc));

  char body[860];
  snprintf(body, sizeof(body),
           "{\"requestId\":\"%s\",\"kind\":\"%s\",\"ok\":%s,\"detail\":\"%s\"}",
           rid_safe, kind, ok ? "true" : "false", esc);

  if (!mqtt_publish_json_to_topic(MQTT_TOPIC_REPLIES, body)) {
    ESP_LOGW(TAG, "Modem reply MQTT publish failed");
  }
}

static bool modem_run_ussd(const char *code, char *detail, size_t detail_len)
{
  bool err = false;
  uart_flush_rx();

  char cmd[96];
  snprintf(cmd, sizeof(cmd), "AT+CUSD=1,\"%s\",15", code);
  modem_send_line(cmd);

  if (!modem_drain_until("+CUSD:", 55000, &err)) {
    snprintf(detail, detail_len, "timeout waiting +CUSD: err=%d tail=%s",
             err ? 1 : 0, s_drain_buf);
    return false;
  }

  const char *p = strstr(s_drain_buf, "+CUSD:");
  if (p != NULL) {
    snprintf(detail, detail_len, "%s", p);
  } else {
    snprintf(detail, detail_len, "%s", s_drain_buf);
  }

  if (err || strstr(s_drain_buf, "\r\nERROR\r\n") != NULL) {
    return false;
  }
  return true;
}

static bool modem_run_sms(const char *to, const char *text, char *detail,
                          size_t detail_len)
{
  bool err = false;

  uart_flush_rx();
  modem_send_line("AT+CMGF=1");
  if (!modem_drain_until("OK", 12000, &err) || err) {
    snprintf(detail, detail_len, "AT+CMGF=1 failed: %s", s_drain_buf);
    return false;
  }

  char hdr[48];
  snprintf(hdr, sizeof(hdr), "AT+CMGS=\"%s\"", to);
  uart_flush_rx();
  modem_send_line(hdr);
  if (!modem_wait_prompt(">", 25000)) {
    snprintf(detail, detail_len, "no > after CMGS: %s", s_drain_buf);
    return false;
  }

  uart_write_bytes(MODEM_UART_NUM, text, strlen(text));
  uint8_t z = 0x1a;
  uart_write_bytes(MODEM_UART_NUM, &z, 1);

  if (!modem_drain_until("+CMGS:", 95000, &err)) {
    snprintf(detail, detail_len, "SMS send timeout: %s", s_drain_buf);
    return false;
  }

  snprintf(detail, detail_len, "%s", s_drain_buf);
  if (strstr(s_drain_buf, "\r\nERROR\r\n") != NULL) {
    return false;
  }
  return !err;
}

static void process_pending_modem_command(void)
{
  pending_modem_t job;

  taskENTER_CRITICAL(&s_pending_mux);
  if (s_pending.kind == PENDING_NONE) {
    taskEXIT_CRITICAL(&s_pending_mux);
    return;
  }
  job = s_pending;
  memset(&s_pending, 0, sizeof(s_pending));
  s_pending.kind = PENDING_NONE;
  taskEXIT_CRITICAL(&s_pending_mux);

  char detail[640];
  bool ok = false;
  const char *kind = "unknown";

  if (job.kind == PENDING_USSD) {
    kind = "ussd";
    ok = modem_run_ussd(job.ussd_code, detail, sizeof(detail));
  } else if (job.kind == PENDING_SMS) {
    kind = "sms";
    ok = modem_run_sms(job.sms_to, job.sms_text, detail, sizeof(detail));
  } else {
    return;
  }

  publish_modem_reply(job.request_id, kind, ok, detail);
}

static void telemetry_publish_if_ready(void)
{
  if (!s_mqtt_pipeline_ok) {
    ESP_LOGD(TAG, "Skip telemetry (MQTT pipeline not up)");
    return;
  }

  if (!s_have_fix) {
    ESP_LOGD(TAG, "Skip telemetry (no GNSS fix yet)");
    return;
  }

  int64_t now_ms = esp_timer_get_time() / 1000;
  if (now_ms < s_next_telemetry_attempt_ms) {
    return;
  }

  int64_t uptime_sec = esp_timer_get_time() / 1000000;

  char body[320];
  if (s_last_csq >= 0 && s_last_csq <= 31) {
    int dbm = csq_to_dbm_approx(s_last_csq);
    snprintf(body, sizeof(body),
             "{\"lat\":%.7f,\"lng\":%.7f,\"uptimeSec\":%lld,\"csq\":%d,"
             "\"rssi\":%d}",
             s_lat, s_lng, (long long)uptime_sec, s_last_csq, dbm);
  } else {
    snprintf(body, sizeof(body),
             "{\"lat\":%.7f,\"lng\":%.7f,\"uptimeSec\":%lld,\"csq\":%d,"
             "\"rssi\":null}",
             s_lat, s_lng, (long long)uptime_sec, s_last_csq);
  }

  ESP_LOGI(TAG, "MQTT pub telemetry: %s", body);

  if (!mqtt_publish_json_to_topic(MQTT_TOPIC_TELEMETRY, body)) {
    ESP_LOGE(TAG, "Telemetry publish failed");
    s_mqtt_pub_fail_streak++;
    s_next_telemetry_attempt_ms =
        now_ms + TELEMETRY_RETRY_AFTER_FAIL_MS;
    if (s_mqtt_pub_fail_streak >= 2) {
      s_mqtt_pipeline_ok = false;
      ESP_LOGW(TAG,
               "MQTT modem publish stuck (%d fails) - scheduling pipeline reconnect",
               s_mqtt_pub_fail_streak);
    }
    return;
  }

  s_mqtt_pub_fail_streak = 0;
  s_next_telemetry_attempt_ms = now_ms + TELEMETRY_MIN_OK_INTERVAL_MS;
  ESP_LOGI(TAG, "Telemetry MQTT publish OK -> %s", MQTT_TOPIC_TELEMETRY);
}

/* --------- MQTT connect / subscribe (SIM7600-style CMQTT) --------- */

/* --------- PDP wait — EPS / LTE often activates after CGACT OK (async URC) --------- */

static bool pdp_context1_read_ip(char *ip_out, size_t ip_len)
{
  bool err = false;
  uart_flush_rx();
  modem_send_line("AT+CGPADDR=1");
  if (!modem_drain_until("OK", 8000, &err) || err) {
    return false;
  }

  const char *p = strstr(s_drain_buf, "+CGPADDR:");
  if (p == NULL) {
    return false;
  }

  int cid = -1;
  char ip[48];
  if (sscanf(p, "+CGPADDR: %d,%47[^,\r\n]", &cid, ip) < 2) {
    return false;
  }

  if (strcmp(ip, "0.0.0.0") == 0 || strlen(ip) < 7) {
    return false;
  }

  if (ip_out != NULL && ip_len > 0) {
    strncpy(ip_out, ip, ip_len - 1);
    ip_out[ip_len - 1] = '\0';
  }

  ESP_LOGI(TAG, "PDP context 1 IP: %s", ip);
  return true;
}

/** Poll until PDP has a real IPv4 or timeout (CMQTTSTART needs live packet data). */
static void wait_for_packet_data_ip(uint32_t max_wait_ms)
{
  const uint32_t step_ms = 750;
  uint32_t waited = 0;

  while (waited < max_wait_ms) {
    char ip[48];
    if (pdp_context1_read_ip(ip, sizeof(ip))) {
      ESP_LOGI(TAG, "Packet data up; settling 3s before MQTT...");
      vTaskDelay(pdMS_TO_TICKS(3000));
      return;
    }
    vTaskDelay(pdMS_TO_TICKS(step_ms));
    waited += step_ms;
  }

  ESP_LOGW(TAG,
           "No PDP IPv4 after %u ms - MQTT often returns +CMQTTSTART: 1 until "
           "registration completes",
           (unsigned int)max_wait_ms);
}

static void modem_cmqtt_teardown_best_effort(void)
{
  bool err = false;
  uart_flush_rx();
  modem_send_line("AT+CMQTTDISC=0,0");
  modem_drain_until("OK", 8000, &err);
  uart_flush_rx();
  modem_send_line("AT+CMQTTREL=0");
  modem_drain_until("OK", 8000, &err);
  uart_flush_rx();
  modem_send_line("AT+CMQTTSTOP");
  modem_drain_until("OK", 25000, &err);
  vTaskDelay(pdMS_TO_TICKS(1500));
}

/**
 * Run AT+CMQTTSTART until result is +CMQTTSTART: 0 (success).
 * Non-zero (e.g. : 1) is common if PDP is not ready yet — fixed by wait_for_packet_data_ip + retries.
 */
static bool modem_cmqtt_start_until_ok(void)
{
  for (int attempt = 0; attempt < 8; attempt++) {
    if (attempt > 0) {
      ESP_LOGW(TAG, "CMQTTSTART retry %d/7", attempt);
      modem_cmqtt_teardown_best_effort();
      wait_for_packet_data_ip(45000);
    }

    bool err = false;
    uart_flush_rx();
    modem_send_line("AT+CMQTTSTART");
    if (!modem_drain_until("+CMQTTSTART:", 90000, &err)) {
      ESP_LOGW(TAG, "CMQTTSTART timeout (attempt %d)", attempt);
      vTaskDelay(pdMS_TO_TICKS(4000));
      continue;
    }

    if (strstr(s_drain_buf, "+CMQTTSTART: 0") != NULL) {
      ESP_LOGI(TAG, "CMQTTSTART OK");
      return true;
    }

    ESP_LOGW(TAG, "CMQTTSTART failed (want : 0): %s", s_drain_buf);
    vTaskDelay(pdMS_TO_TICKS(4000));
  }

  return false;
}

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

  modem_cmqtt_teardown_best_effort();

  wait_for_packet_data_ip(120000);

  if (!modem_cmqtt_start_until_ok()) {
    ESP_LOGE(TAG, "CMQTTSTART never succeeded - check SIM/APN and PDP");
    return false;
  }

  char accq[80];
  snprintf(accq, sizeof(accq), "AT+CMQTTACCQ=%d,\"%s\"", MQTT_CLIENT_INDEX,
           MQTT_CLIENT_ID);
  uart_flush_rx();
  modem_send_line(accq);
  if (!modem_drain_until("OK", 10000, &err) || err) {
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

  modem_drain_until("+CMQTTSUB:", 10000, &err);
  if (strstr(s_drain_buf, "+CMQTTSUB: 0,0") == NULL) {
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

  send_at_csq_query();

  send_at_command("AT+CREG?");
  read_response_log();

  send_at_command("AT+CGATT?");
  read_response_log();

  send_at_command("AT+CGACT=1,1");
  read_response_log();

  send_at_command("AT+CGPADDR=1");
  read_response_log();

  s_mqtt_pipeline_ok = mqtt_stack_start_and_connect();
  if (!s_mqtt_pipeline_ok) {
    ESP_LOGE(TAG,
             "MQTT setup failed - dashboard will stay empty until connect works");
  }

  send_at_command("AT+CGNSSPWR=1");
  read_response_log();
  ESP_LOGI(TAG, "Waiting for GNSS stack (+CGNSSPWR READY / UART URC)...");
  for (int i = 0; i < 50; i++) {
    poll_uart_background(200);
  }

  int mqtt_retry_loops = 0;

  while (1) {
    poll_uart_background(80);

    process_pending_modem_command();

    /* Fast reconnect after modem RF/stack resets wedge CMQTT (> prompt missing). */
    uint32_t mqtt_reconnect_loops =
        (!s_mqtt_pipeline_ok && s_mqtt_pub_fail_streak >= 2) ? 4u : 18u;

    if (!s_mqtt_pipeline_ok) {
      if (++mqtt_retry_loops >= mqtt_reconnect_loops) {
        mqtt_retry_loops = 0;
        ESP_LOGW(TAG, "Reconnecting MQTT pipeline (fail_streak=%d)...",
                 s_mqtt_pub_fail_streak);
        if (mqtt_stack_start_and_connect()) {
          s_mqtt_pipeline_ok = true;
          s_mqtt_pub_fail_streak = 0;
          s_next_telemetry_attempt_ms = 0;
        } else {
          s_mqtt_pipeline_ok = false;
        }
      }
    } else {
      mqtt_retry_loops = 0;
    }

    send_at_csq_query();

    /* Publish telemetry before GNSS — CGNSSINFO ERROR bursts can break CMQTT AT. */
    telemetry_publish_if_ready();

    send_at_command("AT+CGNSSINFO");
    uint8_t data[512];
    int len = uart_read_bytes(MODEM_UART_NUM, data, sizeof(data) - 1,
                              pdMS_TO_TICKS(1500));
    if (len > 0) {
      data[len] = '\0';
      ESP_LOGI(TAG, "GNSS rsp: %s", (char *)data);
      parse_gps_response((char *)data);
      parse_csq_response((char *)data);
      feed_modem_bytes(data, (size_t)len);
    }

    poll_uart_background(200);
    vTaskDelay(pdMS_TO_TICKS(GNSS_POLL_INTERVAL_MS));
  }
}
