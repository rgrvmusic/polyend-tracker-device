/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Gerard Braad (Regroove | RGRV) */
/* Host implementations of the Teensy/ARM DSP helpers used by the Tracker core.
 * Same integer semantics as the Cortex-M instructions they replace. */
#ifndef dspinst_h_
#define dspinst_h_

#include <stdint.h>

static inline int32_t signed_saturate_rshift(int32_t val, int bits, int rshift)
{
    int32_t out = val >> rshift;
    int32_t max = (int32_t)(1u << (bits - 1));
    if (out > max - 1) out = max - 1;
    if (out < -max) out = -max;
    return out;
}

static inline int16_t saturate16(int32_t val)
{
    if (val > 32767) return 32767;
    if (val < -32768) return -32768;
    return (int16_t)val;
}

static inline int32_t signed_multiply_32x16b(int32_t a, uint32_t b)
{
    return (int32_t)(((int64_t)a * (int16_t)(b & 0xFFFF)) >> 16);
}

static inline int32_t signed_multiply_32x16t(int32_t a, uint32_t b)
{
    return (int32_t)(((int64_t)a * (int16_t)(b >> 16)) >> 16);
}

static inline int32_t multiply_32x32_rshift32(int32_t a, int32_t b)
{
    return (int32_t)(((int64_t)a * (int64_t)b) >> 32);
}

static inline int32_t multiply_32x32_rshift32_rounded(int32_t a, int32_t b)
{
    return (int32_t)((((int64_t)a * (int64_t)b) + 0x8000000) >> 32);
}

static inline int32_t multiply_accumulate_32x32_rshift32_rounded(int32_t sum, int32_t a, int32_t b)
{
    return sum + multiply_32x32_rshift32_rounded(a, b);
}

static inline int32_t multiply_subtract_32x32_rshift32_rounded(int32_t sum, int32_t a, int32_t b)
{
    return sum - multiply_32x32_rshift32_rounded(a, b);
}

static inline uint32_t pack_16t_16t(int32_t a, int32_t b)
{
    return (uint32_t)((a & 0xFFFF0000) | ((uint32_t)b >> 16));
}

static inline uint32_t pack_16t_16b(int32_t a, int32_t b)
{
    return (uint32_t)((a & 0xFFFF0000) | (b & 0x0000FFFF));
}

static inline uint32_t pack_16b_16b(int32_t a, int32_t b)
{
    return (uint32_t)(((uint32_t)a << 16) | (b & 0x0000FFFF));
}

static inline int16_t sat_add16(int16_t a, int16_t b)
{
    int32_t s = (int32_t)a + (int32_t)b;
    if (s > 32767) s = 32767;
    if (s < -32768) s = -32768;
    return (int16_t)s;
}

static inline int16_t sat_sub16(int16_t a, int16_t b)
{
    int32_t s = (int32_t)a - (int32_t)b;
    if (s > 32767) s = 32767;
    if (s < -32768) s = -32768;
    return (int16_t)s;
}

static inline uint32_t signed_add_16_and_16(uint32_t a, uint32_t b)
{
    int16_t al = (int16_t)(a & 0xFFFF), ah = (int16_t)(a >> 16);
    int16_t bl = (int16_t)(b & 0xFFFF), bh = (int16_t)(b >> 16);
    al = sat_add16(al, bl);
    ah = sat_add16(ah, bh);
    return ((uint32_t)(uint16_t)al) | ((uint32_t)(uint16_t)ah << 16);
}

static inline int32_t signed_subtract_16_and_16(int32_t a, int32_t b)
{
    int16_t al = (int16_t)(a & 0xFFFF), ah = (int16_t)(a >> 16);
    int16_t bl = (int16_t)(b & 0xFFFF), bh = (int16_t)(b >> 16);
    al = sat_sub16(al, bl);
    ah = sat_sub16(ah, bh);
    return (int32_t)(((uint32_t)(uint16_t)al) | ((uint32_t)(uint16_t)ah << 16));
}

