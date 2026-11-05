/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Gerard Braad (Regroove | RGRV) */
// Copyright 2014 Emilie Gillet.
//
// Author: Emilie Gillet (emilie.o.gillet@gmail.com)
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//
// See http://creativecommons.org/licenses/MIT/ for more information.
//
// -----------------------------------------------------------------------------
//
// Adapted for Polyverb by Wojciech Jakóbczyk (jakobczyk.woj@gmail.com)

#ifndef POLYVERB_UTIL_H
#define POLYVERB_UTIL_H

#include <inttypes.h>
#include <stddef.h>

// for STATIC_ASSERT
#pragma GCC diagnostic ignored "-Wunused-local-typedefs"

// Host copy of core/libs/Audio/Polyverb/util.h.  The only hardware-specific
// line is Clip16, which uses the Cortex-M SSAT instruction in the firmware.
// This file is force-included for Polyverb translation units; its guard is
// the original's, so the vendor header is skipped.
inline int32_t Clip16(int32_t x)
{
    if (x > 32767)
        return 32767;
    if (x < -32768)
        return -32768;
    return x;
}

#define MAKE_INTEGRAL_FRACTIONAL(x)                                                                \
    int32_t x##_integral = static_cast<int32_t>(x);                                                \
    float x##_fractional = x - static_cast<float>(x##_integral);

#define DISALLOW_COPY_AND_ASSIGN(TypeName)                                                         \
    TypeName(const TypeName&);                                                                     \
    void operator=(const TypeName&)

#define JOIN(lhs, rhs)   JOIN_1(lhs, rhs)
#define JOIN_1(lhs, rhs) JOIN_2(lhs, rhs)
#define JOIN_2(lhs, rhs) lhs##rhs

#define STATIC_ASSERT(expression, message)                                                         \
    struct JOIN(__static_assertion_at_line_, __LINE__)                                             \
    {                                                                                              \
        impl::StaticAssertion<static_cast<bool>((expression))> JOIN(                               \
          JOIN(JOIN(STATIC_ASSERTION_FAILED_AT_LINE_, __LINE__), _),                               \
          message);                                                                                \
    };                                                                                             \
    typedef impl::StaticAssertionTest<sizeof(JOIN(__static_assertion_at_line_, __LINE__))> JOIN(   \
      __static_assertion_test_at_line_, __LINE__)

namespace impl
{

template<bool>
struct StaticAssertion;

template<>
struct StaticAssertion<true>
{
}; // StaticAssertion<true>

template<int i>
struct StaticAssertionTest
{
}; // StaticAssertionTest<int>

} // namespace impl

#endif