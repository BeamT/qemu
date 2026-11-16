<!--
Proposed "Wi-Fi support" section for espressif/esp-toolchain-docs,
qemu/esp32/README.md (and the ESP32-C3/S3 pages), to sit after
"Ethernet support". Not part of the QEMU tree; drop this file when the
section has been submitted there.
-->

### Wi-Fi support

QEMU can emulate the ESP32, ESP32-C3 and ESP32-S3 Wi-Fi MAC, so applications
that use `esp_wifi` run unmodified: as a station, as a SoftAP, and with
ESP-NOW between several QEMU instances. No change to the application or to
sdkconfig is needed.

Use the Wi-Fi network device instead of `open_eth`:

| Chip | Network device |
| --- | --- |
| ESP32 | `misc.esp32_wifi` |
| ESP32-C3 | `misc.esp32c3_wifi` |
| ESP32-S3 | `misc.esp32s3_wifi` |

Give each emulated chip its own MAC address, otherwise all of them use
`00:00:00:00:00:00` (see [Specifying eFuse storage](#specifying-efuse-storage)):

```
tests/esp_wifi/mkefuse.py --chip esp32 02:00:00:00:01:00 qemu_efuse.bin
```

Space the addresses of several instances apart (`…:01:00`, `…:02:00`): IDF
gives the SoftAP interface the base address plus one.

#### Station mode

QEMU emulates an access point called `Espressif`, an open network on channel
1. With user mode networking the application gets the address 10.0.2.15 from
it and reaches the host as 10.0.2.2:

```
-nic user,model=misc.esp32_wifi
```

To join a WPA2 network, or to scan several networks, configure up to four
access points (`ap0` to `ap3`):

```
-global driver=misc.esp32_wifi,property=ap0-ssid,value=myssid
-global driver=misc.esp32_wifi,property=ap0-psk,value=mypassword
-global driver=misc.esp32_wifi,property=ap0-channel,value=6
-global driver=misc.esp32_wifi,property=ap0-rssi,value=-55
```

Port forwarding works as with Ethernet:

```
-nic user,model=misc.esp32_wifi,hostfwd=tcp:127.0.0.1:2222-:2222
```

#### SoftAP mode

Each host on the network that sends traffic to the SoftAP is presented to the
application as a Wi-Fi station that joins it, and gets its address from the
SoftAP's DHCP server. Connect the SoftAP to a network with clients, for
example a [tap interface](tap-networking.md):

```
-nic tap,model=misc.esp32_wifi,ifname=tap0,script=no,downscript=no
```

For a WPA2 SoftAP, give the passphrase the emulated stations use:

```
-global driver=misc.esp32_wifi,property=client-psk,value=mypassword
```

WPA3-SAE is not emulated; build WPA2 SoftAPs with
`CONFIG_ESP_WIFI_SOFTAP_SAE_SUPPORT=n`.

#### ESP-NOW

Connect the instances to one network, for example a multicast socket, and
give them different MAC addresses:

```
qemu-system-xtensa ... -nic socket,model=misc.esp32_wifi,mcast=230.0.0.1:1234 \
    -drive file=efuse1.bin,if=none,format=raw,id=efuse \
    -global driver=nvram.esp32.efuse,property=drive,value=efuse
qemu-system-xtensa ... -nic socket,model=misc.esp32_wifi,mcast=230.0.0.1:1234 \
    -drive file=efuse2.bin,if=none,format=raw,id=efuse \
    -global driver=nvram.esp32.efuse,property=drive,value=efuse
```

Frames are delivered between instances on the same channel, and the send
callback reports `ESP_NOW_SEND_SUCCESS` only when the peer acknowledged the
frame.

#### Limitations

No radio modelling (RSSI is fixed, no interference or airtime), no WPA3, WEP
or enterprise authentication, no 802.11n aggregation. See `docs/wifi.md` in
the QEMU sources for the details and the register map.
