//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#include "RuntimeLibraryPch.h"

#include <cmath>
#include <limits>

namespace Js
{
    namespace
    {
        // ToIndex upper bound: 2**53 - 1, exactly representable as a double.
        const double BigIntMaxIndex = 9007199254740991.0;

        // Resource bound for results that must materialize 2**bits (a
        // negative asUintN operand with bits above the operand width). 2**24
        // limbs is 128 MiB on 64-bit limbs and 64 MiB on 32-bit limbs, far
        // beyond the corpus; V8 likewise rejects oversized results with
        // RangeError rather than attempting an unbounded allocation.
        const uint64 BigIntMaxResultLimbs = 1ull << 24;

        inline uint32 BigIntLimbBits()
        {
            return static_cast<uint32>(sizeof(digit_t) * 8);
        }

        // Exponent results are built by repeated schoolbook squaring, so the
        // limb-count bound used by asUintN (2**24 limbs) is far beyond any
        // answer this implementation can produce in reasonable time. Keep a
        // smaller, explicit resource bound: 2**20 bits is 128 KiB and makes a
        // hostile `2n ** (2n**64n - 1n)` a controlled RangeError instead of an
        // unbounded allocation. V8 uses a larger maximum (about 2**30 bits);
        // the observable contract for oversized results is the same.
        const uint64 BigIntMaxExponentResultBits = 1ull << 20;

        inline uint64 BigIntMaxResultBitCount()
        {
            return BigIntMaxResultLimbs * BigIntLimbBits();
        }

        // Number of uint32 halves in one digit_t limb.
        inline uint32 BigIntUnitsPerLimb()
        {
            return static_cast<uint32>(sizeof(digit_t) / sizeof(uint32));
        }

        inline digit_t BigIntLimbMask()
        {
            return static_cast<digit_t>(~static_cast<digit_t>(0));
        }

        inline digit_t BigIntTopLimbMask(uint32 topBits)
        {
            return topBits == 0
                ? BigIntLimbMask()
                : static_cast<digit_t>((static_cast<digit_t>(1) << topBits) - 1);
        }
    }

    JavascriptBigInt * JavascriptBigInt::Create(const char16 * content, charcount_t cchUseLength, bool isNegative, ScriptContext * scriptContext)
    {
        return RecyclerNew(scriptContext->GetRecycler(), JavascriptBigInt, content, cchUseLength, isNegative, scriptContext->GetLibrary()->GetBigIntTypeStatic());
    }

    JavascriptBigInt * JavascriptBigInt::CreateFromNumber(double value, ScriptContext * scriptContext)
    {
        // NumberToBigInt: NaN, +/-Infinity and non-integral values have no
        // BigInt representation and throw (RangeError); the caller checks.
        if (JavascriptNumber::IsNan(value) || JavascriptNumber::IsPosInf(value) ||
            JavascriptNumber::IsNegInf(value) || value != floor(value))
        {
            return nullptr;
        }

        // Integral doubles are exact multiples of a power of two, so splitting
        // the magnitude into half-limb chunks is lossless. A digit_t is
        // uintptr_t: 64-bit on the main builds, 32-bit on x86/ARM Windows.
        // Feed the chunks from the most significant one down; MulThenAdd
        // scales the existing multi-digit value by the chunk base and adds
        // the chunk. This is NumberToBigInt for every finite integral double
        // the BigInt representation supports.
        bool isNegative = value < 0;
        double remaining = isNegative ? -value : value;
        if (remaining == 0)
        {
            return JavascriptBigInt::CreateZero(scriptContext); // includes -0 -> 0n
        }

        const uint32 chunkBits = sizeof(digit_t) * 4;
        const double chunkBase = std::ldexp(1.0, static_cast<int>(chunkBits));
        uint32 chunks[1024 / (sizeof(digit_t) * 4) + 2]; // 2^1024 / chunkBase + 1
        uint32 chunkCount = 0;
        while (remaining >= 1.0)
        {
            Assert(chunkCount < sizeof(chunks) / sizeof(chunks[0]));
            double high = floor(remaining / chunkBase);
            chunks[chunkCount++] = static_cast<uint32>(remaining - high * chunkBase);
            remaining = high;
        }

        JavascriptBigInt * bigintNew = JavascriptBigInt::CreateZero(scriptContext);
        for (uint32 i = chunkCount; i > 0; i--)
        {
            bigintNew->MulThenAdd(static_cast<digit_t>(chunkBase), chunks[i - 1]);
        }
        bigintNew->m_isNegative = isNegative;
        return bigintNew;
    }

    namespace
    {
        // StrWhiteSpace from the StringNumericLiteral grammar (used by StringToBigInt).
        bool IsBigIntWhiteSpace(char16 character)
        {
            return character == 0x0020
                || (character >= 0x0009 && character <= 0x000D)
                || character == 0x00A0
                || character == 0x1680
                || (character >= 0x2000 && character <= 0x200A)
                || character == 0x2028
                || character == 0x2029
                || character == 0x202F
                || character == 0x205F
                || character == 0x3000
                || character == 0xFEFF;
        }

        uint32 BigIntDigitValue(char16 character, uint32 radix)
        {
            uint32 digitValue;
            if (character >= _u('0') && character <= _u('9'))
            {
                digitValue = character - _u('0');
            }
            else if (character >= _u('a') && character <= _u('f'))
            {
                digitValue = character - _u('a') + 10;
            }
            else if (character >= _u('A') && character <= _u('F'))
            {
                digitValue = character - _u('A') + 10;
            }
            else
            {
                return static_cast<uint32>(-1);
            }
            return digitValue < radix ? digitValue : static_cast<uint32>(-1);
        }
    }

    JavascriptBigInt * JavascriptBigInt::CreateFromString(const char16 * content, charcount_t cchContent, ScriptContext * scriptContext)
    {
        Assert(content != nullptr);

        const char16 * pch = content;
        const char16 * pchEnd = content + cchContent;
        while (pch < pchEnd && IsBigIntWhiteSpace(*pch)) pch++;
        while (pchEnd > pch && IsBigIntWhiteSpace(pchEnd[-1])) pchEnd--;
        if (pch == pchEnd)
        {
            return JavascriptBigInt::CreateZero(scriptContext); // empty/blank string is 0n
        }

        bool hasSign = (*pch == _u('-') || *pch == _u('+'));
        bool isNegative = (*pch == _u('-'));
        if (hasSign) pch++;

        // StringToBigInt grammar: the 0x/0o/0b prefixed forms are unsigned
        // only, so a sign in front of a prefix (e.g. "-0x1") is a syntax error.
        uint32 radix = 10;
        if (pchEnd - pch >= 2 && pch[0] == _u('0'))
        {
            char16 radixChar = pch[1];
            if (radixChar == _u('x') || radixChar == _u('X')) { if (hasSign) return nullptr; radix = 16; pch += 2; }
            else if (radixChar == _u('o') || radixChar == _u('O')) { if (hasSign) return nullptr; radix = 8; pch += 2; }
            else if (radixChar == _u('b') || radixChar == _u('B')) { if (hasSign) return nullptr; radix = 2; pch += 2; }
        }
        if (pch == pchEnd)
        {
            return nullptr; // a lone sign or a bare "0x"/"0o"/"0b" prefix
        }

        JavascriptBigInt * bigintNew = JavascriptBigInt::CreateZero(scriptContext);
        for (const char16 * pchDigit = pch; pchDigit < pchEnd; pchDigit++)
        {
            uint32 digitValue = BigIntDigitValue(*pchDigit, radix);
            if (digitValue == static_cast<uint32>(-1))
            {
                return nullptr;
            }
            bigintNew->MulThenAdd(radix, digitValue);
        }

        // Canonical form: no high zero digits, no negative zero.
        while (bigintNew->m_length > 1 && bigintNew->m_digits[bigintNew->m_length - 1] == 0)
        {
            bigintNew->m_length--;
        }
        bigintNew->m_isNegative = isNegative && !(bigintNew->m_length == 1 && bigintNew->m_digits[0] == 0);
        return bigintNew;
    }

    JavascriptBigInt * JavascriptBigInt::CreateZero(ScriptContext * scriptContext)
    {
        return JavascriptBigInt::CreateZeroWithLength(1, scriptContext);
    }

    JavascriptBigInt * JavascriptBigInt::CreateZeroWithLength(digit_t length, ScriptContext * scriptContext)
    {
        JavascriptBigInt * bigintNew = RecyclerNew(scriptContext->GetRecycler(), JavascriptBigInt, scriptContext->GetLibrary()->GetBigIntTypeStatic());
        bigintNew->m_length = length;
        bigintNew->m_isNegative = false;
        bigintNew->m_maxLength = length;
        bigintNew->m_digits = RecyclerNewArrayLeafZ(scriptContext->GetRecycler(), digit_t, bigintNew->m_length);
        return bigintNew;
    }

    JavascriptBigInt * JavascriptBigInt::CreateOne(ScriptContext * scriptContext)
    {
        JavascriptBigInt * bigintNew = JavascriptBigInt::CreateZero(scriptContext);
        bigintNew->m_digits[0] = 1;
        return bigintNew;
    }

    RecyclableObject* JavascriptBigInt::ToObject(ScriptContext * requestContext)
    {
        return requestContext->GetLibrary()->CreateBigIntObject(this);
    }

    Var JavascriptBigInt::GetTypeOfString(ScriptContext * requestContext)
    {
        return requestContext->GetLibrary()->GetBigIntTypeDisplayString();
    }

    // Divide the positive magnitude in `digits` by a small radix in place and
    // return the remainder. The divisor is at most 36, so a half-limb at a
    // time keeps every intermediate inside uint64 without 128-bit math. The
    // half-limb width follows digit_t (32 bits for 64-bit limbs, 16 bits for
    // 32-bit limbs), unlike a hard-coded 64-bit assumption.
    uint32 JavascriptBigInt::DivModInPlace(digit_t * digits, digit_t& length, uint32 divisor)
    {
        Assert(divisor >= 2 && divisor <= 36);
        Assert(length > 0);

        const uint32 halfBits = sizeof(digit_t) * 4;
        const digit_t halfMask = (static_cast<digit_t>(1) << halfBits) - 1;

        uint64 remainder = 0;
        for (digit_t i = length; i > 0; i--)
        {
            uint64 current = (remainder << halfBits) |
                (static_cast<uint64>(digits[i - 1]) >> halfBits);
            uint64 quotientHigh = current / divisor;
            remainder = current % divisor;

            current = (remainder << halfBits) | (digits[i - 1] & halfMask);
            uint64 quotientLow = current / divisor;
            remainder = current % divisor;

            digits[i - 1] = static_cast<digit_t>((quotientHigh << halfBits) | quotientLow);
        }

        // Drop high zero limbs; a zero quotient becomes length 0, which ends
        // the caller's digit loop.
        while (length > 0 && digits[length - 1] == 0)
        {
            length--;
        }
        return static_cast<uint32>(remainder);
    }

