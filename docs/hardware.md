# Hardware Notes

Target gateway board: Waveshare ESP32-S3-LCD-1.47, non-B variant.

This firmware uses:

| LCD signal | ESP32-S3 GPIO |
| --- | --- |
| MOSI | GPIO45 |
| SCLK | GPIO40 |
| CS | GPIO42 |
| DC | GPIO41 |
| RST | GPIO39 |
| BL | GPIO48 |

The board exposes USB for power, firmware upload, and USB CDC serial communication with Ragnar.

The B variant also has a Type-C connector, so Type-C by itself is not a reliable way to distinguish the boards. The practical firmware difference is the LCD backlight pin: `GPIO48` on the regular 1.47 board versus `GPIO46` on the 1.47B board.

