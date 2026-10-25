/*
 * Baseline JPEG test frames for emulated camera sensors
 *
 * A small baseline encoder (ITU T.81 Annex K tables), so that a sensor
 * model can hand the guest a real JPEG of the size and quality it was
 * configured for, without a dependency on libjpeg.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "hw/misc/cam_jpeg.h"
#include <math.h>

/* Natural (row-major) index of each coefficient in zigzag order */
static const uint8_t zigzag[64] = {
     0,  1,  8, 16,  9,  2,  3, 10, 17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

/* Annex K.1 quantization tables, natural order */
static const uint8_t std_luma_qt[64] = {
    16, 11, 10, 16,  24,  40,  51,  61,
    12, 12, 14, 19,  26,  58,  60,  55,
    14, 13, 16, 24,  40,  57,  69,  56,
    14, 17, 22, 29,  51,  87,  80,  62,
    18, 22, 37, 56,  68, 109, 103,  77,
    24, 35, 55, 64,  81, 104, 113,  92,
    49, 64, 78, 87, 103, 121, 120, 101,
    72, 92, 95, 98, 112, 100, 103,  99,
};

static const uint8_t std_chroma_qt[64] = {
    17, 18, 24, 47, 99, 99, 99, 99,
    18, 21, 26, 66, 99, 99, 99, 99,
    24, 26, 56, 99, 99, 99, 99, 99,
    47, 66, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
};

/* Annex K.3 Huffman tables: code counts per length 1-16, then symbols */
static const uint8_t dc_luma_bits[16] = {
    0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0,
};
static const uint8_t dc_chroma_bits[16] = {
    0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0,
};
static const uint8_t dc_vals[12] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11,
};

static const uint8_t ac_luma_bits[16] = {
    0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d,
};
static const uint8_t ac_luma_vals[162] = {
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06,
    0x13, 0x51, 0x61, 0x07, 0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08,
    0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0, 0x24, 0x33, 0x62, 0x72,
    0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28,
    0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45,
    0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
    0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75,
    0x76, 0x77, 0x78, 0x79, 0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
    0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3,
    0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6,
    0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9,
    0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
    0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4,
    0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa,
};

static const uint8_t ac_chroma_bits[16] = {
    0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77,
};
static const uint8_t ac_chroma_vals[162] = {
    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41,
    0x51, 0x07, 0x61, 0x71, 0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91,
    0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0, 0x15, 0x62, 0x72, 0xd1,
    0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26,
    0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44,
    0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58,
    0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74,
    0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
    0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a,
    0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4,
    0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7,
    0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda,
    0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4,
    0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa,
};

typedef struct {
    uint16_t code[256];
    uint8_t len[256];
} HuffTable;

typedef struct {
    GByteArray *out;
    uint32_t bits;
    int nbits;
    HuffTable dc[2];
    HuffTable ac[2];
    uint8_t qt[2][64];
    int prev_dc[3];
} JpegEncoder;

static void build_huff(HuffTable *t, const uint8_t *bits, const uint8_t *vals)
{
    uint16_t code = 0;
    int k = 0;

    for (int len = 1; len <= 16; len++) {
        for (int i = 0; i < bits[len - 1]; i++) {
            t->code[vals[k]] = code++;
            t->len[vals[k]] = len;
            k++;
        }
        code <<= 1;
    }
}

static void scale_qt(uint8_t *qt, const uint8_t *base, int quality)
{
    const int scale = quality < 50 ? 5000 / quality : 200 - quality * 2;

    for (int i = 0; i < 64; i++) {
        qt[i] = MAX(1, MIN(255, (base[i] * scale + 50) / 100));
    }
}

static void put_bits(JpegEncoder *e, uint32_t value, int count)
{
    e->bits = (e->bits << count) | (value & ((1u << count) - 1));
    e->nbits += count;
    while (e->nbits >= 8) {
        const uint8_t byte = e->bits >> (e->nbits - 8);
        g_byte_array_append(e->out, &byte, 1);
        if (byte == 0xff) {
            const uint8_t stuff = 0;
            g_byte_array_append(e->out, &stuff, 1);
        }
        e->nbits -= 8;
    }
}