    JavascriptString* JavascriptBigInt::ToString(uint32 radix, ScriptContext * scriptContext)
    {
        Assert(radix >= 2 && radix <= 36);
        if (IsZeroValue())
        {
            return scriptContext->GetLibrary()->CreateStringFromCppLiteral(_u("0"));
        }

        static const char16 digitChars[] = _u("0123456789abcdefghijklmnopqrstuvwxyz");

        // Base 2 is the longest representation: one character per bit. Check
        // the representation size before allocating (a string past the engine
        // limit cannot be produced anyway).
        size_t maxDigitsSize = static_cast<size_t>(m_length) * (sizeof(digit_t) * 8) + 2;
        if (!IsValidCharCount(maxDigitsSize))
        {
            JavascriptError::ThrowRangeError(scriptContext, JSERR_FunctionArgument_Invalid, _u("BigInt.prototype.toString"));
        }
        charcount_t maxDigits = static_cast<charcount_t>(maxDigitsSize);

        digit_t length = m_length;
        digit_t * digits = RecyclerNewArrayLeaf(scriptContext->GetRecycler(), digit_t, length);
        js_memcpy_s(digits, length * sizeof(digit_t), m_digits, length * sizeof(digit_t));

        // DivModInPlace yields the least-significant digit first.
        char16 * reversedDigits = RecyclerNewArrayLeaf(scriptContext->GetRecycler(), char16, maxDigits);
        charcount_t digitCount = 0;
        while (length > 0)
        {
            uint32 remainder = JavascriptBigInt::DivModInPlace(digits, length, radix);
            reversedDigits[digitCount++] = digitChars[remainder];
        }
        Assert(digitCount <= maxDigits);

        charcount_t resultLength = digitCount + (m_isNegative ? 1 : 0);
        char16 * result = RecyclerNewArrayLeaf(scriptContext->GetRecycler(), char16, resultLength);

        charcount_t index = 0;
        if (m_isNegative)
        {
            result[index++] = _u('-');
        }
        for (charcount_t i = 0; i < digitCount; i++)
        {
            result[index++] = reversedDigits[digitCount - 1 - i];
        }

        return JavascriptString::NewCopyBuffer(result, resultLength, scriptContext);
    }

    JavascriptString* JavascriptBigInt::ToString(ScriptContext * scriptContext)
    {
        return ToString(10, scriptContext);
    }

    double JavascriptBigInt::ToDouble() const
    {
        if (IsZeroValue())
        {
            return 0.0;
        }

        // BigInt-to-Number must be correctly rounded. The magnitude is a
        // little-endian sequence of digit_t limbs (64-bit on the main builds,
        // 32-bit on x86/ARM Windows); find its bit length, take the top 53
        // bits, and round on the guard bit using round-to-nearest,
        // ties-to-even. The result is significand * 2^dropBits, which is
        // exact in binary once the 53-bit significand is chosen.
        const uint32 limbBits = sizeof(digit_t) * 8;
        const double limbBase = std::ldexp(1.0, static_cast<int>(limbBits));
        digit_t top = m_digits[m_length - 1];
        uint64 bitLength = (static_cast<uint64>(m_length) - 1) * limbBits;
        while (top != 0)
        {
            bitLength++;
            top >>= 1;
        }

        if (bitLength <= 53)
        {
            // Exact: the whole magnitude fits in the 53-bit precision.
            double value = 0;
            for (digit_t i = m_length; i > 0; i--)
            {
                value = value * limbBase + static_cast<double>(m_digits[i - 1]);
            }
            return m_isNegative ? -value : value;
        }

        uint64 dropBits = bitLength - 53;
        if (dropBits > 971)
        {
            // The smallest value with this exponent already exceeds the
            // largest finite double (2^1024 - 2^971).
            double infinity = std::numeric_limits<double>::infinity();
            return m_isNegative ? -infinity : infinity;
        }

        uint64 wordIndex = dropBits / limbBits;
        uint32 bitOffset = static_cast<uint32>(dropBits % limbBits);
        uint64 significand = 0;
        uint32 collectedBits = 0;
        for (uint64 i = wordIndex; i < m_length && collectedBits < 53; i++)
        {
            uint32 offset = (i == wordIndex) ? bitOffset : 0;
            uint64 part = static_cast<uint64>(m_digits[i]) >> offset;
            uint32 partBits = limbBits - offset;
            if (partBits > 53 - collectedBits)
            {
                partBits = 53 - collectedBits;
            }
            part &= (static_cast<uint64>(1) << partBits) - 1;
            significand |= part << collectedBits;
            collectedBits += partBits;
        }
        Assert(collectedBits == 53);
        Assert(significand < (static_cast<uint64>(1) << 53));

        uint64 guardIndex = (dropBits - 1) / limbBits;
        uint32 guardBit = static_cast<uint32>((dropBits - 1) % limbBits);
        bool guard = ((m_digits[guardIndex] >> guardBit) & 1) != 0;
        bool sticky = false;
        for (uint64 i = 0; i < guardIndex; i++)
        {
            if (m_digits[i] != 0)
            {
                sticky = true;
                break;
            }
        }
        if (guardBit != 0)
        {
            digit_t lowerMask = (static_cast<digit_t>(1) << guardBit) - 1;
            sticky = sticky || (m_digits[guardIndex] & lowerMask) != 0;
        }

        if (guard && (sticky || (significand & 1)))
        {
            significand++;
            if (significand == (static_cast<uint64>(1) << 53))
            {
                significand >>= 1;
                dropBits++;
                if (dropBits > 971)
                {
                    double infinity = std::numeric_limits<double>::infinity();
                    return m_isNegative ? -infinity : infinity;
                }
            }
        }

        double value = std::ldexp(static_cast<double>(significand), static_cast<int>(dropBits));
        return m_isNegative ? -value : value;
    }

    uint64 JavascriptBigInt::ToIndex(Var value, ScriptContext * scriptContext)
    {
        // ToIndex: ToIntegerOrInfinity(ToNumber(value)); reject negative and
        // values above 2**53 - 1. ToNumber performs ToPrimitive exactly once
        // and already throws TypeError for BigInt/Symbol primitives (including
        // wrappers and @@toPrimitive results).
        double number = JavascriptConversion::ToNumber(value, scriptContext);
        double integer = JavascriptConversion::ToInteger(number);
        if (!(integer >= 0) || integer > BigIntMaxIndex)
        {
            JavascriptError::ThrowRangeError(scriptContext, JSERR_FunctionArgument_Invalid, _u("BigInt.asIntN/asUintN"));
        }
        return static_cast<uint64>(integer);
    }

    uint64 JavascriptBigInt::GetMagnitudeBitLength() const
    {
        digit_t length = m_length;
        while (length > 0 && m_digits[length - 1] == 0)
        {
            length--;
        }
        if (length == 0)
        {
            return 0;
        }

        uint64 bitLength = (static_cast<uint64>(length) - 1) * BigIntLimbBits();
        digit_t top = m_digits[length - 1];
        while (top != 0)
        {
            bitLength++;
            top >>= 1;
        }
        return bitLength;
    }

    bool JavascriptBigInt::GetMagnitudeBit(uint64 bit) const
    {
        uint64 index = bit / BigIntLimbBits();
        if (index >= m_length)
        {
            return false;
        }
        return ((m_digits[index] >> (bit % BigIntLimbBits())) & 1) != 0;
    }

    bool JavascriptBigInt::HasSingleMagnitudeBit(uint64 bit) const
    {
        uint64 index = bit / BigIntLimbBits();
        if (index >= m_length || m_digits[index] != (static_cast<digit_t>(1) << (bit % BigIntLimbBits())))
        {
            return false;
        }
        for (digit_t i = 0; i < index; i++)
        {
            if (m_digits[i] != 0)
            {
                return false;
            }
        }
        for (digit_t i = static_cast<digit_t>(index) + 1; i < m_length; i++)
        {
            if (m_digits[i] != 0)
            {
                return false;
            }
        }
        return true;
    }

    JavascriptBigInt * JavascriptBigInt::TruncateMagnitude(uint64 bits, ScriptContext * scriptContext) const
    {
        Assert(bits > 0);
        const uint32 limbBits = BigIntLimbBits();

        digit_t length = m_length;
        while (length > 1 && m_digits[length - 1] == 0)
        {
            length--;
        }

        uint64 keep64 = (bits - 1) / limbBits + 1;
        Assert(keep64 <= length);
        digit_t keep = static_cast<digit_t>(keep64);
        JavascriptBigInt * result = CreateZeroWithLength(keep, scriptContext);
        digit_t copyLength = length < keep ? length : keep;
        js_memcpy_s(result->m_digits, keep * sizeof(digit_t), m_digits, copyLength * sizeof(digit_t));

        uint32 topBits = static_cast<uint32>(bits % limbBits);
        if (topBits != 0)
        {
            result->m_digits[keep - 1] &= BigIntTopLimbMask(topBits);
        }
        while (result->m_length > 1 && result->m_digits[result->m_length - 1] == 0)
        {
            result->m_length--;
        }
        if (result->IsZeroValue())
        {
            result->m_length = 1;
            result->m_digits[0] = 0;
        }
        return result;
    }

    JavascriptBigInt * JavascriptBigInt::TwosComplement(const JavascriptBigInt * magnitude, uint64 bits, ScriptContext * scriptContext)
    {
        Assert(bits > 0);
        Assert(magnitude != nullptr && !magnitude->IsZeroValue());
        Assert(magnitude->GetMagnitudeBitLength() <= bits);

        const uint32 limbBits = BigIntLimbBits();
        uint64 keep64 = (bits - 1) / limbBits + 1;
        if (keep64 >= BigIntMaxResultLimbs ||
            keep64 > static_cast<uint64>(BigIntLimbMask()) ||
            keep64 > SIZE_MAX / sizeof(digit_t))
        {
            // The requested result cannot be represented by this engine's
            // bounded BigInt digit array (see BigIntMaxResultLimbs).
            JavascriptError::ThrowRangeError(scriptContext, JSERR_FunctionArgument_Invalid, _u("BigInt.asIntN/asUintN"));
        }
        digit_t keep = static_cast<digit_t>(keep64);
        JavascriptBigInt * result = CreateZeroWithLength(keep, scriptContext);
        digit_t topMask = BigIntTopLimbMask(static_cast<uint32>(bits % limbBits));
        digit_t carry = 1;

        for (digit_t i = 0; i < keep; i++)
        {
            digit_t source = i < magnitude->m_length ? magnitude->m_digits[i] : 0;
            digit_t digit = static_cast<digit_t>(~source);
            if (i == keep - 1)
            {
                digit &= topMask;
            }
            digit_t newCarry = 0;
            digit = AddDigit(digit, carry, &newCarry);
            result->m_digits[i] = digit;
            carry = newCarry;
        }
        // The discarded carry is 2**bits; magnitude is non-zero and below
        // 2**bits, so the exact result fits and no carry may escape.
        Assert(carry == 0);

        while (result->m_length > 1 && result->m_digits[result->m_length - 1] == 0)
        {
            result->m_length--;
        }
        Assert(!result->IsZeroValue());
        return result;
    }

