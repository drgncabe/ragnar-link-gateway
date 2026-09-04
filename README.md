# Ragnar Link Gateway

Firmware for the Ragnar Link ESP-NOW gateway.

Target board: Waveshare ESP32-S3-LCD-1.47, non-B variant.

The gateway connects to the Ragnar device over USB CDC serial, receives JSON status frames from the `ragnar-link` host service, broadcasts compact ESP-NOW packets to IRIS/M5Stack stopwatch devices, and shows local link status on the Waveshare LCD.

Host-side repo: https://github.com/drgncabe/ragnar-link

## Build

Install PlatformIO, then:

```sh
pio run
```

## Flash

Plug the Waveshare ESP32-S3-LCD-1.47 into USB, then:

```sh
pio run -t upload
```

Serial monitor:

```sh
pio device monitor -b 115200
```

## Hardware Target

This repo is configured for the regular `ESP32-S3-LCD-1.47` pinout:

| LCD signal | ESP32-S3 GPIO |
| --- | --- |
| MOSI | GPIO45 |
| SCLK | GPIO40 |
| CS | GPIO42 |
| DC | GPIO41 |
| RST | GPIO39 |
| BL | GPIO48 |

The `ESP32-S3-LCD-1.47B` board uses `GPIO46` for LCD backlight. If you later discover the hardware is the B variant, change `TFT_BL=48` to `TFT_BL=46` in `platformio.ini`.

## Protocol

The ESP-NOW packet contract is documented in [docs/espnow-protocol.md](docs/espnow-protocol.md). IRIS should treat that file and the matching host-side docs in `ragnar-link` as the v1 source of truth.

