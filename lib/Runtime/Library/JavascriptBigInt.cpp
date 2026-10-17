//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#include "RuntimeLibraryPch.h"

#include <cmath>
#include <limits>

namespace Js
{
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

        // BigInt() with no argument is ToBigInt(undefined): a TypeError, not a
        // host abort. Route it through the same conversion for one message.
        Var result = JavascriptConversion::ToBigInt(
            args.Info.Count > 1 ? args[1] : scriptContext->GetLibrary()->GetUndefined(),
            scriptContext);

        return isCtorSuperCall ?
            JavascriptOperators::OrdinaryCreateFromConstructor(VarTo<RecyclableObject>(newTarget), UnsafeVarTo<RecyclableObject>(result), nullptr, scriptContext) :
            result;
    }

    BOOL JavascriptBigInt::Equals(Var other, BOOL* value, ScriptContext * requestContext)
    {
        return JavascriptBigInt::Equals(this, other, value, requestContext);
    }

    BOOL JavascriptBigInt::Equals(JavascriptBigInt* left, Var right, BOOL* value, ScriptContext * requestContext)
    {
        switch (JavascriptOperators::GetTypeId(right))
        {
        case TypeIds_BigInt:
            *value = JavascriptBigInt::Equals(left, right);
            break;
        default:
            // Mixed abstract equality does not ToPrimitive wrapper operands
            // here (C3 work); treat any non-BigInt operand as unequal rather
            // than asserting the host.
            *value = FALSE;
            break;
        }
        return true;
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
        Assert(charLength >= 0);
        Assert(pChar != 0);

        const EncodedChar *pCharLimit = pChar + charLength - 1;//'n' at the end

        m_length = 0;
        m_digits = RecyclerNewArrayLeaf(this->GetScriptContext()->GetRecycler(), digit_t, m_maxLength);
        m_isNegative = isNegative;

        digit_t digitMul = 1;
        digit_t digitAdd = 0;
        for (; pChar < pCharLimit; pChar++)
        {
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
        // remove trailing zero
        if (result->m_digits[result->m_length-1] == 0)
        {
            result->m_length--;
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

    int JavascriptBigInt::Compare(JavascriptBigInt *pbi)
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

    int JavascriptBigInt::CompareAbsolute(JavascriptBigInt *pbi)
    {
        digit_t index;

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
            pbi2->m_isNegative = !pbi2->m_isNegative;
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

} // namespace Js
