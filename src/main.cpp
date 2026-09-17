#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_idf_version.h>
#include <esp_wifi.h>

#include "ragnar_link_protocol.h"

#ifndef RAGNAR_GATEWAY_DISPLAY
#define RAGNAR_GATEWAY_DISPLAY 1
#endif

#if RAGNAR_GATEWAY_DISPLAY
#include <TFT_eSPI.h>
#endif

namespace {
constexpr uint32_t SERIAL_BAUD = 115200;
constexpr uint8_t DEFAULT_CHANNEL = 6;
constexpr uint32_t HOST_TIMEOUT_MS = 15000;
constexpr uint32_t DISPLAY_REFRESH_MS = 500;

#if RAGNAR_GATEWAY_DISPLAY
TFT_eSPI tft;
#endif
uint8_t espnow_channel = DEFAULT_CHANNEL;
uint32_t tx_sequence = 0;
uint32_t last_host_ms = 0;
uint32_t last_display_ms = 0;
uint32_t last_tx_ms = 0;
volatile uint32_t last_rx_ms = 0;
uint32_t sent_packets = 0;
uint32_t send_failures = 0;
volatile uint32_t tx_callbacks = 0;
volatile uint32_t tx_callback_failures = 0;
volatile uint32_t rx_packets = 0;
volatile int last_rx_len = 0;
volatile bool rx_event_pending = false;
volatile bool tx_failure_pending = false;
String last_message = "booting";
uint8_t last_rx_mac[ESP_NOW_ETH_ALEN] = {0, 0, 0, 0, 0, 0};
RagnarStatusPacket last_status = {};

const uint8_t broadcast_peer[ESP_NOW_ETH_ALEN] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

void handle_plain_serial_line(const String &line);
#if ESP_IDF_VERSION_MAJOR >= 5
void on_data_sent(const wifi_tx_info_t *, esp_now_send_status_t status);
void on_data_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len);
#else
void on_data_sent(const uint8_t *, esp_now_send_status_t status);
void on_data_recv(const uint8_t *mac, const uint8_t *data, int len);
#endif

String mac_to_string(const uint8_t *mac) {
  char text[18];
  snprintf(text, sizeof(text), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(text);
}

uint8_t map_ragnar_state(const char *value) {
  if (!value) return RAGNAR_STATE_UNKNOWN;
  if (!strcmp(value, "idle")) return RAGNAR_STATE_IDLE;
  if (!strcmp(value, "active")) return RAGNAR_STATE_ACTIVE;
  if (!strcmp(value, "error")) return RAGNAR_STATE_ERROR;
  return RAGNAR_STATE_UNKNOWN;
}

uint8_t map_gps_state(const char *value) {
  if (!value) return GPS_STATE_UNKNOWN;
  if (!strcmp(value, "fix")) return GPS_STATE_FIX;
  if (!strcmp(value, "no_fix")) return GPS_STATE_NO_FIX;
  if (!strcmp(value, "nofix")) return GPS_STATE_NO_FIX;
  return GPS_STATE_UNKNOWN;
}

uint8_t map_capture_state(const char *value) {
  if (!value) return CAPTURE_STATE_IDLE;
  if (!strcmp(value, "running")) return CAPTURE_STATE_RUNNING;
  if (!strcmp(value, "paused")) return CAPTURE_STATE_PAUSED;
  if (!strcmp(value, "error")) return CAPTURE_STATE_ERROR;
  return CAPTURE_STATE_IDLE;
}

void emit_json(const char *type, uint32_t seq, const char *status, const char *message = nullptr) {
  StaticJsonDocument<192> doc;
  doc["v"] = RAGNAR_LINK_VERSION;
  doc["type"] = type;
  doc["seq"] = seq;
  if (status) doc["status"] = status;
  if (message) doc["message"] = message;
  serializeJson(doc, Serial);
  Serial.println();
}

void draw_status() {
#if RAGNAR_GATEWAY_DISPLAY
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(8, 8);
  tft.print("Ragnar Link");

  tft.setTextSize(1);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(8, 42);
  tft.printf("Ch: %u", espnow_channel);
  tft.setCursor(8, 58);
  tft.printf("Sent: %lu", static_cast<unsigned long>(sent_packets));
  tft.setCursor(8, 74);
  tft.printf("Fail: %lu", static_cast<unsigned long>(send_failures));
  tft.setCursor(8, 90);
  tft.printf("TXcb: %lu/%lu", static_cast<unsigned long>(tx_callbacks),
             static_cast<unsigned long>(tx_callback_failures));

  bool host_ok = millis() - last_host_ms < HOST_TIMEOUT_MS;
  tft.setTextColor(host_ok ? TFT_GREEN : TFT_RED, TFT_BLACK);
  tft.setCursor(8, 116);
  tft.print(host_ok ? "Host: OK" : "Host: waiting");

  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setCursor(8, 142);
  tft.printf("Cams %u", last_status.camera_count);
  tft.setCursor(8, 158);
  tft.printf("WiFi %u", last_status.wifi_count);
  tft.setCursor(8, 174);
  tft.printf("BLE  %u", last_status.ble_count);

  tft.setTextColor(TFT_MAGENTA, TFT_BLACK);
  tft.setCursor(8, 206);
  tft.print("RX " + String(rx_packets) + " " + mac_to_string(last_rx_mac));

  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.setCursor(8, 238);
  tft.print(last_message.substring(0, 24));
#endif
}

void configure_espnow(uint8_t channel) {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);

  esp_err_t init_result = esp_now_init();
  if (init_result != ESP_OK) {
    last_message = "esp-now init failed " + String(init_result);
    return;
  }

  esp_now_register_send_cb(on_data_sent);
  esp_now_register_recv_cb(on_data_recv);

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, broadcast_peer, ESP_NOW_ETH_ALEN);
  peer.channel = channel;
  peer.encrypt = false;
  esp_err_t peer_result = esp_now_add_peer(&peer);
  if (peer_result != ESP_OK && peer_result != ESP_ERR_ESPNOW_EXIST) {
    last_message = "peer add failed " + String(peer_result);
    return;
  }
  last_message = "radio ready ch " + String(channel);
}

