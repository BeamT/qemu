# Bluetooth LE for ESP32, ESP32-C3 and ESP32-S3

QEMU does not emulate the Bluetooth controller of these chips: its baseband
is undocumented and the controller firmware is a binary blob tied to it.
ESP-IDF can instead build the NimBLE host on its own ("host only") and talk
standard HCI to an external controller over a UART. QEMU already emulates
the UARTs, so an application built that way runs unchanged in QEMU, with its
HCI UART connected to a virtual controller on the host:

```
 QEMU                                Host
+----------------------------+      +-------------------------------------+
| app -> NimBLE host         |      | Rootcanal                           |
|          |                 |      |   controller A <--+                 |
|        UART1 (H4 HCI) -----+------+--> TCP :6402      | virtual radio   |
+----------------------------+      |   controller B <--+                 |
                                    |        ^                            |
                                    +--------+----------------------------+
                                             |
                                     Bumble, BlueZ (via a bridge), or the
                                     Android emulator (via netsim)
```

[Rootcanal](https://android.googlesource.com/platform/packages/modules/Bluetooth/+/refs/heads/main/tools/rootcanal/)
is the virtual controller the Android emulator uses. Each TCP connection to
its HCI port gets its own controller, and all of them share one virtual
radio: the emulated chip advertises, and a host on another connection scans,
connects, uses GATT and pairs with it.

## Building the application

Add to the application's `sdkconfig.defaults`, or better to a separate file
used only for QEMU builds (`SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.qemu"`):

```
CONFIG_BT_ENABLED=y
CONFIG_BT_CONTROLLER_DISABLED=y
CONFIG_BT_NIMBLE_ENABLED=y
CONFIG_BT_NIMBLE_TRANSPORT_UART=y
CONFIG_BT_NIMBLE_TRANSPORT_UART_PORT=1
# On by default for the ESP32; Rootcanal does not implement
# HCI Set Controller To Host Flow Control
CONFIG_BT_NIMBLE_HS_FLOW_CTRL=n
```

- `CONFIG_BT_NIMBLE_TRANSPORT_UART_PORT` picks the UART. Use one the
  application does not use for anything else; UART0 is the console.
- The pins (`CONFIG_BT_NIMBLE_UART_TX_PIN`/`RX_PIN`) and the baud rate do not
  matter in QEMU.
- The rest of the application, GATT services and pairing configuration
  included, stays as it is. Code that calls the controller API
  (`esp_bt_controller_init()`, `esp_bt_controller_enable()`,
  `esp_ble_tx_power_set()`...) does not link without a controller and needs
  `#if CONFIG_BT_CONTROLLER_ENABLED`.

Tested with ESP-IDF 5.5, 6.0 and 6.1. Bluedroid also has a host-only mode,
but these tests use NimBLE only.

## Quick start

Install Rootcanal and Bumble on the host:

```
pip install -r tests/esp_bt/requirements.txt
```

Start Rootcanal. Its HCI port is 6402 by default:

```
python -m rootcanal
```

Boot the application with UART1 connected to that port. QEMU numbers
`-serial` options from UART0, so the second one is UART1:

```
qemu-system-xtensa -nographic -machine esp32 -m 4M \
    -drive file=flash.bin,if=mtd,format=raw \
    -serial mon:stdio -serial tcp:127.0.0.1:6402
```

The application logs that the NimBLE host has synced, with the address
Rootcanal gave its controller. From another terminal, scan with Bumble:

```
bumble-scan tcp-client:127.0.0.1:6402
```

It lists `qemu-ble` (the test application's name) next to the two beacons
Rootcanal always simulates (`gDevice-beacon`). Other Bumble tools work the
same way, for example `bumble-gatt-dump` or `bumble-pair`, or a Python
script using `bumble.device.Device`.

| Chip | QEMU | UARTs | HCI on UART1 | HCI on UART2 |
| --- | --- | --- | --- | --- |
| ESP32 | `qemu-system-xtensa -machine esp32` | 3 | `-serial mon:stdio -serial tcp:...` | `-serial mon:stdio -serial null -serial tcp:...` |
| ESP32-S3 | `qemu-system-xtensa -machine esp32s3` | 3 | same | same |
| ESP32-C3 | `qemu-system-riscv32 -machine esp32c3` | 2 | same | - |

QEMU connects to Rootcanal when it starts, so start Rootcanal first. If QEMU
is restarted, Rootcanal gives the new connection the address it had before,
so a peer bonded with the chip finds it again.

## Connecting other hosts

Anything that speaks H4 HCI over TCP can take the other end of the virtual
radio. The automated tests use Bumble; the other two routes below have not
been tried with this setup yet.

- **Bumble**: `tcp-client:<host>:6402`, as above.
- **BlueZ**: `bumble-hci-bridge` can expose a Rootcanal controller as a Linux
  virtual HCI device (`vhci`), so that `bluetoothctl` and BlueZ applications
  see it as an adapter.
- **The Android emulator** runs Rootcanal inside
  [netsim](https://android.googlesource.com/platform/tools/netsim/), which
  serves gRPC rather than a raw HCI port. Bumble's `android-netsim` transport
  connects to it, so `bumble-hci-bridge` can join the emulated chip to the
  emulator's radio in place of a standalone Rootcanal:

  ```
  bumble-hci-bridge tcp-server:_:6402 android-netsim:<emulator-host>:<grpc-port>,name=esp32
  ```

  QEMU then connects to port 6402 of the bridge. The emulator sees the chip
  as one more nearby device, and an Android application scans for it and
  connects as it would to real hardware. netsim's gRPC port is in its `.ini`
  file and listens on localhost; when the emulator runs in a container,
  publish or forward that port.

## Tests

`tests/esp_bt/run_tests.py` builds the test application in
`tests/esp_bt/apps/peripheral` (a NimBLE peripheral with readable, writable,
notifying and encryption-only characteristics) for each IDF version and
chip, and runs it against Rootcanal with a Bumble host as the central:

| Test | What it checks |
| --- | --- |
| `advertise` | The NimBLE host syncs with the controller over the UART; a scanner sees the advertisement |
| `gatt` | Connect, discover, read, MTU exchange to 247, subscribe, a 244-byte write notified back and read back |
| `pairing` | An encryption-only read is refused, LE Secure Connections pairing with bonding, then the read succeeds |
| `reconnect` | Three disconnect/reconnect cycles, each encrypted with the stored LTK and no new pairing |
| `reboot` | The chip restarts; the bond kept in NVS still encrypts the link |
| `uart2` | HCI on UART2 instead of UART1 (ESP32 and ESP32-S3) |

```
pip install -r tests/esp_bt/requirements.txt
tests/esp_bt/run_tests.py --idf ~/esp/esp-idf-v6.1 --idf ~/esp/esp-idf-v5.5.6 \
    --chips esp32,esp32c3,esp32s3 --report bt-report.md
```

Each run keeps the console log, the Rootcanal log and an HCI capture per
controller (`rootcanal_<n>_<address>.pcap`, readable with Wireshark) under
`esp_bt_work/runs/<idf>/<chip>/<test>/`.

## Debugging

- Rootcanal logs every HCI command it receives. A command it does not
  implement shows as `Unimplemented command`; the host then waits for a reply
  that never comes, and NimBLE reports `HCI wait for ack returned 19` and
  resets.
- `python -m rootcanal -enable_hci_sniffer` writes an HCI capture per
  controller in its working directory.
- NimBLE keeps bonds in NVS. If the controller address changes between runs
  with the same flash image, NimBLE logs `ble_store_config_write_local_irk
  rc=27`: start from a fresh image.

## Known limits

- The firmware is not the production build: the controller and the UART
  transport are configured differently. The application and the host stack
  are the same code.
- No controller behaviour of the chip: no Espressif vendor HCI commands, no
  TX power, no Wi-Fi/Bluetooth coexistence, no sleep modes.
- Radio timing, RSSI and interference are Rootcanal's, not real.
- Bluetooth LE only; Classic Bluetooth (BR/EDR) has not been tried.
