#pragma once

#include <Arduino.h>

constexpr uint8_t RAGNAR_LINK_VERSION = 1;
constexpr uint8_t RAGNAR_LINK_FRAME_STATUS = 0x01;
constexpr uint8_t RAGNAR_LINK_FRAME_BUTTON = 0x10;
constexpr uint8_t RAGNAR_LINK_FRAME_HELLO = 0x20;

constexpr uint8_t RAGNAR_STATE_UNKNOWN = 0;
constexpr uint8_t RAGNAR_STATE_IDLE = 1;
constexpr uint8_t RAGNAR_STATE_ACTIVE = 2;
constexpr uint8_t RAGNAR_STATE_ERROR = 3;

constexpr uint8_t GPS_STATE_UNKNOWN = 0;
constexpr uint8_t GPS_STATE_NO_FIX = 1;
constexpr uint8_t GPS_STATE_FIX = 2;

constexpr uint8_t CAPTURE_STATE_IDLE = 0;
constexpr uint8_t CAPTURE_STATE_RUNNING = 1;
constexpr uint8_t CAPTURE_STATE_PAUSED = 2;
constexpr uint8_t CAPTURE_STATE_ERROR = 3;

struct __attribute__((packed)) RagnarStatusPacket {
  uint8_t magic[2];       // 'R', 'L'
  uint8_t version;        // 1
  uint8_t type;           // 0x01
  uint32_t sequence;
  uint32_t uptime_s;
  uint16_t camera_count;
  uint16_t wifi_count;
  uint16_t ble_count;
  uint8_t ragnar_state;
  uint8_t gps_state;
  uint8_t capture_state;
  int8_t gateway_rssi;
  uint16_t crc16;
};

static_assert(sizeof(RagnarStatusPacket) == 24, "RagnarStatusPacket wire size changed");

uint16_t ragnar_crc16_ccitt(const uint8_t *data, size_t len);

