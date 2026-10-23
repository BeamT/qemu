#!/usr/bin/env python3
#
# Create an eFuse image for the QEMU ESP32 and ESP32-C3 machines with a
# factory MAC address, so that several emulated chips on one network can be
# told apart. A blank image gives every instance the MAC 00:00:00:00:00:00.
#
# SPDX-License-Identifier: GPL-2.0-or-later

import argparse
import struct


def crc8(data):
    """CRC-8, reflected polynomial 0x8c, as esp_crc8() in the ESP32 ROM."""
    crc = 0
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0x8c if crc & 1 else crc >> 1
    return crc


def esp32_image(mac):
    # BLK0: MAC_FACTORY in bits 32..79, most significant byte at bit 72,
    # MAC_FACTORY_CRC in bits 80..87. Image = 7 + 3 * 8 words.
    words = bytearray(31 * 4)
    words[4:10] = bytes(reversed(mac))
    words[10] = crc8(mac)
    return bytes(words)


def esp32c3_image(mac):
    # MAC in EFUSE_RD_MAC_SPI_SYS_0/1 (block 1), stored byte reversed. The
    # ESP32-C3 eFuse image is the full 0x400-byte memory, block 1 at 0x44.
    image = bytearray(0x400)
    image[0x44:0x4a] = bytes(reversed(mac))
    return bytes(image)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--chip', choices=['esp32', 'esp32c3'], default='esp32')
    p.add_argument('mac', help='e.g. 02:00:00:00:00:01')
    p.add_argument('output')
    args = p.parse_args()
    mac = bytes(int(x, 16) for x in args.mac.split(':'))
    assert len(mac) == 6
    image = esp32_image(mac) if args.chip == 'esp32' else esp32c3_image(mac)
    with open(args.output, 'wb') as f:
        f.write(image)


if __name__ == '__main__':
    main()