    void JavascriptBigInt::MakeNegative(JavascriptBigInt * value)
    {
        if (!value->IsZeroValue())
        {
            value->m_isNegative = true;
        }
    }

    JavascriptBigInt * JavascriptBigInt::AsUintN(JavascriptBigInt * value, uint64 bits, ScriptContext * scriptContext)
    {
        Assert(value != nullptr);
        if (bits == 0)
        {
            return CreateZero(scriptContext);
        }

        uint64 bitLength = value->GetMagnitudeBitLength();
        if (!value->m_isNegative)
        {
            // Positive values already below 2**bits are unchanged. This also
            // keeps huge `bits` from allocating when the result is just the
            // input (e.g. asUintN(2**53-1, 1n)).
            return bitLength <= bits ? value : value->TruncateMagnitude(bits, scriptContext);
        }

        if (bitLength == 0)
        {
            return CreateZero(scriptContext);
        }
        if (bitLength <= bits)
        {
            // 2**bits - |value|, the unsigned two's-complement pattern.
            return TwosComplement(value, bits, scriptContext);
        }

        JavascriptBigInt * magnitude = value->TruncateMagnitude(bits, scriptContext);
        return magnitude->IsZeroValue()
            ? magnitude
            : TwosComplement(magnitude, bits, scriptContext);
    }

    JavascriptBigInt * JavascriptBigInt::AsIntN(JavascriptBigInt * value, uint64 bits, ScriptContext * scriptContext)
    {
        Assert(value != nullptr);
        if (bits == 0)
        {
            return CreateZero(scriptContext);
        }

        uint64 bitLength = value->GetMagnitudeBitLength();
        if (!value->m_isNegative)
        {
            if (bitLength < bits)
            {
                // Fits including the sign bit (this also covers 0n).
                return value;
            }
            if (bitLength == bits)
            {
                // The highest bit is the sign bit: value - 2**bits.
                JavascriptBigInt * result = TwosComplement(value, bits, scriptContext);
                MakeNegative(result);
                return result;
            }

            JavascriptBigInt * magnitude = value->TruncateMagnitude(bits, scriptContext);
            if (magnitude->IsZeroValue() || !magnitude->GetMagnitudeBit(bits - 1))
            {
                return magnitude;
            }
            JavascriptBigInt * result = TwosComplement(magnitude, bits, scriptContext);
            MakeNegative(result);
            return result;
        }

        if (bitLength < bits)
        {
            // |value| < 2**(bits-1), so the signed result is value itself.
            return value;
        }
        if (bitLength == bits)
        {
            // Sign bit of the two's-complement pattern is set. Exactly
            // -2**(bits-1) is its own result; any larger magnitude wraps to
            // a positive value.
            return value->HasSingleMagnitudeBit(bits - 1)
                ? value
                : TwosComplement(value, bits, scriptContext);
        }

        JavascriptBigInt * magnitude = value->TruncateMagnitude(bits, scriptContext);
        if (magnitude->IsZeroValue())
        {
            return magnitude;
        }

        uint64 truncatedBits = magnitude->GetMagnitudeBitLength();
        if (truncatedBits < bits)
        {
            // |value| mod 2**bits is below 2**(bits-1): negative result.
            MakeNegative(magnitude);
            return magnitude;
        }
        Assert(truncatedBits == bits);
        if (magnitude->HasSingleMagnitudeBit(bits - 1))
        {
            // Exactly -2**(bits-1).
            MakeNegative(magnitude);
            return magnitude;
        }
        // |value| mod 2**bits exceeds 2**(bits-1): positive wrapped value.
        return TwosComplement(magnitude, bits, scriptContext);
    }

    Var JavascriptBigInt::EntryAsIntN(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();

        AssertMsg(args.HasArg(), "Should always have implicit 'this'");
        uint64 bits = ToIndex(
            args.Info.Count > 1 ? args[1] : scriptContext->GetLibrary()->GetUndefined(),
            scriptContext);
        JavascriptBigInt * bigint = JavascriptConversion::ToBigInt(
            args.Info.Count > 2 ? args[2] : scriptContext->GetLibrary()->GetUndefined(),
            scriptContext);
        return AsIntN(bigint, bits, scriptContext);
    }

    Var JavascriptBigInt::EntryAsUintN(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();

        AssertMsg(args.HasArg(), "Should always have implicit 'this'");
        uint64 bits = ToIndex(
            args.Info.Count > 1 ? args[1] : scriptContext->GetLibrary()->GetUndefined(),
            scriptContext);
        JavascriptBigInt * bigint = JavascriptConversion::ToBigInt(
            args.Info.Count > 2 ? args[2] : scriptContext->GetLibrary()->GetUndefined(),
            scriptContext);
        return AsUintN(bigint, bits, scriptContext);
    }

    JavascriptBigInt * JavascriptBigInt::ThisBigIntValue(Var value, ScriptContext * scriptContext)
    {
        TypeId typeId = JavascriptOperators::GetTypeId(value);
        if (typeId == TypeIds_BigInt)
        {
            return UnsafeVarTo<JavascriptBigInt>(value);
        }
        if (typeId == TypeIds_BigIntObject)
        {
            JavascriptBigIntObject * wrapper = UnsafeVarTo<JavascriptBigIntObject>(value);
            JavascriptBigInt * bigint = wrapper->GetValue();
            if (bigint != nullptr)
            {
                // The wrapper may belong to another realm; return the
                // primitive in the caller's script context, as the Symbol
                // and Number wrapper paths do.
                return UnsafeVarTo<JavascriptBigInt>(
                    CrossSite::MarshalVar(scriptContext, bigint, wrapper->GetScriptContext()));
            }
        }

        JavascriptError::ThrowTypeError(scriptContext, JSERR_This_NeedObject, _u("BigInt"));
        return nullptr;
    }

    Var JavascriptBigInt::EntryValueOf(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();

        AssertMsg(args.HasArg(), "Should always have implicit 'this'");
        return ThisBigIntValue(args[0], scriptContext);
    }

    Var JavascriptBigInt::EntryToString(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();

        AssertMsg(args.HasArg(), "Should always have implicit 'this'");
        JavascriptBigInt * bigint = ThisBigIntValue(args[0], scriptContext);

        uint32 radix = 10;
        if (args.Info.Count > 1 && JavascriptOperators::GetTypeId(args[1]) != TypeIds_Undefined)
        {
            double radixNumber = JavascriptConversion::ToInteger(args[1], scriptContext);
            if (!(radixNumber >= 2 && radixNumber <= 36))
            {
                JavascriptError::ThrowRangeError(scriptContext, JSERR_FunctionArgument_Invalid, _u("BigInt.prototype.toString"));
            }
            radix = static_cast<uint32>(radixNumber);
        }

        return bigint->ToString(radix, scriptContext);
    }

    Var JavascriptBigInt::EntryToLocaleString(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();

        AssertMsg(args.HasArg(), "Should always have implicit 'this'");
        return ThisBigIntValue(args[0], scriptContext)->ToString(scriptContext);
    }

    JavascriptBigInt * JavascriptBigInt::New(JavascriptBigInt * pbi, ScriptContext * scriptContext)
    {
        Assert(pbi->m_maxLength >= pbi->m_length);
        JavascriptBigInt * bigintNew = RecyclerNew(scriptContext->GetRecycler(), JavascriptBigInt, scriptContext->GetLibrary()->GetBigIntTypeStatic());
        bigintNew->m_length = pbi->m_length;
        bigintNew->m_maxLength = pbi->m_maxLength;
        bigintNew->m_isNegative = pbi->m_isNegative;
        bigintNew->m_digits = RecyclerNewArrayLeaf(scriptContext->GetRecycler(), digit_t, pbi->m_maxLength);
        js_memcpy_s(bigintNew->m_digits, bigintNew->m_length * sizeof(digit_t), pbi->m_digits, bigintNew->m_length * sizeof(digit_t));
 
        return bigintNew;
    }

    RecyclableObject * JavascriptBigInt::CloneToScriptContext(ScriptContext* requestContext)
    {
        return JavascriptBigInt::New(this, requestContext);
    }

    Var JavascriptBigInt::NewInstance(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();

        AssertMsg(args.HasArg(), "Should always have implicit 'this'");

        // SkipDefaultNewObject function flag should have prevented the default object from
        // being created, except when call true a host dispatch.
        Var newTarget = args.GetNewTarget();
        bool isCtorSuperCall = JavascriptOperators::GetAndAssertIsConstructorSuperCall(args);

        if (callInfo.Flags & CallFlags_New)
        {
            // BigInt is not a constructor (spec sec-bigint-constructor: "If
            // NewTarget is not undefined, throw a TypeError exception"). The
            // check runs before any argument coercion, so a throwing
            // valueOf/toString on the argument is never observed.
            JavascriptError::ThrowTypeError(scriptContext, VBSERR_TypeMismatch, _u("BigInt construct"));
        }

        // BigInt(value): ToPrimitive(hint Number), then NumberToBigInt when
        // that yields a Number; otherwise ToBigInt, which deliberately
        // rejects Number (spec ToBigInt). BigInt() with no argument is
        // ToPrimitive(undefined)/ToBigInt(undefined): a TypeError, not a host
        // abort. BigInt({ valueOf() { return 42; } }) still takes the
        // NumberToBigInt path.
        Var value = args.Info.Count > 1 ? args[1] : scriptContext->GetLibrary()->GetUndefined();
        Var primitive = JavascriptConversion::ToPrimitive<JavascriptHint::HintNumber>(value, scriptContext);
        JavascriptBigInt * result;
        TypeId primitiveType = JavascriptOperators::GetTypeId(primitive);
        if (primitiveType == TypeIds_Integer || primitiveType == TypeIds_Number ||
            primitiveType == TypeIds_Int64Number || primitiveType == TypeIds_UInt64Number)
        {
            result = JavascriptBigInt::CreateFromNumber(
                JavascriptConversion::ToNumber(primitive, scriptContext), scriptContext);
            if (result == nullptr)
            {
                // NumberToBigInt: NaN, +/-Infinity and non-integral values
                // have no BigInt representation.
                JavascriptError::ThrowRangeError(scriptContext, VBSERR_TypeMismatch, _u("NumberToBigInt"));
            }
        }
        else
        {
            result = JavascriptConversion::ToBigInt(primitive, scriptContext);
        }

        return isCtorSuperCall ?
            JavascriptOperators::OrdinaryCreateFromConstructor(VarTo<RecyclableObject>(newTarget), result, nullptr, scriptContext) :
            result;
    }

    BOOL JavascriptBigInt::Equals(Var other, BOOL* value, ScriptContext * requestContext)
    {
        return JavascriptBigInt::Equals(this, other, value, requestContext);
    }

