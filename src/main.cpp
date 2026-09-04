#include <Arduino.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

#include "ragnar_link_protocol.h"

namespace {
constexpr uint32_t SERIAL_BAUD = 115200;
constexpr uint8_t DEFAULT_CHANNEL = 6;
constexpr uint32_t HOST_TIMEOUT_MS = 15000;
constexpr uint32_t DISPLAY_REFRESH_MS = 500;

TFT_eSPI tft;
uint8_t espnow_channel = DEFAULT_CHANNEL;
uint32_t tx_sequence = 0;
uint32_t last_host_ms = 0;
uint32_t last_display_ms = 0;
uint32_t sent_packets = 0;
uint32_t send_failures = 0;
String last_message = "booting";
RagnarStatusPacket last_status = {};

const uint8_t broadcast_peer[ESP_NOW_ETH_ALEN] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

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

  bool host_ok = millis() - last_host_ms < HOST_TIMEOUT_MS;
  tft.setTextColor(host_ok ? TFT_GREEN : TFT_RED, TFT_BLACK);
  tft.setCursor(8, 100);
  tft.print(host_ok ? "Host: OK" : "Host: waiting");

  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setCursor(8, 126);
  tft.printf("Cams %u", last_status.camera_count);
  tft.setCursor(8, 142);
  tft.printf("WiFi %u", last_status.wifi_count);
  tft.setCursor(8, 158);
  tft.printf("BLE  %u", last_status.ble_count);

  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.setCursor(8, 190);
  tft.print(last_message.substring(0, 24));
}

void configure_espnow(uint8_t channel) {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);

  if (esp_now_init() != ESP_OK) {
    last_message = "esp-now init failed";
    return;
  }

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, broadcast_peer, ESP_NOW_ETH_ALEN);
  peer.channel = channel;
  peer.encrypt = false;
  esp_now_add_peer(&peer);
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
    last_status = packet;
    last_message = doc["message"] | "status sent";
    emit_json("ack", doc["seq"] | 0, "sent");
  } else {
    send_failures++;
    last_message = "esp-now send failed";
    emit_json("ack", doc["seq"] | 0, "send_failed");
  }
}

void handle_serial_line(String line) {
  line.trim();
  if (line.length() == 0) return;

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
    } else {
      emit_json("ack", doc["seq"] | 0, "unknown_command");
    }
  } else if (!strcmp(type, "status")) {
    send_status_packet(doc);
  } else {
    emit_json("ack", doc["seq"] | 0, "unknown_type");
  }
}
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

  Serial.println("ragnar-link-gateway: init display");
  tft.init();
  tft.setRotation(0);
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, TFT_BACKLIGHT_ON);

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
  static String line;
  while (Serial.available()) {
    char c = static_cast<char>(Serial.read());
    if (c == '\n') {
      handle_serial_line(line);
      line = "";
    } else if (c != '\r') {
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
