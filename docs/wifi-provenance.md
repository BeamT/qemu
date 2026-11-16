# Provenance of the Espressif Wi-Fi emulation

This page records where each part of the Wi-Fi emulation came from, who
wrote it and under which licence. Every source below is compatible with
GPL-2.0-or-later, the licence of the new files.

## Imported files

None of the out-of-tree files was copied verbatim. The model was rewritten
around a chip-independent core, and the logic, register offsets and constants
taken from the sources below are credited in the file headers.

| File in this tree | Derived from | Authors | Licence of the source |
| --- | --- | --- | --- |
| `hw/misc/esp_wifi.c` | `hw/misc/esp32_wifi.c` in a159x36/qemu, espressif/qemu PR #80 and lcgamboa/qemu | Martin Johnson, redfast00, lcgamboa, Adam Matthews | GPL-2.0-or-later (QEMU default for files without a header) |
| `hw/misc/esp_wifi_air.c` | `hw/misc/esp32_wifi_ap.c`, `hw/misc/esp32_wlan_packet.c` in the same trees | Clemens Kolbitsch (2008), Martin Johnson, redfast00, lcgamboa | MIT (Kolbitsch header), the rest GPL-2.0-or-later |
| `hw/misc/esp_wifi_int.h` | `hw/misc/esp32_wlan.h` (frame constants, RX control layout) | Clemens Kolbitsch, Martin Johnson, lcgamboa | MIT / GPL-2.0-or-later |
| `hw/misc/esp32_wifi.c` | `esp32_wifi.c`, `esp32_ana.c`, `esp32_fe.c`, `esp32_phya.c` in a159x36/qemu and lcgamboa/qemu | Martin Johnson, redfast00, lcgamboa | GPL-2.0-or-later |
| `hw/misc/esp32c3_wifi.c` | `esp32c3_wifi.c`, `esp32c3_ana.c`, `esp32c3_pwrmng.c` in lcgamboa/qemu | lcgamboa, Adam Matthews | GPL-2.0-or-later |
| `include/hw/misc/esp_wifi.h`, `esp32_wifi.h`, `esp32c3_wifi.h` | `include/hw/misc/esp32_wifi.h`, `esp32c3_wifi.h` | Martin Johnson, redfast00, lcgamboa | GPL-2.0-or-later |

The WPA2-PSK handshake, the RX descriptor protocol, the TX slot handling, the
ESP-NOW tunnel format, the CCMP framing of protected frames and the test
tooling under `tests/esp_wifi/` are new.

## Reference material (facts only, no code)

| Source | Used for | Licence |
| --- | --- | --- |
| [esp32-open-mac](https://github.com/esp32-open-mac/esp32-open-mac) `main/hardware.c` | TX slot layout (5 slots, stride −8), TXQ complete/error registers, RX descriptor reload protocol | MIT |
| [esp-wifi-hal](https://github.com/esp32-open-mac/esp-wifi-hal) `src/ll.rs` | MAC interrupt cause bits (TX done `0x80`, collision `0x100`, timeout `0x80000`), error register halves | MIT OR Apache-2.0 |
| [zeus.gent MAC write-up](https://zeus.gent/blog/23-24/open-source-esp32-wifi-mac/) | Address map of the Wi-Fi blocks | — |
| ESP32 Technical Reference Manual | `DPORT_WIFI_CLK_EN_REG` reset value | — |

The IDF Wi-Fi binary libraries were not disassembled. Register behaviour was
learnt by tracing the MMIO accesses of a running guest
(`-trace 'esp_wifi_*'`, `-d unimp`).

## Comparison of the out-of-tree implementations

State of each fork when this work started (October 2026).

| | PR #80 (redfast00) | a159x36 `esp-develop-9.2` | lcgamboa `picsimlab-esp32` |
| --- | --- | --- | --- |
| Base | esp-develop, April 2023 | esp-develop, August 2025 (QEMU 9.2) | esp-develop, December 2021 |
| Chips | ESP32 | ESP32, ESP32-S3 (`iss3` flag) | ESP32, ESP32-C3 |
| Mixed with unrelated changes | No | Yes: displays, ULP, RMT, servo, skins | Yes: PICSimLab board support |
| STA, open network | Yes | Yes | Yes |
| SoftAP | AP code, untested | Same as PR #80 | Yes: emulates a client and requests an address with its own DHCP client |
| ESP-NOW | No | No | Yes: action frames in Ethernet frames, type byte `0xA0 \| channel`; ACK timer; PHY TX status `0x270` (ESP32) / `0x320` (C3) |
| WPA2 | No | No | No |
| TX slots | 1 | 1 | 1 |
| Channel detection | Analog register write | Analog register write | ESP32: analog `0xc4`; C3: PLL frequency in analog `0x150` |
| Guest reset | No reset handler | Clears `ap_state` only | C3: clears registers and AP state; ESP32: `ap_state` only |
| RX descriptor handling | Wraps to the first descriptor | Same | Checks the owner bit before writing (October 2026) |
| Configurable APs | Hard-coded | Hard-coded | Hard-coded |
| `DPORT_WIFI_CLK_EN` for IDF ≥ 5.3 | Not handled | Read as constant `0x8f8f` | C3: stored in the machine |

Ebiroll/qemu_esp32, listed as a source in the original plan, is not a QEMU
tree. It holds ESP32 example projects and a `wifi.md` file of tracing notes,
and contributed nothing to this implementation.

## Consent of the original authors

Not yet recorded. The commit trailers (`Co-authored-by`) name the authors
above. Their agreement, and whether they also want a `Signed-off-by`, has to
be obtained before the series is posted upstream.