static inline int32_t signed_halving_add_16_and_16(int32_t a, int32_t b)
{
    int16_t al = (int16_t)(a & 0xFFFF), ah = (int16_t)(a >> 16);
    int16_t bl = (int16_t)(b & 0xFFFF), bh = (int16_t)(b >> 16);
    al = (int16_t)(((int32_t)al + bl) >> 1);
    ah = (int16_t)(((int32_t)ah + bh) >> 1);
    return (int32_t)(((uint32_t)(uint16_t)al) | ((uint32_t)(uint16_t)ah << 16));
}

static inline int32_t signed_halving_subtract_16_and_16(int32_t a, int32_t b)
{
    int16_t al = (int16_t)(a & 0xFFFF), ah = (int16_t)(a >> 16);
    int16_t bl = (int16_t)(b & 0xFFFF), bh = (int16_t)(b >> 16);
    al = (int16_t)(((int32_t)al - bl) >> 1);
    ah = (int16_t)(((int32_t)ah - bh) >> 1);
    return (int32_t)(((uint32_t)(uint16_t)al) | ((uint32_t)(uint16_t)ah << 16));
}

static inline int32_t signed_multiply_accumulate_32x16b(int32_t sum, int32_t a, uint32_t b)
{
    return sum + (int32_t)(((int64_t)a * (int16_t)(b & 0xFFFF)) >> 16);
}

static inline int32_t signed_multiply_accumulate_32x16t(int32_t sum, int32_t a, uint32_t b)
{
    return sum + (int32_t)(((int64_t)a * (int16_t)(b >> 16)) >> 16);
}

static inline uint32_t logical_and(uint32_t a, uint32_t b)
{
    return a & b;
}

static inline int32_t multiply_16tx16t_add_16bx16b(uint32_t a, uint32_t b)
{
    int32_t al = (int16_t)(a & 0xFFFF), ah = (int16_t)(a >> 16);
    int32_t bl = (int16_t)(b & 0xFFFF), bh = (int16_t)(b >> 16);
    return al * bl + ah * bh;
}

static inline int32_t multiply_16tx16b_add_16bx16t(uint32_t a, uint32_t b)
{
    int32_t al = (int16_t)(a & 0xFFFF), ah = (int16_t)(a >> 16);
    int32_t bl = (int16_t)(b & 0xFFFF), bh = (int16_t)(b >> 16);
    return al * bh + ah * bl;
}

static inline int64_t multiply_accumulate_16tx16t_add_16bx16b(int64_t sum, uint32_t a, uint32_t b)
{
    return sum + (int64_t)multiply_16tx16t_add_16bx16b(a, b);
}

static inline int64_t multiply_accumulate_16tx16b_add_16bx16t(int64_t sum, uint32_t a, uint32_t b)
{
    return sum + (int64_t)multiply_16tx16b_add_16bx16t(a, b);
}

static inline int32_t multiply_16bx16b(uint32_t a, uint32_t b)
{
    return (int32_t)((int16_t)(a & 0xFFFF)) * (int32_t)((int16_t)(b & 0xFFFF));
}

static inline int32_t multiply_16bx16t(uint32_t a, uint32_t b)
{
    return (int32_t)((int16_t)(a & 0xFFFF)) * (int32_t)((int16_t)(b >> 16));
}

static inline int32_t multiply_16tx16b(uint32_t a, uint32_t b)
{
    return (int32_t)((int16_t)(a >> 16)) * (int32_t)((int16_t)(b & 0xFFFF));
}

static inline int32_t multiply_16tx16t(uint32_t a, uint32_t b)
{
    return (int32_t)((int16_t)(a >> 16)) * (int32_t)((int16_t)(b >> 16));
}

static inline int32_t substract_32_saturate(uint32_t a, uint32_t b)
{
    int64_t d = (int64_t)(int32_t)a - (int64_t)(int32_t)b;
    if (d > 2147483647LL) d = 2147483647LL;
    if (d < -2147483648LL) d = -2147483648LL;
    return (int32_t)d;
}

static inline uint32_t get_q_psr(void) { return 0; }
static inline void clr_q_psr(void) {}

static inline int32_t multiply_32x32_rshift(int32_t x, int32_t y, int z)
{
    return (int32_t)(((int64_t)x * y) >> (31 - z));
}

#endif