    BOOL JavascriptBigInt::Equals(JavascriptBigInt* left, Var right, BOOL* value, ScriptContext * requestContext)
    {
        *value = FALSE;
        TypeId rightType = JavascriptOperators::GetTypeId(right);
        switch (rightType)
        {
        case TypeIds_BigInt:
            *value = JavascriptBigInt::Equals(left, right) ? TRUE : FALSE;
            break;
        case TypeIds_Integer:
            {
                int32 integer = TaggedInt::ToInt32(right);
                bool isNegative = integer < 0;
                uint64 magnitude = isNegative
                    ? static_cast<uint64>(-static_cast<int64>(integer))
                    : static_cast<uint64>(integer);
                *value = left->CompareToUInt64(isNegative, magnitude) == 0 ? TRUE : FALSE;
            }
            break;
        case TypeIds_Number:
            {
                // BigInt == Number: true only for a finite integral Number
                // with the same mathematical value. NaN, +/-Infinity and
                // fractional values are unequal; comparison is exact for
                // magnitudes beyond 2**53.
                int comparison;
                if (left->CompareToDouble(JavascriptNumber::GetValue(right), &comparison))
                {
                    *value = comparison == 0 ? TRUE : FALSE;
                }
            }
            break;
        case TypeIds_Int64Number:
            {
                __int64 integer = UnsafeVarTo<JavascriptInt64Number>(right)->GetValue();
                bool isNegative = integer < 0;
                uint64 magnitude = isNegative
                    ? static_cast<uint64>(-(integer + 1)) + 1
                    : static_cast<uint64>(integer);
                *value = left->CompareToUInt64(isNegative, magnitude) == 0 ? TRUE : FALSE;
            }
            break;
        case TypeIds_UInt64Number:
            *value = left->CompareToUInt64(false, UnsafeVarTo<JavascriptUInt64Number>(right)->GetValue()) == 0 ? TRUE : FALSE;
            break;
        case TypeIds_String:
            {
                // BigInt == String uses StringToBigInt; a string outside the
                // grammar is simply unequal (not a SyntaxError).
                JavascriptString* string = UnsafeVarTo<JavascriptString>(right);
                JavascriptBigInt* parsed = JavascriptBigInt::CreateFromString(
                    string->GetString(), string->GetLength(), requestContext);
                *value = (parsed != nullptr && left->Compare(parsed) == 0) ? TRUE : FALSE;
            }
            break;
        case TypeIds_Boolean:
            *value = left->CompareToUInt64(false, VarTo<JavascriptBoolean>(right)->GetValue() ? 1 : 0) == 0 ? TRUE : FALSE;
            break;
        case TypeIds_Null:
        case TypeIds_Undefined:
        case TypeIds_Symbol:
            // Null/undefined only equal each other; Symbol primitives never
            // equal a BigInt.
            break;
        default:
            if (JavascriptOperators::IsObjectType(rightType))
            {
                // BigInt == Object: ToPrimitive with the default hint, then
                // re-enter abstract equality exactly once.
                Var primitive = JavascriptConversion::ToPrimitive<JavascriptHint::None>(right, requestContext);
                return JavascriptBigInt::Equals(left, primitive, value, requestContext);
            }
            break;
        }
        return TRUE;
    }

    void JavascriptBigInt::Resize(digit_t length)
    {
        digit_t *digits;

        if (length <= m_maxLength)
        {
            return;
        }

        length += length;// double size
        if (SIZE_MAX / sizeof(digit_t) < length) // overflow 
        {
            JavascriptError::ThrowRangeError(this->GetScriptContext(), VBSERR_TypeMismatch, _u("Resize BigInt"));
        }

        digits = RecyclerNewArrayLeaf(this->GetScriptContext()->GetRecycler(), digit_t, length);
        if (NULL == digits)
        {
            JavascriptError::ThrowRangeError(this->GetScriptContext(), VBSERR_TypeMismatch, _u("Resize BigInt"));
        }

        if (0 < m_length) // in this case, we need to copy old data over
        {
            js_memcpy_s(digits, length * sizeof(digit_t), m_digits, m_length * sizeof(digit_t));
        }

        m_digits = digits;
        m_maxLength = length;
    }

    template <typename EncodedChar>
    void JavascriptBigInt::InitFromCharDigits(const EncodedChar *pChar, uint32 charLength, bool isNegative)
    {
        Assert(charLength > 0);
        Assert(pChar != 0);

        const EncodedChar *pCharLimit = pChar + charLength - 1;//'n' at the end
        Assert(*pCharLimit == _u('n'));

        m_length = 0;
        m_digits = RecyclerNewArrayLeaf(this->GetScriptContext()->GetRecycler(), digit_t, m_maxLength);
        m_isNegative = isNegative;

        // The scanner keeps the whole literal text in the token pid, so a
        // 0x/0X, 0o/0O or 0b/0B prefix reaches this constructor as well. The
        // digits after the prefix may contain numeric separators.
        uint32 radix = 10;
        if (charLength >= 3 && pChar[0] == _u('0'))
        {
            switch (pChar[1])
            {
            case _u('x'):
            case _u('X'):
                radix = 16;
                pChar += 2;
                break;
            case _u('o'):
            case _u('O'):
                radix = 8;
                pChar += 2;
                break;
            case _u('b'):
            case _u('B'):
                radix = 2;
                pChar += 2;
                break;
            }
        }

        if (radix == 10)
        {
            digit_t digitMul = 1;
            digit_t digitAdd = 0;
            for (; pChar < pCharLimit; pChar++)
            {
                if (*pChar == _u('_'))
                {
                    // The scanner validated separator placement for decimal
                    // BigInt literals too; exclude it from the value.
                    continue;
                }
                Assert(NumberUtilities::IsDigit(*pChar));
                if (digitMul == 1e9)
                {
                    MulThenAdd(digitMul, digitAdd);
                    digitMul = 1;
                    digitAdd = 0;
                }
                digitMul *= 10;
                digitAdd = digitAdd * 10 + *pChar - '0';
            }
            Assert(1 < digitMul);
            MulThenAdd(digitMul, digitAdd);
        }
        else
        {
            Assert(radix == 2 || radix == 8 || radix == 16);

            // Accumulate the largest radix power that fits a limb, then fold
            // that chunk with one MulThenAdd. A per-digit MulThenAdd is
            // quadratic for long source literals and lets untrusted source
            // burn minutes in the scanner/constant path.
            uint32 chunkDigits = 0;
            digit_t chunkBase = 1;
            const digit_t maxDigit = static_cast<digit_t>(~static_cast<digit_t>(0));
            while (chunkBase <= maxDigit / radix)
            {
                chunkBase *= radix;
                chunkDigits++;
            }
            Assert(chunkDigits != 0);

            digit_t chunkValue = 0;
            digit_t chunkMul = 1;
            uint32 chunkCount = 0;
            for (; pChar < pCharLimit; pChar++)
            {
                if (*pChar == _u('_'))
                {
                    // The scanner's base parser already validated separator
                    // placement (between two digits); just exclude it here.
                    continue;
                }
                uint32 digitValue = BigIntDigitValue(static_cast<char16>(*pChar), radix);
                Assert(digitValue != static_cast<uint32>(-1));
                if (digitValue == static_cast<uint32>(-1))
                {
                    continue; // unreachable for scanner-produced literals
                }
                chunkValue = chunkValue * radix + digitValue;
                chunkMul *= radix;
                chunkCount++;
                if (chunkCount == chunkDigits)
                {
                    MulThenAdd(chunkMul, chunkValue);
                    chunkValue = 0;
                    chunkMul = 1;
                    chunkCount = 0;
                }
            }
            if (chunkCount != 0)
            {
                MulThenAdd(chunkMul, chunkValue);
            }
        }

        // make sure this is no negative zero
        if (m_length == 0)
        {
            m_isNegative = false;
            m_length = 1;
            m_digits[0] = 0;
        }
    }

    // return low(a+b) and out carry
    digit_t JavascriptBigInt::AddDigit(digit_t a, digit_t b, digit_t* carry)
    {
        digit_t result = a + b;
        if (result < a)
        {
            *carry += 1;
        }
        return result;
    }

    // return low(a+b) and out carry
    digit_t JavascriptBigInt::SubDigit(digit_t a, digit_t b, digit_t* borrow)
    {
        digit_t result = a - b;
        if (result > a)
        {
            *borrow += 1;
        }
        return result;
    }

    bool JavascriptBigInt::IsZero(JavascriptBigInt * pbi)
    {
        return pbi->IsZeroValue();
    }

    void JavascriptBigInt::AbsoluteIncrement(JavascriptBigInt * pbi)
    {
        JavascriptBigInt* result = pbi;
        digit_t carry = 1;
        for (digit_t i = 0; i < result->m_length && carry > 0; i++)
        {
            digit_t tempCarry = 0;
            result->m_digits[i] = JavascriptBigInt::AddDigit(result->m_digits[i], carry, &tempCarry);
            carry = tempCarry;
        }
        if (carry > 0) //increase length
        {
            if (result->m_length >= result->m_maxLength)
            {
                result->Resize(result->m_length + 1);
            }
            result->m_digits[result->m_length++] = carry;
        }
    }

    void JavascriptBigInt::AbsoluteDecrement(JavascriptBigInt * pbi)
    {
        JavascriptBigInt* result = pbi;
        Assert(!JavascriptBigInt::IsZero(result));
        digit_t borrow = 1;
        for (digit_t i = 0; i < result->m_length && borrow > 0; i++)
        {
            digit_t tempBorrow = 0;
            result->m_digits[i] = JavascriptBigInt::SubDigit(result->m_digits[i], borrow, &tempBorrow);
            borrow = tempBorrow;
        }
        Assert(borrow == 0);
        // Remove trailing zero limbs and canonicalize a zero result to one
        // zero limb; 1n - 1n must compare equal to the 0n literal.
        while (result->m_length > 1 && result->m_digits[result->m_length - 1] == 0)
        {
            result->m_length--;
        }
        if (result->m_length == 1 && result->m_digits[0] == 0)
        {
            result->m_isNegative = false;
        }
    }

    void JavascriptBigInt::Increment(JavascriptBigInt * pbi)
    {
        if (pbi->m_isNegative)
        {
            // return 0n for -1n
            if (pbi->m_length == 1 && pbi->m_digits[0] == 1)
            {
                JavascriptBigInt* result = pbi;
                result->m_digits[0] = 0;
                result->m_isNegative = false;
                return;
            }
            return JavascriptBigInt::AbsoluteDecrement(pbi);
        }
        return JavascriptBigInt::AbsoluteIncrement(pbi);
    }

    void JavascriptBigInt::Decrement(JavascriptBigInt * pbi)
    {
        if (pbi->m_isNegative)
        {
            return JavascriptBigInt::AbsoluteIncrement(pbi);
        }
        if (JavascriptBigInt::IsZero(pbi)) // return -1n for 0n
        {
            JavascriptBigInt* result = pbi;
            result->m_digits[0] = 1;
            result->m_isNegative = true;
            return;
        }
        return JavascriptBigInt::AbsoluteDecrement(pbi);
    }