static void put_huff(JpegEncoder *e, const HuffTable *t, uint8_t symbol)
{
    put_bits(e, t->code[symbol], t->len[symbol]);
}

/* The magnitude category of a coefficient and its appended bits */
static int category(int v)
{
    int n = 0;

    for (v = abs(v); v; v >>= 1) {
        n++;
    }
    return n;
}

static void put_value(JpegEncoder *e, int v, int size)
{
    put_bits(e, v < 0 ? v + (1 << size) - 1 : v, size);
}

/* dct[u][x] = C(u) / 2 * cos((2x + 1) u pi / 16): the 8-point DCT-II */
static float dct[8][8];

static void init_dct(void)
{
    for (int u = 0; u < 8; u++) {
        for (int x = 0; x < 8; x++) {
            dct[u][x] = (u ? 0.5f : 0.5f * M_SQRT1_2) *
                        cosf((2 * x + 1) * u * M_PI / 16);
        }
    }
}

static void encode_block(JpegEncoder *e, const float *block, int comp)
{
    const int t = comp ? 1 : 0;
    float rows[64];
    int q[64];

    /* Separable 2D DCT: transform each row, then each column */
    for (int y = 0; y < 8; y++) {
        for (int u = 0; u < 8; u++) {
            float sum = 0;
            for (int x = 0; x < 8; x++) {
                sum += dct[u][x] * block[y * 8 + x];
            }
            rows[y * 8 + u] = sum;
        }
    }
    for (int u = 0; u < 8; u++) {
        for (int v = 0; v < 8; v++) {
            float sum = 0;
            for (int y = 0; y < 8; y++) {
                sum += dct[v][y] * rows[y * 8 + u];
            }
            q[v * 8 + u] = lrintf(sum / e->qt[t][v * 8 + u]);
        }
    }

    const int diff = q[0] - e->prev_dc[comp];
    e->prev_dc[comp] = q[0];
    const int dc_size = category(diff);
    put_huff(e, &e->dc[t], dc_size);
    put_value(e, diff, dc_size);

    int run = 0;
    for (int i = 1; i < 64; i++) {
        const int v = q[zigzag[i]];
        if (v == 0) {
            run++;
            continue;
        }
        for (; run > 15; run -= 16) {
            put_huff(e, &e->ac[t], 0xf0);
        }
        const int size = category(v);
        put_huff(e, &e->ac[t], (run << 4) | size);
        put_value(e, v, size);
        run = 0;
    }
    if (run) {
        put_huff(e, &e->ac[t], 0x00);
    }
}

static void put_marker(GByteArray *out, uint8_t marker, const uint8_t *data,
                       size_t len)
{
    const uint8_t head[4] = { 0xff, marker, (len + 2) >> 8, (len + 2) & 0xff };

    g_byte_array_append(out, head, sizeof(head));
    g_byte_array_append(out, data, len);
}

static void put_dht(GByteArray *out, uint8_t class_id, const uint8_t *bits,
                    const uint8_t *vals)
{
    uint8_t seg[1 + 16 + 162];
    size_t n = 0;

    seg[0] = class_id;
    memcpy(seg + 1, bits, 16);
    for (int i = 0; i < 16; i++) {
        n += bits[i];
    }
    memcpy(seg + 17, vals, n);
    put_marker(out, 0xc4, seg, 17 + n);
}