void send_status_packet(JsonDocument &doc) {
  RagnarStatusPacket packet = {};
  packet.magic[0] = 'R';
  packet.magic[1] = 'L';
  packet.version = RAGNAR_LINK_VERSION;
  packet.type = RAGNAR_LINK_FRAME_STATUS;
  packet.sequence = ++tx_sequence;
  packet.uptime_s = doc["uptime_s"] | 0;
  packet.camera_count = doc["camera_count"] | 0;
  packet.wifi_count = doc["wifi_count"] | 0;
  packet.ble_count = doc["ble_count"] | 0;
  packet.ragnar_state = map_ragnar_state(doc["ragnar"] | "unknown");
  packet.gps_state = map_gps_state(doc["gps"] | "unknown");
  packet.capture_state = map_capture_state(doc["capture"] | "idle");
  packet.gateway_rssi = 0;
  packet.crc16 = ragnar_crc16_ccitt(reinterpret_cast<const uint8_t *>(&packet), sizeof(packet) - 2);

  esp_err_t result = esp_now_send(broadcast_peer, reinterpret_cast<uint8_t *>(&packet), sizeof(packet));
  if (result == ESP_OK) {
    sent_packets++;
    last_tx_ms = millis();
    last_status = packet;
    last_message = doc["message"] | "status sent";
    emit_json("ack", doc["seq"] | 0, "sent");
  } else {
    send_failures++;
    last_message = "esp-now send failed";
    emit_json("ack", doc["seq"] | 0, "send_failed");
  }
}

void print_stats() {
  StaticJsonDocument<384> doc;
  doc["v"] = RAGNAR_LINK_VERSION;
  doc["type"] = "event";
  doc["event"] = "stats";
  doc["mac"] = WiFi.macAddress();
  doc["channel"] = espnow_channel;
  doc["sent"] = sent_packets;
  doc["send_failures"] = send_failures;
  doc["tx_callbacks"] = tx_callbacks;
  doc["tx_callback_failures"] = tx_callback_failures;
  doc["rx_packets"] = rx_packets;
  doc["last_rx_peer"] = mac_to_string(last_rx_mac);
  doc["last_rx_len"] = last_rx_len;
  doc["last_tx_ms"] = last_tx_ms;
  doc["last_rx_ms"] = last_rx_ms;
  doc["message"] = last_message;
  serializeJson(doc, Serial);
  Serial.println();
}