    Var JavascriptBigInt::Increment(Var aRight)
    {
        JavascriptBigInt* rightBigInt = VarTo<JavascriptBigInt>(aRight);
        JavascriptBigInt* newBigInt = JavascriptBigInt::New(rightBigInt, rightBigInt->GetScriptContext());
        JavascriptBigInt::Increment(newBigInt);
        return newBigInt;
    }

    Var JavascriptBigInt::Decrement(Var aRight)
    {
        JavascriptBigInt* rightBigInt = VarTo<JavascriptBigInt>(aRight);
        JavascriptBigInt* newBigInt = JavascriptBigInt::New(rightBigInt, rightBigInt->GetScriptContext());
        JavascriptBigInt::Decrement(newBigInt);
        return newBigInt;
    }

    Var JavascriptBigInt::Not(Var aRight)
    {
        JavascriptBigInt* rightBigInt = VarTo<JavascriptBigInt>(aRight);
        JavascriptBigInt* newBigInt = JavascriptBigInt::New(rightBigInt, rightBigInt->GetScriptContext());
        JavascriptBigInt::Negate(newBigInt);
        JavascriptBigInt * bigintOne = JavascriptBigInt::CreateOne(rightBigInt->GetScriptContext());
        return JavascriptBigInt::Sub(newBigInt, bigintOne);
    }

    Var JavascriptBigInt::Negate(Var aRight)
    {
        JavascriptBigInt* rightBigInt = VarTo<JavascriptBigInt>(aRight);
        JavascriptBigInt* newBigInt = JavascriptBigInt::New(rightBigInt, rightBigInt->GetScriptContext());
        JavascriptBigInt::Negate(newBigInt);
        return newBigInt;
    }

    void JavascriptBigInt::Negate(JavascriptBigInt * pbi)
    {
        if (JavascriptBigInt::IsZero(pbi))
        {
            return;
        }
        pbi->m_isNegative = !pbi->m_isNegative;
    }

    // return low(a*b) and out high
    digit_t JavascriptBigInt::MulDigit(digit_t a, digit_t b, digit_t* resultHigh)
    {
        // Multiply is performed in half chuck favor.
        // For inputs [AH AL]*[BH BL], the result is:
        //
        //            [AL*BL]  // rLow
        //    +    [AL*BH]     // rMid1
        //    +    [AH*BL]     // rMid2
        //    + [AH*BH]        // rHigh
        //    = [R1 R2 R3 R4]  // high = [R1 R2], low = [R3 R4]
        //

        digit_t kHalfDigitBits = sizeof(digit_t) * 4;
        digit_t kHalfDigitMask = ((digit_t)1 << kHalfDigitBits) - 1;

        digit_t aLow = a & kHalfDigitMask;
        digit_t aHigh = a >> kHalfDigitBits;
        digit_t bLow = b & kHalfDigitMask;
        digit_t bHigh = b >> kHalfDigitBits;

        digit_t rLow = aLow * bLow;
        digit_t rMid1 = aLow * bHigh;
        digit_t rMid2 = aHigh * bLow;
        digit_t rHigh = aHigh * bHigh;

        digit_t carry = 0;
        digit_t resultLow = JavascriptBigInt::AddDigit(rLow, rMid1 << kHalfDigitBits, &carry);
        resultLow = JavascriptBigInt::AddDigit(resultLow, rMid2 << kHalfDigitBits, &carry);
        *resultHigh = (rMid1 >> kHalfDigitBits) + (rMid2 >> kHalfDigitBits) + rHigh + carry;
        return resultLow;
    }

    void JavascriptBigInt::MulThenAdd(digit_t digitMul, digit_t digitAdd)
    {
        Assert(digitMul != 0);

        digit_t carryDigit = 0;
        digit_t *pDigit = m_digits;
        digit_t *pDigitLimit = pDigit + m_length;

        for (; pDigit < pDigitLimit; pDigit++)
        {
            *pDigit = JavascriptBigInt::MulDigit(*pDigit, digitMul, &carryDigit);// return low Digit to digit, hight Digit to carry
            if (digitAdd > 0)
            {
                *pDigit = JavascriptBigInt::AddDigit(*pDigit, digitAdd, &carryDigit);// add carry to result
            }
            digitAdd = carryDigit;
        }
        if (0 < digitAdd) // length increase by 1
        {
            if (m_length >= m_maxLength)
            {
                Resize(m_length + 1);
            }
            m_digits[m_length++] = digitAdd;
        }
    }

    // Compare this BigInt's magnitude (m_isNegative is ignored) with an
    // unsigned 64-bit integer. Returns -1/0/1.
    int JavascriptBigInt::CompareMagnitudeToUInt64(uint64 value) const
    {
        const uint32 limbBits = BigIntLimbBits();
        uint64 magnitude = 0;
        if (limbBits >= 64)
        {
            // On a 64-bit limb build a canonical value with more than one
            // limb is at least 2**64, beyond any uint64.
            if (m_length > 1)
            {
                return 1;
            }
            magnitude = m_length == 1 ? static_cast<uint64>(m_digits[0]) : 0;
        }
        else
        {
            if (m_length > 2)
            {
                return 1;
            }
            for (digit_t i = m_length; i > 0; i--)
            {
                magnitude = (magnitude << limbBits) | static_cast<uint64>(m_digits[i - 1]);
            }
        }
        return magnitude > value ? 1 : (magnitude < value ? -1 : 0);
    }

    int JavascriptBigInt::CompareToUInt64(bool valueIsNegative, uint64 value) const
    {
        if (IsZeroValue())
        {
            if (value == 0)
            {
                return 0;
            }
            return valueIsNegative ? 1 : -1;
        }
        if (m_isNegative != valueIsNegative)
        {
            return m_isNegative ? -1 : 1;
        }
        int comparison = CompareMagnitudeToUInt64(value);
        return m_isNegative ? -comparison : comparison;
    }

    bool JavascriptBigInt::CompareToDouble(double value, int* result) const
    {
        if (JavascriptNumber::IsNan(value))
        {
            return false;
        }
        if (JavascriptNumber::IsPosInf(value))
        {
            *result = -1; // every BigInt is less than +Infinity
            return true;
        }
        if (JavascriptNumber::IsNegInf(value))
        {
            *result = 1; // every BigInt is greater than -Infinity
            return true;
        }
        if (value == 0)
        {
            *result = IsZeroValue() ? 0 : (m_isNegative ? -1 : 1);
            return true;
        }

        bool isIntegral = (value == floor(value));
        // A fractional double lies strictly between floor(value) and
        // ceil(value); comparing against floor is exact and handles both
        // signs. Equality to a fractional value is impossible.
        double integerValue = isIntegral ? value : floor(value);

        JavascriptBigInt* other = JavascriptBigInt::CreateFromNumber(integerValue, GetScriptContext());
        if (other == nullptr)
        {
            // Unreachable after the NaN/Infinity/integral checks above; fail
            // closed rather than dereference a null conversion.
            *result = 0;
            return false;
        }
        int comparison = Compare(other);
        if (!isIntegral)
        {
            // A BigInt not greater than floor(value) is below the double;
            // otherwise it is above it.
            *result = comparison <= 0 ? -1 : 1;
        }
        else
        {
            *result = comparison;
        }
        return true;
    }

    int JavascriptBigInt::CompareTo(const JavascriptBigInt* other) const
    {
        return Compare(const_cast<JavascriptBigInt*>(other));
    }

    int JavascriptBigInt::Compare(JavascriptBigInt *pbi) const
    {
        if (m_isNegative != pbi->m_isNegative)
        {
            if (m_isNegative)
            {
                return -1;
            }
            else
            {
                return 1;
            }
        }

        int sign = m_isNegative ? -1 : 1;
        return sign * JavascriptBigInt::CompareAbsolute(pbi);
    }

    int JavascriptBigInt::CompareAbsolute(JavascriptBigInt *pbi) const
    {
        digit_t index;

        // Intermediates can be zero with either a zero limb or a zero length
        // (AbsoluteDecrement used to leave the latter). Compare mathematical
        // zeros as equal regardless of their storage shape.
        bool thisZero = IsZeroValue();
        bool otherZero = pbi->IsZeroValue();
        if (thisZero || otherZero)
        {
            return thisZero ? (otherZero ? 0 : -1) : 1;
        }

        if (m_length > pbi->m_length)
        {
            return 1;
        }
        if (m_length < pbi->m_length)
        {
            return -1;
        }
        if (0 == m_length)
        {
            return 0;
        }

#pragma prefast(suppress:__WARNING_LOOP_ONLY_EXECUTED_ONCE,"noise")
        for (index = m_length - 1; m_digits[index] == pbi->m_digits[index]; index--)
        {
            if (0 == index)
            {
                return 0;
            }
        }
        Assert(m_digits[index] != pbi->m_digits[index]);

        return (m_digits[index] > pbi->m_digits[index]) ? 1 : -1;
    }

    bool JavascriptBigInt::LessThan(Var aLeft, Var aRight)
    {
        AssertMsg(VarIs<JavascriptBigInt>(aLeft) && VarIs<JavascriptBigInt>(aRight), "BigInt LessThan");

        JavascriptBigInt *leftBigInt = VarTo<JavascriptBigInt>(aLeft);
        JavascriptBigInt *rightBigInt = VarTo<JavascriptBigInt>(aRight);

        return (leftBigInt->Compare(rightBigInt) < 0);
    }

    bool JavascriptBigInt::Equals(Var aLeft, Var aRight)
    {
        JavascriptBigInt *leftBigInt = VarTo<JavascriptBigInt>(aLeft);
        JavascriptBigInt *rightBigInt = VarTo<JavascriptBigInt>(aRight);

        return (leftBigInt->Compare(rightBigInt) == 0);
    }

    // pbi1 += pbi2 assume pbi1 has length no less than pbi2
    void JavascriptBigInt::AddAbsolute(JavascriptBigInt * pbi1, JavascriptBigInt * pbi2)
    {
        Assert(pbi1->m_length >= pbi2->m_length);
        digit_t carryDigit = 0;
        digit_t *pDigit1 = pbi1->m_digits;
        digit_t *pDigit2 = pbi2->m_digits;
        digit_t i = 0;

        for (; i < pbi2->m_length; i++)
        {
            digit_t tempCarryDigit = 0;
            pDigit1[i] = JavascriptBigInt::AddDigit(pDigit1[i], pDigit2[i], &tempCarryDigit);
            pDigit1[i] = JavascriptBigInt::AddDigit(pDigit1[i], carryDigit, &tempCarryDigit);
            carryDigit = tempCarryDigit;
        }

        for (; i < pbi1->m_length && carryDigit > 0; i++)
        {
            digit_t tempCarryDigit = 0;
            pDigit1[i] = JavascriptBigInt::AddDigit(pDigit1[i], carryDigit, &tempCarryDigit);
            carryDigit = tempCarryDigit;
        }

        if (0 < carryDigit) // length increase by 1
        {
            if (pbi1->m_length >= pbi1->m_maxLength)
            {
                pbi1->Resize(pbi1->m_length + 1);
            }
            pbi1->m_digits[pbi1->m_length++] = carryDigit;
        }
    }