static void put_headers(JpegEncoder *e, unsigned w, unsigned h,
                        const char *comment)
{
    static const uint8_t soi[2] = { 0xff, 0xd8 };
    static const uint8_t jfif[14] = {
        'J', 'F', 'I', 'F', 0, 1, 1, 0, 0, 1, 0, 1, 0, 0,
    };
    uint8_t dqt[65];

    g_byte_array_append(e->out, soi, sizeof(soi));
    put_marker(e->out, 0xe0, jfif, sizeof(jfif));
    if (comment) {
        put_marker(e->out, 0xfe, (const uint8_t *)comment, strlen(comment));
    }
    for (int t = 0; t < 2; t++) {
        dqt[0] = t;
        for (int i = 0; i < 64; i++) {
            dqt[1 + i] = e->qt[t][zigzag[i]];
        }
        put_marker(e->out, 0xdb, dqt, sizeof(dqt));
    }

    const uint8_t sof[15] = {
        8, h >> 8, h & 0xff, w >> 8, w & 0xff, 3,
        1, 0x22, 0,     /* Y: 2x2 sampling, table 0 */
        2, 0x11, 1,     /* Cb */
        3, 0x11, 1,     /* Cr */
    };
    put_marker(e->out, 0xc0, sof, sizeof(sof));

    put_dht(e->out, 0x00, dc_luma_bits, dc_vals);
    put_dht(e->out, 0x10, ac_luma_bits, ac_luma_vals);
    put_dht(e->out, 0x01, dc_chroma_bits, dc_vals);
    put_dht(e->out, 0x11, ac_chroma_bits, ac_chroma_vals);

    static const uint8_t sos[10] = { 3, 1, 0x00, 2, 0x11, 3, 0x11, 0, 63, 0 };
    put_marker(e->out, 0xda, sos, sizeof(sos));
}

/*
 * Colour bars (white, yellow, cyan, green, magenta, red, blue, black),
 * darkening towards the bottom
 */
static void pattern_pixel(unsigned x, unsigned y, unsigned w, unsigned h,
                          float *yy, float *cb, float *cr)
{
    static const uint8_t bars[8][3] = {
        { 255, 255, 255 }, { 255, 255, 0 }, { 0, 255, 255 }, { 0, 255, 0 },
        { 255, 0, 255 }, { 255, 0, 0 }, { 0, 0, 255 }, { 0, 0, 0 },
    };
    const uint8_t *c = bars[MIN(x * 8 / w, 7)];
    const float shade = 1.0f - 0.6f * y / h;
    const float r = c[0] * shade, g = c[1] * shade, b = c[2] * shade;

    *yy = 0.299f * r + 0.587f * g + 0.114f * b;
    *cb = -0.168736f * r - 0.331264f * g + 0.5f * b + 128;
    *cr = 0.5f * r - 0.418688f * g - 0.081312f * b + 128;
}

GByteArray *cam_jpeg_test_pattern(unsigned width, unsigned height,
                                  int quality, const char *comment)
{
    JpegEncoder e = { .out = g_byte_array_new() };

    quality = MAX(1, MIN(100, quality));
    init_dct();
    build_huff(&e.dc[0], dc_luma_bits, dc_vals);
    build_huff(&e.dc[1], dc_chroma_bits, dc_vals);
    build_huff(&e.ac[0], ac_luma_bits, ac_luma_vals);
    build_huff(&e.ac[1], ac_chroma_bits, ac_chroma_vals);
    scale_qt(e.qt[0], std_luma_qt, quality);
    scale_qt(e.qt[1], std_chroma_qt, quality);
    put_headers(&e, width, height, comment);

    for (unsigned my = 0; my < height; my += 16) {
        for (unsigned mx = 0; mx < width; mx += 16) {
            float y[4][64], cb[64] = { 0 }, cr[64] = { 0 };

            for (int py = 0; py < 16; py++) {
                for (int px = 0; px < 16; px++) {
                    /* Past the right or bottom edge, repeat the last pixel */
                    const unsigned x = MIN(mx + px, width - 1);
                    const unsigned yy = MIN(my + py, height - 1);
                    float l, b, r;
                    pattern_pixel(x, yy, width, height, &l, &b, &r);
                    y[(py / 8) * 2 + px / 8][(py % 8) * 8 + px % 8] = l - 128;
                    cb[(py / 2) * 8 + px / 2] += (b - 128) / 4;
                    cr[(py / 2) * 8 + px / 2] += (r - 128) / 4;
                }
            }
            for (int i = 0; i < 4; i++) {
                encode_block(&e, y[i], 0);
            }
            encode_block(&e, cb, 1);
            encode_block(&e, cr, 2);
        }
    }

    /* Pad the last byte with 1 bits, then EOI */
    if (e.nbits) {
        put_bits(&e, 0x7f, 8 - e.nbits);
    }
    static const uint8_t eoi[2] = { 0xff, 0xd9 };
    g_byte_array_append(e.out, eoi, sizeof(eoi));
    return e.out;
}
