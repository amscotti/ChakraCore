//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt in the project root for license information.
//-------------------------------------------------------------------------------------------------------
#pragma once

#include <cstring>

namespace Js
{
    // Raw binary16 storage for Float16Array elements. The buffer holds the
    // 16-bit patterns; load widens with Float16::ToDouble and store narrows
    // with Float16::ToBits after ToNumber. A distinct struct (rather than a
    // uint16 alias) gives Float16Array its own TypedArray instantiation,
    // TypeId and conversion semantics. Trivially copyable so typed-array
    // memmove/sort fast paths can copy it bitwise.
    struct Float16Bits
    {
        uint16 bits;
        Float16Bits() : bits(0) {}
        Float16Bits(uint16 value) : bits(value) {}
        bool operator==(int rhs) const { return bits == (uint16)rhs; }
        bool operator==(const Float16Bits& rhs) const { return bits == rhs.bits; }
        explicit operator int() const { return bits; }
    };

    // Portable IEEE 754 binary16 conversion. Everything is done with integer
    // bit manipulation so the result does not depend on a compiler's __fp16
    // support or on the host floating point rounding mode.
    class Float16
    {
    public:
        static constexpr uint16 SignMask = 0x8000;
        static constexpr uint16 PositiveInfinity = 0x7C00;
        static constexpr uint16 NegativeInfinity = 0xFC00;
        static constexpr uint16 QuietNaN = 0x7E00;

        // Round a binary64 value to binary16 and return the 16-bit pattern.
        // Round-to-nearest, ties-to-even; NaN becomes a quiet NaN.
        static uint16 ToBits(double value)
        {
            uint64 bits;
            memcpy(&bits, &value, sizeof(bits));

            uint16 sign = (uint16)((bits >> 48) & SignMask);
            uint32 exponent = (uint32)((bits >> 52) & 0x7FF);
            uint64 fraction = bits & 0xFFFFFFFFFFFFFULL;

            if (exponent == 0x7FF)
            {
                return fraction == 0 ? (uint16)(sign | PositiveInfinity) : (uint16)(sign | QuietNaN);
            }

            if (exponent == 0)
            {
                // Zero, or a binary64 subnormal far below the binary16
                // underflow threshold.
                return sign;
            }

            int e = (int)exponent - 1023;

            if (e > 15)
            {
                return (uint16)(sign | PositiveInfinity);
            }

            if (e >= -14)
            {
                // Normal binary16: keep 10 fraction bits from binary64's 52.
                uint64 q = fraction >> 42;
                uint64 remainder = fraction & ((1ULL << 42) - 1);
                uint64 halfway = 1ULL << 41;
                if (remainder > halfway || (remainder == halfway && (q & 1)))
                {
                    q++;
                }
                if (q == 0x400)
                {
                    q = 0;
                    e++;
                    if (e > 15)
                    {
                        return (uint16)(sign | PositiveInfinity);
                    }
                }
                return (uint16)(sign | ((uint16)(e + 15) << 10) | (uint16)q);
            }

            if (e < -25)
            {
                return sign;
            }

            if (e == -25)
            {
                // Exactly 2^-25 is the tie between zero and the smallest
                // subnormal and rounds to even (zero).
                return fraction == 0 ? sign : (uint16)(sign | 1);
            }

            // Subnormal binary16: round (1.fraction) * 2^(e + 24) to an
            // integer number of 2^-24 units.
            uint32 shift = (uint32)(e + 24);
            uint32 right = 52 - shift;
            uint64 significand = 0x10000000000000ULL | fraction;
            uint64 q = significand >> right;
            uint64 remainder = significand & ((1ULL << right) - 1);
            uint64 halfway = 1ULL << (right - 1);
            if (remainder > halfway || (remainder == halfway && (q & 1)))
            {
                q++;
            }
            if (q == 0x400)
            {
                // Rounded up to the smallest normal.
                return (uint16)(sign | 0x0400);
            }
            return (uint16)(sign | q);
        }

        // Exact widening of a binary16 pattern to binary64. A NaN payload is
        // carried in the high fraction bits; callers observe an
        // implementation-defined NaN.
        static double ToDouble(uint16 value)
        {
            uint64 sign = (uint64)(value & SignMask) << 48;
            uint32 exponent = (value >> 10) & 0x1F;
            uint32 fraction = value & 0x3FF;
            uint64 bits;

            if (exponent == 0)
            {
                if (fraction == 0)
                {
                    bits = sign;
                }
                else
                {
                    uint32 highest = 9;
                    while ((fraction & (1U << highest)) == 0)
                    {
                        highest--;
                    }
                    int e = (int)highest - 24;
                    bits = sign |
                        ((uint64)(e + 1023) << 52) |
                        (((uint64)fraction << (52 - highest)) & 0xFFFFFFFFFFFFFULL);
                }
            }
            else if (exponent == 0x1F)
            {
                bits = sign | (0x7FFULL << 52) | ((uint64)fraction << 42);
            }
            else
            {
                bits = sign |
                    ((uint64)((int)exponent - 15 + 1023) << 52) |
                    ((uint64)fraction << 42);
            }

            double result;
            memcpy(&result, &bits, sizeof(result));
            return result;
        }

        // Math.f16round: ToNumber already applied by the caller.
        static double Round(double value)
        {
            return ToDouble(ToBits(value));
        }
    };
}