    // pbi1 -= pbi2 assume |pbi1| > |pbi2|
    void JavascriptBigInt::SubAbsolute(JavascriptBigInt * pbi1, JavascriptBigInt * pbi2)
    {
        Assert(pbi1->CompareAbsolute(pbi2) == 1);
        digit_t carryDigit = 0;
        digit_t *pDigit1 = pbi1->m_digits;
        digit_t *pDigit2 = pbi2->m_digits;
        digit_t i = 0;

        for (; i < pbi2->m_length; i++)
        {
            digit_t tempCarryDigit = 0;
            pDigit1[i] = JavascriptBigInt::SubDigit(pDigit1[i], pDigit2[i], &tempCarryDigit);
            pDigit1[i] = JavascriptBigInt::SubDigit(pDigit1[i], carryDigit, &tempCarryDigit);
            carryDigit = tempCarryDigit;
        }

        for (; i < pbi1->m_length && carryDigit > 0; i++)
        {
            digit_t tempCarryDigit = 0;
            pDigit1[i] = JavascriptBigInt::SubDigit(pDigit1[i], carryDigit, &tempCarryDigit);
            carryDigit = tempCarryDigit;
        }
        // adjust length
        while ((pbi1->m_length>0) && (pbi1->m_digits[pbi1->m_length-1] == 0))
        {
            pbi1->m_length--;
        }
        Assert(pbi1->m_length > 0);
    }

    JavascriptBigInt * JavascriptBigInt::Sub(JavascriptBigInt * pbi1, JavascriptBigInt * pbi2)
    {
        if (JavascriptBigInt::IsZero(pbi1))
        {
            // Negate through the canonical helper so 0n - 0n stays positive
            // zero instead of acquiring a negative sign bit.
            JavascriptBigInt::Negate(pbi2);
            return pbi2;
        }
        if (JavascriptBigInt::IsZero(pbi2))
        {
            return pbi1;
        }

        if (pbi2->m_isNegative)
        {
            pbi2->m_isNegative = false;
            return JavascriptBigInt::Add(pbi1, pbi2);// a-(-b)=a+b
        }

        if (pbi1->m_isNegative) // -a-b=-(a+b)
        {
            if (pbi1->m_length >= pbi2->m_length)
            {
                JavascriptBigInt::AddAbsolute(pbi1, pbi2);
                return pbi1;
            }
            JavascriptBigInt::AddAbsolute(pbi2, pbi1);
            return pbi2;
        }
        else // both positive 
        {
            switch (pbi1->CompareAbsolute(pbi2))
            {
            case 0: // a -a = 0
                return JavascriptBigInt::CreateZero(pbi1->GetScriptContext());
            case 1: 
                JavascriptBigInt::SubAbsolute(pbi1, pbi2); // a - b > 0
                return pbi1;
            default:
                pbi2->m_isNegative = true;
                JavascriptBigInt::SubAbsolute(pbi2, pbi1); // a - b = - (b-a) < 0
                return pbi2;
            }
        }
    }

    // return |pbi1| * |pbi2|
    JavascriptBigInt * JavascriptBigInt::MulAbsolute(JavascriptBigInt * pbi1, JavascriptBigInt * pbi2)
    {
        // Start with maximum length possible in pbi3
        digit_t length = pbi1->m_length + pbi2->m_length;
        if (SIZE_MAX / sizeof(digit_t) < length) // overflow 
        {
            JavascriptError::ThrowRangeError(pbi1->GetScriptContext(), VBSERR_TypeMismatch, _u("Multiply BigInt"));
        }
        JavascriptBigInt * pbi3 = JavascriptBigInt::CreateZeroWithLength(length, pbi1->GetScriptContext());

        // Compute pbi3 = pbi1 * pbi2 as follow:
        // e.g. A1 A0 * B1 B0 = C3 C2 C1 C0
        // C0 = A0 * B0 (take the digit and carry)
        // C1 = carry + A0 * B1 + A1 * B0 (take the digit and carry)
        // C2 = carry + A1 * B1 (take the digit and carry)
        // C3 = carry
        digit_t carryDigit = 0;
        digit_t i3 = 0;

        for (digit_t i1 = 0; i1 < pbi1->m_length; i1++)
        {
            carryDigit = 0;
            for (digit_t i2 = 0; i2 < pbi2->m_length; i2++)
            {
                i3 = i1 + i2;
                digit_t tempCarryDigit1 = 0;
                digit_t tempCarryDigit2 = 0;
                pbi3->m_digits[i3] = JavascriptBigInt::AddDigit(pbi3->m_digits[i3], carryDigit, &tempCarryDigit1);
                digit_t mulDigitResult = JavascriptBigInt::MulDigit(pbi1->m_digits[i1], pbi2->m_digits[i2], &carryDigit);
                pbi3->m_digits[i3] = JavascriptBigInt::AddDigit(pbi3->m_digits[i3], mulDigitResult, &tempCarryDigit2);
                digit_t overflow = 0;
                carryDigit = JavascriptBigInt::AddDigit(carryDigit, tempCarryDigit1, &overflow);
                Assert(overflow == 0); // [i1] * [i2] can not carry through [i1+i2+2]
                carryDigit = JavascriptBigInt::AddDigit(carryDigit, tempCarryDigit2, &overflow);
                Assert(overflow == 0); // [i1] * [i2] can not carry through [i1+i2+2]
            }
            if (carryDigit > 0)
            {
                pbi3->m_digits[i3 + 1] = carryDigit;
            }
        }
        
        // adjust length
        while ((pbi3->m_length > 0) && (pbi3->m_digits[pbi3->m_length - 1] == 0))
        {
            pbi3->m_length--;
        }
        Assert(pbi3->m_length > 0);
        return pbi3;
    }

    JavascriptBigInt * JavascriptBigInt::Mul(JavascriptBigInt * pbi1, JavascriptBigInt * pbi2)
    {
        if (JavascriptBigInt::IsZero(pbi1) || JavascriptBigInt::IsZero(pbi2))
        {
            return JavascriptBigInt::CreateZero(pbi1->GetScriptContext());
        }
        JavascriptBigInt * result = JavascriptBigInt::MulAbsolute(pbi1, pbi2);
        if (pbi1->m_isNegative != pbi2->m_isNegative) 
        {
            result->m_isNegative = true;
        }
        return result;
    }

    JavascriptBigInt * JavascriptBigInt::Add(JavascriptBigInt * pbi1, JavascriptBigInt * pbi2)
    {
        if (JavascriptBigInt::IsZero(pbi1)) 
        {
            return pbi2;
        }
        if (JavascriptBigInt::IsZero(pbi2))
        {
            return pbi1;
        }

        if (pbi1->m_isNegative == pbi2->m_isNegative) // (-a)+(-b) = -(a+b)
        {
            if (pbi1->m_length >= pbi2->m_length)
            {
                JavascriptBigInt::AddAbsolute(pbi1, pbi2);
                return pbi1;
            }
            JavascriptBigInt::AddAbsolute(pbi2, pbi1);
            return pbi2;
        }
        else 
        {
            switch (pbi1->CompareAbsolute(pbi2))
            {
            case 0:
                return JavascriptBigInt::CreateZero(pbi1->GetScriptContext()); // a + (-a) = -a + a = 0
            case 1: 
                JavascriptBigInt::SubAbsolute(pbi1, pbi2); // a + (-b) = a - b or (-a) + b = -(a-b)
                return pbi1;
            default:
                JavascriptBigInt::SubAbsolute(pbi2, pbi1); // -a + b = b - a or a + (-b) = -(b-a)
                return pbi2;
            }
        }
    }

    Var JavascriptBigInt::Add(Var aLeft, Var aRight)
    {
        JavascriptBigInt *leftBigInt = VarTo<JavascriptBigInt>(aLeft);
        JavascriptBigInt *rightBigInt = VarTo<JavascriptBigInt>(aRight);
        return JavascriptBigInt::Add(JavascriptBigInt::New(leftBigInt, leftBigInt->GetScriptContext()), JavascriptBigInt::New(rightBigInt, rightBigInt->GetScriptContext()));
        // TODO: Consider deferring creation of new instances until we need them
    }

    Var JavascriptBigInt::Sub(Var aLeft, Var aRight)
    {
        JavascriptBigInt *leftBigInt = VarTo<JavascriptBigInt>(aLeft);
        JavascriptBigInt *rightBigInt = VarTo<JavascriptBigInt>(aRight);
        return JavascriptBigInt::Sub(JavascriptBigInt::New(leftBigInt, leftBigInt->GetScriptContext()), JavascriptBigInt::New(rightBigInt, rightBigInt->GetScriptContext()));
        // TODO: Consider deferring creation of new instances until we need them
    }

    Var JavascriptBigInt::Mul(Var aLeft, Var aRight)
    {
        JavascriptBigInt *leftBigInt = VarTo<JavascriptBigInt>(aLeft);
        JavascriptBigInt *rightBigInt = VarTo<JavascriptBigInt>(aRight);
        return JavascriptBigInt::Mul(leftBigInt, rightBigInt);
    }

    // ------------------------------------------------------------------
    // Division and remainder
    // ------------------------------------------------------------------
    //
    // Knuth's Algorithm D over 32-bit half-limbs. The BigInt magnitude is a
    // little-endian array of digit_t, 32 or 64 bits wide; working in uint32
    // units keeps every intermediate in uint64 (including the 32x32->64
    // products and the normalized subtraction) without __int128, which is the
    // same portability rule the existing DivModInPlace/ToDouble code follows.
    // A zero divisor throws RangeError before any result is published.

    uint64 JavascriptBigInt::GetMagnitudeAsUInt64(const JavascriptBigInt * pbi, bool* tooLarge)
    {
        *tooLarge = false;
        uint64 value = 0;
        if (BigIntLimbBits() >= 64)
        {
            if (pbi->m_length > 1)
            {
                *tooLarge = true;
                return 0;
            }
            value = pbi->m_length == 1 ? static_cast<uint64>(pbi->m_digits[0]) : 0;
        }
        else
        {
            if (pbi->m_length > 2)
            {
                *tooLarge = true;
                return 0;
            }
            for (digit_t i = pbi->m_length; i > 0; i--)
            {
                value = (value << BigIntLimbBits()) | static_cast<uint64>(pbi->m_digits[i - 1]);
            }
        }
        return value;
    }

    JavascriptBigInt * JavascriptBigInt::CreateFromUnits(
        const uint32 * units, uint32 unitCount, bool isNegative, ScriptContext * scriptContext)
    {
        Assert(units != nullptr);
        while (unitCount > 0 && units[unitCount - 1] == 0)
        {
            unitCount--;
        }

        const uint32 unitsPerLimb = BigIntUnitsPerLimb();
        const uint32 limbCount = unitCount == 0 ? 1 : (unitCount + unitsPerLimb - 1) / unitsPerLimb;
        JavascriptBigInt * result = JavascriptBigInt::CreateZeroWithLength(limbCount, scriptContext);
        for (uint32 i = 0; i < unitCount; i++)
        {
            result->m_digits[i / unitsPerLimb] |= static_cast<digit_t>(units[i]) << (32 * (i % unitsPerLimb));
        }
        while (result->m_length > 1 && result->m_digits[result->m_length - 1] == 0)
        {
            result->m_length--;
        }
        result->m_isNegative = isNegative && !result->IsZeroValue();
        return result;
    }

