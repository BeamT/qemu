# Wi-Fi emulation for ESP32, ESP32-C3 and ESP32-S3

The `esp32`, `esp32c3` and `esp32s3` machines can emulate the chip's Wi-Fi
MAC, so that
unmodified ESP-IDF applications using `esp_wifi` run in QEMU: as a station
joining an access point, as a SoftAP serving clients, and with ESP-NOW between
several QEMU instances. Traffic leaves the emulated chip through an ordinary
QEMU network backend (`user`, `socket`, `dgram`, `tap`...).

Espressif does not document the Wi-Fi hardware. Everything below was learnt
by tracing what the IDF driver does in a running guest; the
[register map](#register-map) gives a confidence level for each register.

## Quick start

Station, joining the emulated access point "Espressif" and reaching the host
network through slirp:

```
qemu-system-xtensa -nographic -machine esp32 -m 4M \
    -drive file=flash.bin,if=mtd,format=raw \
    -nic user,model=misc.esp32_wifi
```

| Chip | QEMU | NIC model |
| --- | --- | --- |
| ESP32 | `qemu-system-xtensa -machine esp32` | `misc.esp32_wifi` |
| ESP32-C3 | `qemu-system-riscv32 -machine esp32c3` | `misc.esp32c3_wifi` |
| ESP32-S3 | `qemu-system-xtensa -machine esp32s3` | `misc.esp32s3_wifi` |

For the other chips see [Chip status](#chip-status).

The device is only created when a `-nic` (or `-net nic`) asks for this
model. It replaces `open_eth` for that network interface; on the ESP32-C3 and
ESP32-S3 both use interrupt source 0, so only one of them can be present.

Backends whose options contain dots, such as `dgram`, cannot be given to
`-nic`; use `-netdev dgram,id=n0,... -net nic,netdev=n0,model=...`.

Give each emulated chip its own MAC address in eFuse, otherwise all of them
use `00:00:00:00:00:00`:

```
tests/esp_wifi/mkefuse.py --chip esp32 02:00:00:00:01:00 efuse.bin   # or esp32c3, esp32s3
qemu-system-xtensa ... \
    -drive file=efuse.bin,if=none,format=raw,id=efuse \
    -global driver=nvram.esp32.efuse,property=drive,value=efuse
```

IDF gives the SoftAP, Bluetooth and Ethernet interfaces the next addresses
after the base MAC, so space the base addresses of several instances apart
(`…:01:00`, `…:02:00`), not consecutively.

## Configuring the emulated access points

Up to four access points can be configured with device properties. Because
the type name contains a dot, use the long form of `-global`:

```
-global driver=misc.esp32_wifi,property=ap0-ssid,value=home
-global driver=misc.esp32_wifi,property=ap0-channel,value=6
-global driver=misc.esp32_wifi,property=ap0-rssi,value=-55
-global driver=misc.esp32_wifi,property=ap0-psk,value=secret-passphrase
```

| Property | Default | Meaning |
| --- | --- | --- |
| `apN-ssid` (N = 0…3) | `ap0-ssid` = `Espressif`, others unset | SSID; an AP without SSID does not exist |
| `apN-channel` | 1 | Channel 1–14; the AP is only heard on its channel |
| `apN-rssi` | −40 | Signal strength reported to the guest, in dBm |
| `apN-psk` | unset | WPA2-PSK passphrase (8–63 characters); unset is an open network |
| `client-psk` | unset | Passphrase used by the emulated clients of a WPA2 SoftAP |
| `ack-timeout-us` | 20000 | How long a unicast ESP-NOW frame waits for its ACK |
| `beacon-interval` | 100 | Beacon interval of the emulated APs, in TU |

The BSSID of access point N is `12:01:00:c4:0a:5N`.

## Station mode

The emulated access point answers probe requests, sends beacons on its
channel, accepts open-system authentication and association, and for WPA2-PSK
runs the 4-way handshake (PMK from the passphrase, PTK, GTK delivery), so the
driver installs real keys. Like a real AP it sends message 1 shortly after the
association response and retransmits messages 1 and 3 every second, up to four
times, until the station answers. Data frames are then translated:

- frames from the guest to the AP become Ethernet frames on the netdev,
  source the guest's MAC, destination the final destination;
- Ethernet frames on the netdev addressed to the guest (or broadcast) become
  802.11 data frames from the AP.

With `-nic user` the guest gets its address from slirp's DHCP server
(10.0.2.15) and reaches the host as 10.0.2.2.

## SoftAP mode

When the guest starts a SoftAP, the model learns its BSSID, SSID, channel and
security from the beacons it transmits. For each host on the netdev that sends
traffic, the model emulates a station: it authenticates, associates and, for
WPA2, completes the 4-way handshake with `client-psk`, then forwards that
host's Ethernet frames to the SoftAP and the SoftAP's frames back. The guest
sees one client per host MAC, and serves it DHCP like a real client.

The emulated clients use the AKM the SoftAP offers: WPA2-PSK, or PSK-SHA256
with protected management frames when the SoftAP requires PMF (the IDF
`softAP` example does). A WPA3-SAE-only SoftAP is reported with `-d unimp`
and cannot be joined; build it with `CONFIG_ESP_WIFI_SOFTAP_SAE_SUPPORT=n`.

Connect the SoftAP to a network where the clients live, for example a `tap`
interface, a `socket`/`stream` link to another VM, or a `dgram` socket to a
test program (as `tests/esp_wifi/run_matrix.py` does). `-nic user` is of
little use here, since slirp would compete with the SoftAP's DHCP server.

## ESP-NOW and other QEMU instances

Frames that do not belong to the emulated APs or clients (ESP-NOW and other
action frames) are sent whole to the netdev, inside Ethernet frames with
EtherType `0x88b5` (IEEE local experimental):

| Offset | Size | Content |
| --- | --- | --- |
| 0 | 6 | Destination: receiver address of the frame, or broadcast |
| 6 | 6 | Source: transmitter address |
| 12 | 2 | `0x88b5` |
| 14 | 4 | `QWIF` |
| 18 | 1 | Version, 1 |
| 19 | 1 | Channel the frame was sent on |
| 20 | 2 | Reserved, 0 |
| 22 | n | 802.11 frame, without FCS |

A receiving instance on the same channel delivers frames addressed to it, or
broadcast, and answers unicast frames with an ACK frame in the same
encapsulation. The sender reports success if that ACK arrives within
`ack-timeout-us`, failure otherwise, so the ESP-NOW send callback gets the
same status as on hardware.

Connect the instances with a shared netdev, for example:

```
-nic socket,model=misc.esp32_wifi,mcast=230.0.0.1:1234
```

## Architecture

```
          hw/misc/esp32_wifi.c        hw/misc/esp32c3_wifi.c (C3 and S3)
          register offsets, RX        register offsets, RX header,
          header, radio quirks        radio quirks
                       \                    /
                        hw/misc/esp_wifi.c
     MAC registers, TX slots, RX descriptor chain, interrupts, radio blocks
                                  |
                        hw/misc/esp_wifi_air.c
     emulated APs, SoftAP clients, WPA2 handshake, 802.11 <-> Ethernet,
     ESP-NOW tunnel
                                  |
                           QEMU netdev
```

`TYPE_ESP_WIFI` is an abstract sysbus device. A chip subclass provides an
`EspWifiMacLayout` with the offsets the core acts on, a table of radio blocks
(PHY, RF front end, analog, baseband) that the PHY calibration code writes and
polls, the size and contents of the RX control header the MAC writes in front
of each frame, and hooks for radio register quirks such as the channel. The
machine maps the regions at the addresses in that table.

### Transmission

The driver writes a DMA descriptor address with bits 31:30 set into the
`PLCP0` register of one of five TX slots. The model gathers the frame from the
descriptor chain, drops the 4 bytes reserved for the FCS (and, for protected
frames, the 8 bytes reserved for the CCMP MIC), and hands it to the medium.
Some 20 µs later, or when the ACK arrives or times out, it sets the slot's bit
in the TX complete register, writes the PHY TX status (`0` acknowledged,
`0x2300` no ACK) and raises the TX done interrupt.

### Reception

The driver sets up a chain of RX descriptors and writes the first to `RX_BASE`.
The model fills one descriptor per frame (owner bit set, EOF clear, large
enough), writes the RX control header and the frame, sets EOF and the length,
and raises the RX interrupt. When the chain runs out, `RX_NEXT` reads as the
bare DRAM base; the driver appends descriptors and sets bit 0 of `RX_CTRL`,
and the model continues after the last descriptor it filled. Frames that
arrive while no descriptor is available wait in a queue of 64, which also
throttles the netdev.

### Encryption

The model does not encrypt. On a protected link the driver hands frames to
the MAC with the CCMP header and room for the MIC, and expects received frames
in the same shape after decryption; the model strips and adds those fields.
Key material installed by the driver is ignored.

## Register map

Confidence levels:

- **High**: behaviour observed in traces with all tested IDF versions, the
  driver depends on it, and an independent source agrees.
- **Medium**: observed and needed, single source.
- **Low**: needed for the driver to proceed, meaning unknown.

### ESP32 MAC (`0x3ff73000`, 8 KiB)

| Offset | Name in the model | Behaviour | Confidence |
| --- | --- | --- | --- |
| `0x000`–`0x02c` | BSSID filters | stored | Medium |
| `0x040`, `0x048` | `addr_filter[0/1]` | MAC address of interface 0 (STA) and 1 (AP), 6 bytes little endian | High |
| `0x060`–`0x06c` | address filter masks | stored | Medium |
| `0x084` | `RX_CTRL` | bit 0: reload RX descriptors, reads back 0 | High |
| `0x088` | `RX_BASE` | first RX descriptor | High |
| `0x08c` | `RX_NEXT` | descriptor the next frame goes to; DRAM base when none | High |
| `0x090` | `RX_LAST` | last descriptor filled | High |
| `0x0d8`–`0x0e4` | RX policy | stored | Low |
| `0xc48` | `INT_STATUS` | interrupt causes: `0x1000024` RX, `0x80` TX done | High |
| `0xc4c` | `INT_CLEAR` | write 1 to clear | High |
| `0xcb8` | `MAC_CTRL` | bit 13 busy, reads 0 | Medium |
| `0xcbc` / `0xcc0` | TXQ error clear / status | bits 15:0 collision, 31:16 timeout, per slot | Medium |
| `0xcc4` / `0xcc8` | TXQ complete clear / status | bit n: slot n done | High |
| `0xd1c − 8n` | `TX_CONFIG(n)` | stored | Medium |
| `0xd20 − 8n` | `PLCP0(n)` | bits 19:0 descriptor address, bits 31:30 start | High |
| `0xd24` | TX DMA state | bit 1 stop request (self clearing), bit 0 idle | Medium |
| `0x1258 − 0x3c·n` | `PLCP1(n)` | bits 11:0 length including FCS | High |
| `0x1270` | PHY TX status | `0` acknowledged, `0x2300` no ACK | Medium |

Every other offset is stored as plain memory and reported once with
`-d unimp`.

### ESP32 radio blocks

| Block | Address | Behaviour | Confidence |
| --- | --- | --- | --- |
| FE2 | `0x3ff45000` | reads `0xffffffff` | Low |
| FE | `0x3ff46000` | stored; `0x7c`, `0x174` read `0xffffffff` | Low |
| Analog | `0x3ff4e000` | stored; `0x04` reads `0xfdffffff`, `0x44`, `0x4c`, `0xc4` read `0xffffffff`; a write to `0xc4` whose low byte is `10·(ch+1)+4` selects channel `ch` | Medium (channel), Low (rest) |
| BT | `0x3ff51000` | stored | Low |
| NRX | `0x3ff5c000` | reads `0xffffffff` | Low |
| BB | `0x3ff5d000` | reads `0xffffffff` | Low |
| PHY | `0x3ff71000` | reads `0xffffffff` | Low |
| WDEV | `0x3ff75000` | stored (RNG at `0x144` is the RNG model) | Low |

Also required: `DPORT_WIFI_CLK_EN` (`0x3ff000cc`) and `DPORT_WIFI_RST_EN`
(`0x3ff000d0`) must read back what was written; IDF 5.3 and later check the
modem clock bits.

### ESP32-C3 MAC (`0x60033000`, 8 KiB)

| Offset | Behaviour | Confidence |
| --- | --- | --- |
| `0x040`, `0x048` | as ESP32 | High |
| `0x084`–`0x090` | as ESP32 | High |
| `0xc3c` / `0xc40` | `INT_STATUS` / `INT_CLEAR`; RX cause `0x1004000` | High |
| `0xca4`–`0xcb0` | TXQ error clear, error, complete clear, complete | Medium |
| `0xd08 − 8n` | `PLCP0(n)` | Medium |
| `0xd14` | TX DMA state, as ESP32 `0xd24` | Medium |
| `0x1320` | PHY TX status | Medium |

The TX complete (`0xcb0`, cleared through `0xcac`) and PHY TX status
(`0x1320`) offsets are confirmed by traces of ESP-NOW sends; the TX error
pair (`0xca4`/`0xca8`) is inferred from the ESP32 ordering and has not been
seen in use.

### ESP32-C3 radio blocks

| Block | Address | Behaviour | Confidence |
| --- | --- | --- | --- |
| FE2 | `0x60005000` | stored | Low |
| FE | `0x60006000` | as ESP32 | Low |
| Analog (RTC I2C) | `0x6000e000` | `0x00`, `0x48` read `0x00ffffff`; `0x50` reads bits 26:24 set; bits 27:20 of a write to `0x150` are the PLL frequency in MHz above 2400, i.e. `7 + 5·ch` | Medium (channel), Low (rest) |
| NRX | `0x6001c000` | reads `0xffffffff` | Low |
| BB | `0x6001d000` | reads `0xffffffff` | Low |
| WDEV | `0x60035000` | stored; `0x07c` returns random data (the RNG data register, as on the ESP32-S3; the PHY library stalls if it reads 0); `0x118`, `0x128` read 0 | Medium (`0x07c`), Low (rest) |

`SYSCON_WIFI_CLK_EN` (`0x60026014`) and `SYSCON_WIFI_RST_EN` (`0x60026018`)
read back what was written.

### ESP32-S3

Same MAC as the ESP32-C3, at the same address, with RX interrupt cause
`0x1004024`. Radio blocks as the ESP32-C3, plus BT (`0x60011000`, stored)
and PHY (`0x60031000`, reads `0xffffffff`); FE2 reads `0xffffffff`. The RNG
model keeps the WDEV word `0x07c`. The PHY calibration also waits for the
temperature sensor: `SENS_SAR_TSENS_CTRL` (`0x60008850`) reports ready with a
raw value of 104 (about 25 °C).

### RX control header

ESP32: 28 bytes. ESP32-C3 and ESP32-S3: 48 bytes. Word 0 is common: RSSI in
bits 7:0 (ESP32: dBm + 96; ESP32-C3/S3: dBm), rate 12:8, length 27:16, address filter
matches 31:28. The channel is in bits 19:16 of word 2, the timestamp (µs) in
word 3, and the length again in the last word, bits 11:0 and 23:12.

## Chip status

| Chip | Emulated by esp-develop | Wi-Fi | Notes |
| --- | --- | --- | --- |
| ESP32 | yes | supported | |
| ESP32-C3 | yes | supported | |
| ESP32-S3 | yes | supported | Same MAC generation as the ESP32-C3 |
| ESP32-S2 | no | not ported | No machine to run the driver on |
| ESP32-C6 | yes | not ported | Wi-Fi 6 MAC and PHY in the modem area at `0x600a0000`–`0x600affff` (802.15.4 at `0x600a3000`), unrelated to the other layouts. The driver first stops on the modem clock check (`MODEM_SYSCON` at `0x600a9800` is not modelled); with that register block stored, PHY calibration polls `0x600ad000` and the MAC is initialised around `0x600a00c0`. |

## Testing

`tests/esp_wifi/run_matrix.py` builds IDF examples and test apps for each IDF
version and chip, runs them, and writes a report:

```
tests/esp_wifi/run_matrix.py --idf ~/esp/esp-idf-v6.1 --idf ~/esp/esp-idf-v5.5.6 \
    --chips esp32,esp32c3,esp32s3 --report report.md
```

| Test | Source | Pass condition |
| --- | --- | --- |
| `station` | `wifi/getting_started/station` | "got ip" |
| `station_wpa2` | same, WPA2-PSK | "got ip" |
| `http_request` | `protocols/http_request`, server moved to the host | HTTP 200 |
| `scan` | `wifi/scan` | three APs, each listed once with its channel, RSSI and auth mode |
| `softap`, `softap_wpa2` | `wifi/getting_started/softAP` | a host on a `dgram` netdev gets a DHCP lease |
| `espnow` | `wifi/espnow`, two instances | both receive, unicast exchange, no send error |
| `espnow_ack`, `espnow_ack_encrypted` | `tests/esp_wifi/apps/espnow_ack` | every frame to the peer acknowledged, every frame to a missing station failed |
| `iperf` | `wifi/iperf` | TCP and UDP to the host for 60 s, every 5 s interval above 0, no gap over 3 s at the host |
| `restart` | `tests/esp_wifi/apps/restart` | reconnects after `esp_restart()` during UDP traffic |

Every run keeps its serial log, pcap and `-d unimp` log under
`--work/runs/<idf>/<chip>/<test>/`. `--icount` repeats the matrix with
`-icount shift=auto`. The `WifiTests` job in `.github/workflows/build.yml`
runs the matrix for each IDF version on every push.

## Debugging

```
-trace 'esp_wifi_*'        # every MAC/radio access, frames, links, handshakes
-trace 'esp_wifi_[^mr]*'   # frames, channel, links only
-d unimp                   # MAC registers the model does not know yet
```

## Known limits

- No radio modelling: no RSSI variation, interference, airtime, or rate
  adaptation. Frames are delivered after a fixed short delay.
- One guest station per emulated AP. Up to four emulated SoftAP clients.
- WPA2-PSK (CCMP) only, plus PSK-SHA256 with PMF for the emulated SoftAP
  clients. No WEP, TKIP or enterprise; the emulated APs do not offer PMF.
- No 802.11n aggregation, power save buffering or DTIM handling: frames are
  delivered as soon as they arrive.
- Frames are not encrypted on the netdev; ESP-NOW with encryption works, but
  the tunnel carries clear text.
- No WPA3-SAE, so a WPA3-only SoftAP cannot be joined by the emulated
  clients, and the emulated APs offer WPA2-PSK only.
- Channel decoding relies on the PLL programming sequence; a new PHY library
  that programs the PLL differently will make every AP visible on every
  channel (the model treats an unknown channel as "all").
- The MAC acknowledges unicast frames as soon as the driver has set its
  address, as hardware does, even if no application listens yet.
