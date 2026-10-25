/*
 * Baseline JPEG test frames for emulated camera sensors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */

#pragma once

/**
 * cam_jpeg_test_pattern:
 * @width: frame width in pixels
 * @height: frame height in pixels
 * @quality: IJG quality, 1 (smallest) to 100 (best)
 * @comment: text for a COM segment, or NULL for none
 *
 * Encode colour bars over a vertical gradient as a baseline JFIF JPEG
 * (YCbCr 4:2:0, the standard Huffman tables). A smaller @quality gives a
 * smaller file, as on a real sensor.
 *
 * Returns: the JPEG, from SOI to EOI. Free with g_byte_array_unref().
 */
GByteArray *cam_jpeg_test_pattern(unsigned width, unsigned height,
                                  int quality, const char *comment);