    JavascriptBigInt * JavascriptBigInt::DivMod(JavascriptBigInt * pbi1, JavascriptBigInt * pbi2, bool wantQuotient)
    {
        ScriptContext * scriptContext = pbi1->GetScriptContext();
        if (pbi2->IsZeroValue())
        {
            // BigInt::divide/remainder step 1: a zero divisor is a RangeError.
            JavascriptError::ThrowRangeError(scriptContext, VBSERR_TypeMismatch, _u("Divide BigInt"));
        }
        if (pbi1->IsZeroValue())
        {
            return JavascriptBigInt::CreateZero(scriptContext);
        }
        if (pbi1->CompareAbsolute(pbi2) < 0)
        {
            // |dividend| < |divisor|: quotient is 0n, remainder is the
            // dividend (with its own sign).
            return wantQuotient ? JavascriptBigInt::CreateZero(scriptContext) : JavascriptBigInt::New(pbi1, scriptContext);
        }

        const uint32 unitsPerLimb = BigIntUnitsPerLimb();
        uint32 uCount = static_cast<uint32>(pbi1->m_length * unitsPerLimb);
        uint32 vCount = static_cast<uint32>(pbi2->m_length * unitsPerLimb);
        uint32 * u = RecyclerNewArrayLeafZ(scriptContext->GetRecycler(), uint32, uCount);
        uint32 * v = RecyclerNewArrayLeafZ(scriptContext->GetRecycler(), uint32, vCount);
        for (digit_t i = 0; i < pbi1->m_length; i++)
        {
            digit_t digit = pbi1->m_digits[i];
            for (uint32 j = 0; j < unitsPerLimb; j++)
            {
                u[i * unitsPerLimb + j] = static_cast<uint32>(digit >> (32 * j));
            }
        }
        for (digit_t i = 0; i < pbi2->m_length; i++)
        {
            digit_t digit = pbi2->m_digits[i];
            for (uint32 j = 0; j < unitsPerLimb; j++)
            {
                v[i * unitsPerLimb + j] = static_cast<uint32>(digit >> (32 * j));
            }
        }
        while (uCount > 0 && u[uCount - 1] == 0) uCount--;
        while (vCount > 0 && v[vCount - 1] == 0) vCount--;
        Assert(vCount > 0);
        Assert(uCount > 0);

        if (vCount == 1)
        {
            // Single 32-bit divisor: the classic long-division loop.
            uint32 divisor = v[0];
            uint32 * quotientUnits = RecyclerNewArrayLeafZ(scriptContext->GetRecycler(), uint32, uCount);
            uint64 remainder = 0;
            for (uint32 i = uCount; i > 0; i--)
            {
                uint64 current = (remainder << 32) | u[i - 1];
                quotientUnits[i - 1] = static_cast<uint32>(current / divisor);
                remainder = current % divisor;
            }
            uint32 remainderUnit = static_cast<uint32>(remainder);
            JavascriptBigInt * quotient = JavascriptBigInt::CreateFromUnits(quotientUnits, uCount, false, scriptContext);
            quotient->m_isNegative = (pbi1->m_isNegative != pbi2->m_isNegative) && !quotient->IsZeroValue();
            return wantQuotient ? quotient :
                JavascriptBigInt::CreateFromUnits(&remainderUnit, 1, pbi1->m_isNegative, scriptContext);
        }

        // Normalize so the divisor's top bit is set. Shift counts stay below
        // 32 because the top 32-bit unit of a canonical magnitude is nonzero.
        uint32 shift = 0;
        for (uint32 top = v[vCount - 1]; (top & 0x80000000u) == 0; top <<= 1)
        {
            shift++;
        }

        uint32 * normalizedV = RecyclerNewArrayLeafZ(scriptContext->GetRecycler(), uint32, vCount);
        for (uint32 i = 0; i < vCount; i++)
        {
            uint32 low = i == 0 ? 0 : v[i - 1];
            normalizedV[i] = shift == 0
                ? v[i]
                : (v[i] << shift) | (low >> (32 - shift));
        }

        uint32 * normalizedU = RecyclerNewArrayLeafZ(scriptContext->GetRecycler(), uint32, uCount + 1);
        normalizedU[uCount] = shift == 0 ? 0 : (u[uCount - 1] >> (32 - shift));
        for (uint32 i = 0; i < uCount; i++)
        {
            uint32 low = i == 0 ? 0 : u[i - 1];
            normalizedU[i] = shift == 0
                ? u[i]
                : (u[i] << shift) | (low >> (32 - shift));
        }

        const uint32 quotientCount = uCount - vCount + 1;
        uint32 * quotientUnits = RecyclerNewArrayLeafZ(scriptContext->GetRecycler(), uint32, quotientCount);

        for (uint32 step = quotientCount; step > 0; step--)
        {
            uint32 j = step - 1;
            uint64 numerator = (static_cast<uint64>(normalizedU[j + vCount]) << 32) | normalizedU[j + vCount - 1];
            uint64 qhat = numerator / normalizedV[vCount - 1];
            uint64 rhat = numerator % normalizedV[vCount - 1];

            while (qhat > 0xFFFFFFFFull ||
                (qhat * normalizedV[vCount - 2] >
                    ((rhat << 32) | normalizedU[j + vCount - 2])))
            {
                qhat--;
                rhat += normalizedV[vCount - 1];
                if (rhat >= 0x100000000ull)
                {
                    break;
                }
            }

            // Multiply-subtract qhat * divisor from the current window. `k`
            // is the borrow, kept in int64 so the final t can be negative.
            int64 k = 0;
            for (uint32 i = 0; i < vCount; i++)
            {
                uint64 product = qhat * normalizedV[i];
                int64 t = static_cast<int64>(normalizedU[j + i]) - k -
                    static_cast<int64>(product & 0xFFFFFFFFull);
                normalizedU[j + i] = static_cast<uint32>(t);
                // The borrow beyond this 32-bit unit is ceil(-t / 2**32);
                // t can need two units when the incoming borrow and the
                // subtracted low half both exceed the current unit.
                uint64 borrow = 0;
                if (t < 0)
                {
                    uint64 magnitude = static_cast<uint64>(-(t + 1)) + 1;
                    borrow = (magnitude + 0xFFFFFFFFull) >> 32;
                }
                k = static_cast<int64>(product >> 32) + static_cast<int64>(borrow);
            }
            int64 top = static_cast<int64>(normalizedU[j + vCount]) - k;
            normalizedU[j + vCount] = static_cast<uint32>(top);
            quotientUnits[j] = static_cast<uint32>(qhat);

            if (top < 0)
            {
                // qhat was one too large: add the divisor back.
                quotientUnits[j]--;
                uint64 carry = 0;
                for (uint32 i = 0; i < vCount; i++)
                {
                    uint64 sum = static_cast<uint64>(normalizedU[j + i]) + normalizedV[i] + carry;
                    normalizedU[j + i] = static_cast<uint32>(sum);
                    carry = sum >> 32;
                }
                normalizedU[j + vCount] =
                    static_cast<uint32>(static_cast<uint64>(normalizedU[j + vCount]) + carry);
            }
        }

        Assert(normalizedU[vCount] == 0);
        uint32 * remainderUnits = RecyclerNewArrayLeafZ(scriptContext->GetRecycler(), uint32, vCount);
        if (shift == 0)
        {
            for (uint32 i = 0; i < vCount; i++)
            {
                remainderUnits[i] = normalizedU[i];
            }
        }
        else
        {
            for (uint32 i = 0; i < vCount; i++)
            {
                uint32 high = i + 1 < vCount ? normalizedU[i + 1] << (32 - shift) : 0;
                remainderUnits[i] = (normalizedU[i] >> shift) | high;
            }
        }

        JavascriptBigInt * quotient = JavascriptBigInt::CreateFromUnits(quotientUnits, quotientCount, false, scriptContext);
        quotient->m_isNegative = (pbi1->m_isNegative != pbi2->m_isNegative) && !quotient->IsZeroValue();
        return wantQuotient ? quotient :
            JavascriptBigInt::CreateFromUnits(remainderUnits, vCount, pbi1->m_isNegative, scriptContext);
    }

    Var JavascriptBigInt::Div(Var aLeft, Var aRight)
    {
        return JavascriptBigInt::DivMod(
            VarTo<JavascriptBigInt>(aLeft), VarTo<JavascriptBigInt>(aRight), true);
    }

    Var JavascriptBigInt::Modulus(Var aLeft, Var aRight)
    {
        return JavascriptBigInt::DivMod(
            VarTo<JavascriptBigInt>(aLeft), VarTo<JavascriptBigInt>(aRight), false);
    }

    // ------------------------------------------------------------------
    // Exponentiation
    // ------------------------------------------------------------------

    JavascriptBigInt * JavascriptBigInt::Exponentiate(JavascriptBigInt * pbiBase, JavascriptBigInt * pbiExponent)
    {
        ScriptContext * scriptContext = pbiBase->GetScriptContext();
        if (pbiExponent->m_isNegative)
        {
            // BigInt::exponentiate step 1: a negative exponent is a
            // RangeError, even for 0n or for an exponent that would otherwise
            // be handled by a trivial case.
            JavascriptError::ThrowRangeError(scriptContext, VBSERR_TypeMismatch, _u("Exponentiate BigInt"));
        }
        if (pbiExponent->IsZeroValue())
        {
            return JavascriptBigInt::CreateOne(scriptContext); // includes 0n ** 0n
        }
        if (pbiBase->IsZeroValue())
        {
            return JavascriptBigInt::CreateZero(scriptContext);
        }

        if (pbiBase->m_length == 1 && pbiBase->m_digits[0] == 1)
        {
            // (+/-1) ** n is always +/-1, independent of the exponent size.
            JavascriptBigInt * result = JavascriptBigInt::CreateOne(scriptContext);
            if (pbiBase->m_isNegative && (pbiExponent->m_digits[0] & 1) != 0)
            {
                result->m_isNegative = true;
            }
            return result;
        }

        bool exponentTooLarge = false;
        uint64 exponent = JavascriptBigInt::GetMagnitudeAsUInt64(pbiExponent, &exponentTooLarge);
        if (exponentTooLarge)
        {
            JavascriptError::ThrowRangeError(scriptContext, VBSERR_TypeMismatch, _u("Exponentiate BigInt"));
        }

        // Bound the result before multiplying. |base| >= 2 here, so the
        // result has more than baseBitLength * exponent bits; this estimate
        // is accurate enough to reject an oversized request in O(1).
        uint64 baseBits = pbiBase->GetMagnitudeBitLength();
        if (baseBits == 0 || exponent > BigIntMaxExponentResultBits / baseBits)
        {
            JavascriptError::ThrowRangeError(scriptContext, VBSERR_TypeMismatch, _u("Exponentiate BigInt"));
        }

        bool negate = pbiBase->m_isNegative && (exponent & 1) != 0;
        JavascriptBigInt * result = JavascriptBigInt::CreateOne(scriptContext);
        JavascriptBigInt * factor = JavascriptBigInt::New(pbiBase, scriptContext);
        factor->m_isNegative = false;
        while (exponent > 0)
        {
            if ((exponent & 1) != 0)
            {
                result = JavascriptBigInt::MulAbsolute(result, factor);
            }
            exponent >>= 1;
            if (exponent != 0)
            {
                factor = JavascriptBigInt::MulAbsolute(factor, factor);
            }
        }
        result->m_isNegative = negate;
        return result;
    }