void handle_serial_line(String line) {
  line.trim();
  if (line.length() == 0) return;
  if (!line.startsWith("{")) {
    handle_plain_serial_line(line);
    return;
  }

  StaticJsonDocument<512> doc;
  DeserializationError error = deserializeJson(doc, line);
  if (error) {
    emit_json("ack", 0, "bad_json", error.c_str());
    return;
  }

  if ((doc["v"] | 0) != RAGNAR_LINK_VERSION) {
    emit_json("ack", doc["seq"] | 0, "bad_version");
    return;
  }

  last_host_ms = millis();
  const char *type = doc["type"] | "";

  if (!strcmp(type, "hello")) {
    emit_json("hello", doc["seq"] | 0, "ready", "ragnar-espnow-gateway");
  } else if (!strcmp(type, "command")) {
    const char *command = doc["command"] | "";
    if (!strcmp(command, "set_channel")) {
      espnow_channel = doc["channel"] | DEFAULT_CHANNEL;
      esp_now_deinit();
      configure_espnow(espnow_channel);
      emit_json("ack", doc["seq"] | 0, "channel_set");
    } else if (!strcmp(command, "stats")) {
      print_stats();
      emit_json("ack", doc["seq"] | 0, "stats");
    } else {
      emit_json("ack", doc["seq"] | 0, "unknown_command");
    }
  } else if (!strcmp(type, "status")) {
    send_status_packet(doc);
  } else {
    emit_json("ack", doc["seq"] | 0, "unknown_type");
  }
}

void handle_plain_serial_line(const String &line) {
  if (line == "STATS" || line == "stats") {
    print_stats();
  } else if (line == "HELP" || line == "help") {
    Serial.println("Ragnar Link Gateway commands:");
    Serial.println("  STATS");
  }
}

void process_radio_events() {
  if (rx_event_pending) {
    rx_event_pending = false;
    String peer = mac_to_string(last_rx_mac);
    last_message = "rx " + String(last_rx_len) + "B";
    Serial.print("ragnar-link-gateway: rx ");
    Serial.print(last_rx_len);
    Serial.print(" bytes from ");
    Serial.println(peer);
  }

  if (tx_failure_pending) {
    tx_failure_pending = false;
    last_message = "tx callback failed";
    Serial.println("ragnar-link-gateway: tx callback failed");
  }
}

void handle_espnow_data(const uint8_t *mac, const uint8_t *data, int len) {
  rx_packets++;
  last_rx_ms = millis();
  last_rx_len = len;
  if (mac != nullptr) {
    memcpy(last_rx_mac, mac, ESP_NOW_ETH_ALEN);
  }
  rx_event_pending = true;
  (void)data;
}

void handle_data_sent(esp_now_send_status_t status) {
  tx_callbacks++;
  if (status != ESP_NOW_SEND_SUCCESS) {
    tx_callback_failures++;
    tx_failure_pending = true;
  }
}

#if ESP_IDF_VERSION_MAJOR >= 5
void on_data_sent(const wifi_tx_info_t *, esp_now_send_status_t status) {
  handle_data_sent(status);
}

void on_data_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  const uint8_t *mac = info != nullptr ? info->src_addr : nullptr;
  handle_espnow_data(mac, data, len);
}
#else
void on_data_sent(const uint8_t *, esp_now_send_status_t status) {
  handle_data_sent(status);
}

void on_data_recv(const uint8_t *mac, const uint8_t *data, int len) {
  handle_espnow_data(mac, data, len);
}
#endif
}  // namespace

uint16_t ragnar_crc16_ccitt(const uint8_t *data, size_t len) {
  uint16_t crc = 0xffff;
  for (size_t i = 0; i < len; i++) {
    crc ^= static_cast<uint16_t>(data[i]) << 8;
    for (uint8_t bit = 0; bit < 8; bit++) {
      crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021) : static_cast<uint16_t>(crc << 1);
    }
  }
  return crc;
}

void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(1000);
  Serial.println();
  Serial.println("ragnar-link-gateway: boot");

#if RAGNAR_GATEWAY_DISPLAY
  Serial.println("ragnar-link-gateway: init display");
  tft.init();
  tft.setRotation(0);
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Starting...", 8, 8, 2);
#else
  Serial.println("ragnar-link-gateway: display disabled");
#endif

  Serial.println("ragnar-link-gateway: init esp-now");
  configure_espnow(espnow_channel);
  last_status.magic[0] = 'R';
  last_status.magic[1] = 'L';
  last_status.version = RAGNAR_LINK_VERSION;
  last_status.type = RAGNAR_LINK_FRAME_STATUS;
  Serial.println("ragnar-link-gateway: draw display");
  draw_status();
  Serial.println("ragnar-link-gateway: ready");
  emit_json("hello", 0, "ready", "ragnar-espnow-gateway");
}

void loop() {
  process_radio_events();

  static String line;
  while (Serial.available()) {
    char c = static_cast<char>(Serial.read());
    if (c == '\n' || c == '\r') {
      handle_serial_line(line);
      line = "";
    } else {
      line += c;
      if (line.length() > 768) {
        line = "";
        emit_json("ack", 0, "line_too_long");
      }
    }
  }

  if (millis() - last_display_ms >= DISPLAY_REFRESH_MS) {
    draw_status();
    last_display_ms = millis();
  }
}
