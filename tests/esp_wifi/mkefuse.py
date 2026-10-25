#!/usr/bin/env python3
#
# Create an eFuse image for the QEMU ESP32, ESP32-C3 and ESP32-S3 machines
# with a factory MAC address, so that several emulated chips on one network
# can be told apart. A blank image gives every instance the MAC
# 00:00:00:00:00:00.
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


def esp32c3_image(mac, revision=True):
    # The image holds the read registers from EFUSE_RD_WR_DIS (0x2c) on.
    # MAC in EFUSE_RD_MAC_SPI_SYS_0/1, byte reversed. On the ESP32-C3, chip
    # revision v0.3 and eFuse block revision v1.3, as the model sets without
    # an image; the ESP32-S3 model leaves them at 0.
    image = bytearray(0x400)
    image[0x18:0x1e] = bytes(reversed(mac))
    if revision:
        sys3 = (3 << 18) | (3 << 24)    # WAFER_VER_MINOR_LO, BLK_VERSION_MINOR
        image[0x24:0x28] = struct.pack('<I', sys3)
        image[0x40:0x44] = struct.pack('<I', 1)     # BLK_VERSION_MAJOR
    return bytes(image)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--chip', choices=['esp32', 'esp32c3', 'esp32s3'],
                   default='esp32')
    p.add_argument('mac', help='e.g. 02:00:00:00:00:01')
    p.add_argument('output')
    args = p.parse_args()
    mac = bytes(int(x, 16) for x in args.mac.split(':'))
    assert len(mac) == 6
    if args.chip == 'esp32':
        image = esp32_image(mac)
    else:
        image = esp32c3_image(mac, args.chip == 'esp32c3')
    with open(args.output, 'wb') as f:
        f.write(image)


if __name__ == '__main__':
    main()