    Var JavascriptBigInt::Exponentiate(Var aLeft, Var aRight)
    {
        return JavascriptBigInt::Exponentiate(
            VarTo<JavascriptBigInt>(aLeft), VarTo<JavascriptBigInt>(aRight));
    }

    // ------------------------------------------------------------------
    // Shift operators
    // ------------------------------------------------------------------
    //
    // BigInt::leftShift treats a negative count as an arithmetic right shift;
    // BigInt::signedRightShift likewise reverses a negative count. Right
    // shifts round toward negative infinity (floor), matching the infinite
    // two's-complement bit string, so -5n >> 1n is -3n.

    JavascriptBigInt * JavascriptBigInt::ShiftLeftMagnitude(const JavascriptBigInt * pbi, uint64 bits)
    {
        ScriptContext * scriptContext = pbi->GetScriptContext();
        if (pbi->IsZeroValue())
        {
            return JavascriptBigInt::CreateZero(scriptContext);
        }
        uint64 bitLength = pbi->GetMagnitudeBitLength();
        if (bits > BigIntMaxResultBitCount() || bitLength > BigIntMaxResultBitCount() - bits)
        {
            JavascriptError::ThrowRangeError(scriptContext, VBSERR_TypeMismatch, _u("Shift BigInt"));
        }

        const uint32 limbBits = BigIntLimbBits();
        digit_t limbShift = static_cast<digit_t>(bits / limbBits);
        uint32 bitShift = static_cast<uint32>(bits % limbBits);
        digit_t resultLength = pbi->m_length + limbShift + (bitShift == 0 ? 0 : 1);
        if (resultLength > BigIntMaxResultLimbs)
        {
            JavascriptError::ThrowRangeError(scriptContext, VBSERR_TypeMismatch, _u("Shift BigInt"));
        }

        JavascriptBigInt * result = JavascriptBigInt::CreateZeroWithLength(resultLength, scriptContext);
        for (digit_t i = 0; i < pbi->m_length; i++)
        {
            digit_t digit = pbi->m_digits[i];
            result->m_digits[i + limbShift] |= static_cast<digit_t>(digit << bitShift);
            if (bitShift != 0)
            {
                result->m_digits[i + limbShift + 1] |= static_cast<digit_t>(digit >> (limbBits - bitShift));
            }
        }
        while (result->m_length > 1 && result->m_digits[result->m_length - 1] == 0)
        {
            result->m_length--;
        }
        result->m_isNegative = pbi->m_isNegative;
        return result;
    }

    JavascriptBigInt * JavascriptBigInt::ShiftRightMagnitudeFloor(const JavascriptBigInt * pbi, uint64 bits)
    {
        ScriptContext * scriptContext = pbi->GetScriptContext();
        if (pbi->IsZeroValue())
        {
            return JavascriptBigInt::CreateZero(scriptContext);
        }

        uint64 bitLength = pbi->GetMagnitudeBitLength();
        if (bits >= bitLength)
        {
            // 0 for non-negative values; -1 for negative values (all
            // remaining bits are the sign extension).
            JavascriptBigInt * result = JavascriptBigInt::CreateOne(scriptContext);
            if (pbi->m_isNegative)
            {
                result->m_isNegative = true;
            }
            else
            {
                result->m_digits[0] = 0;
                result->m_isNegative = false;
            }
            return result;
        }

        const uint32 limbBits = BigIntLimbBits();
        digit_t limbShift = static_cast<digit_t>(bits / limbBits);
        uint32 bitShift = static_cast<uint32>(bits % limbBits);
        digit_t resultLength = pbi->m_length - limbShift;
        JavascriptBigInt * result = JavascriptBigInt::CreateZeroWithLength(resultLength, scriptContext);
        for (digit_t i = 0; i < resultLength; i++)
        {
            digit_t source = i + limbShift;
            digit_t digit = pbi->m_digits[source] >> bitShift;
            if (bitShift != 0 && source + 1 < pbi->m_length)
            {
                digit |= pbi->m_digits[source + 1] << (limbBits - bitShift);
            }
            result->m_digits[i] = digit;
        }
        while (result->m_length > 1 && result->m_digits[result->m_length - 1] == 0)
        {
            result->m_length--;
        }

        if (pbi->m_isNegative)
        {
            // floor(-m / 2^bits) = -(m >> bits) when no bit was dropped, and
            // one larger in magnitude otherwise.
            bool dropped = false;
            for (digit_t i = 0; i < limbShift && !dropped; i++)
            {
                dropped = pbi->m_digits[i] != 0;
            }
            if (!dropped && bitShift != 0)
            {
                digit_t mask = (static_cast<digit_t>(1) << bitShift) - 1;
                dropped = (pbi->m_digits[limbShift] & mask) != 0;
            }
            if (dropped)
            {
                JavascriptBigInt::AbsoluteIncrement(result);
            }
        }
        result->m_isNegative = pbi->m_isNegative && !result->IsZeroValue();
        return result;
    }

    Var JavascriptBigInt::LeftShift(Var aLeft, Var aRight)
    {
        JavascriptBigInt * value = VarTo<JavascriptBigInt>(aLeft);
        JavascriptBigInt * count = VarTo<JavascriptBigInt>(aRight);
        bool tooLarge = false;
        uint64 bits = JavascriptBigInt::GetMagnitudeAsUInt64(count, &tooLarge);
        if (tooLarge)
        {
            bits = ~static_cast<uint64>(0);
        }
        if (count->m_isNegative)
        {
            return JavascriptBigInt::ShiftRightMagnitudeFloor(value, bits);
        }
        return JavascriptBigInt::ShiftLeftMagnitude(value, bits);
    }

    Var JavascriptBigInt::RightShift(Var aLeft, Var aRight)
    {
        JavascriptBigInt * value = VarTo<JavascriptBigInt>(aLeft);
        JavascriptBigInt * count = VarTo<JavascriptBigInt>(aRight);
        bool tooLarge = false;
        uint64 bits = JavascriptBigInt::GetMagnitudeAsUInt64(count, &tooLarge);
        if (tooLarge)
        {
            bits = ~static_cast<uint64>(0);
        }
        if (count->m_isNegative)
        {
            return JavascriptBigInt::ShiftLeftMagnitude(value, bits);
        }
        return JavascriptBigInt::ShiftRightMagnitudeFloor(value, bits);
    }

    // ------------------------------------------------------------------
    // Bitwise operators
    // ------------------------------------------------------------------
    //
    // JavaScript BigInt bitwise operations use the infinite two's-complement
    // representation, not the sign-magnitude limbs. Materialize both operands
    // in an n-limb two's complement window (n = max length + 1, so the extra
    // limb carries the infinite sign extension), apply the operation, then
    // convert a negative result back to sign-magnitude.

    JavascriptBigInt * JavascriptBigInt::BitwiseOperation(
        const JavascriptBigInt * pbiLeft, const JavascriptBigInt * pbiRight, uint32 operation)
    {
        ScriptContext * scriptContext = pbiLeft->GetScriptContext();
        const digit_t limbCount = (pbiLeft->m_length > pbiRight->m_length ? pbiLeft->m_length : pbiRight->m_length) + 1;
        digit_t * left = RecyclerNewArrayLeafZ(scriptContext->GetRecycler(), digit_t, limbCount);
        digit_t * right = RecyclerNewArrayLeafZ(scriptContext->GetRecycler(), digit_t, limbCount);
        digit_t * result = RecyclerNewArrayLeafZ(scriptContext->GetRecycler(), digit_t, limbCount);

        auto fill = [&](const JavascriptBigInt * value, digit_t * target)
        {
            for (digit_t i = 0; i < value->m_length; i++)
            {
                target[i] = value->m_digits[i];
            }
            if (value->m_isNegative)
            {
                // Two's complement: complement every limb, then add one,
                // propagating a 0/1 carry until it stops.
                bool carry = true;
                for (digit_t i = 0; i < limbCount; i++)
                {
                    target[i] = ~target[i];
                    if (carry)
                    {
                        digit_t sum = target[i] + 1;
                        carry = sum == 0;
                        target[i] = sum;
                    }
                }
            }
        };
        fill(pbiLeft, left);
        fill(pbiRight, right);

        for (digit_t i = 0; i < limbCount; i++)
        {
            switch (operation)
            {
            case 0: result[i] = left[i] & right[i]; break;
            case 1: result[i] = left[i] | right[i]; break;
            default: result[i] = left[i] ^ right[i]; break;
            }
        }

        bool isNegative = (result[limbCount - 1] >> (BigIntLimbBits() - 1)) != 0;
        if (isNegative)
        {
            bool carry = true;
            for (digit_t i = 0; i < limbCount; i++)
            {
                result[i] = ~result[i];
                if (carry)
                {
                    digit_t sum = result[i] + 1;
                    carry = sum == 0;
                    result[i] = sum;
                }
            }
        }

        JavascriptBigInt * value = JavascriptBigInt::CreateZeroWithLength(limbCount, scriptContext);
        for (digit_t i = 0; i < limbCount; i++)
        {
            value->m_digits[i] = result[i];
        }
        while (value->m_length > 1 && value->m_digits[value->m_length - 1] == 0)
        {
            value->m_length--;
        }
        value->m_isNegative = isNegative && !value->IsZeroValue();
        return value;
    }

    Var JavascriptBigInt::BitwiseAnd(Var aLeft, Var aRight)
    {
        return JavascriptBigInt::BitwiseOperation(
            VarTo<JavascriptBigInt>(aLeft), VarTo<JavascriptBigInt>(aRight), 0);
    }

    Var JavascriptBigInt::BitwiseOr(Var aLeft, Var aRight)
    {
        return JavascriptBigInt::BitwiseOperation(
            VarTo<JavascriptBigInt>(aLeft), VarTo<JavascriptBigInt>(aRight), 1);
    }

    Var JavascriptBigInt::BitwiseXor(Var aLeft, Var aRight)
    {
        return JavascriptBigInt::BitwiseOperation(
            VarTo<JavascriptBigInt>(aLeft), VarTo<JavascriptBigInt>(aRight), 2);
    }

} // namespace Js
