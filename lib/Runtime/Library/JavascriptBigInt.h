//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#pragma once


namespace Js
{
    class JavascriptBigInt sealed : public RecyclableObject
    {
    private:
        Field(digit_t*) m_digits;         // digits
        Field(digit_t) m_length;          // length
        Field(digit_t) m_maxLength;          // max length without resize
        Field(bool) m_isNegative;

        static const digit_t InitDigitLength = 2;  // Max Digit length 

        DEFINE_VTABLE_CTOR(JavascriptBigInt, RecyclableObject);

    public:
        JavascriptBigInt(StaticType * type)
            : RecyclableObject(type), m_length(0), m_digits(nullptr), m_isNegative(false), m_maxLength(InitDigitLength)
        {
            Assert(type->GetTypeId() == TypeIds_BigInt);
        }

        JavascriptBigInt(const char16 * content, charcount_t cchUseLength, bool isNegative, StaticType * type)
            : JavascriptBigInt(type)
        {
            Assert(type->GetTypeId() == TypeIds_BigInt);
            InitFromCharDigits(content, cchUseLength, isNegative);
        }


        static bool LessThan(Var aLeft, Var aRight);
        static bool Equals(Var aLeft, Var aRight);
        // Abstract equality with a BigInt left operand. `right` may be any
        // primitive or an object (converted once with the default hint).
        static BOOL Equals(JavascriptBigInt* left, Var right, BOOL* value, ScriptContext * requestContext);
        // Exact mixed comparison used by the relational operators. The double
        // form returns false for NaN; otherwise *result is the mathematical
        // sign of this - value (a fractional value compares against floor).
        bool CompareToDouble(double value, int* result) const;
        int CompareToUInt64(bool valueIsNegative, uint64 value) const;
        int CompareTo(const JavascriptBigInt* other) const;
        static Var Increment(Var aRight);
        static Var Add(Var aLeft, Var aRight);
        static Var Sub(Var aLeft, Var aRight);
        static Var Mul(Var aLeft, Var aRight);
        static Var Decrement(Var aRight);
        static Var Not(Var aRight);
        static Var Negate(Var aRight);
        // Multiplicative, exponentiation, shift and bitwise operators. The
        // caller has already applied ToPrimitive(hint Number) to both
        // operands and rejected mixed BigInt/Number pairs, so these entry
        // points only see two BigInt primitives.
        static Var Div(Var aLeft, Var aRight);
        static Var Modulus(Var aLeft, Var aRight);
        static Var Exponentiate(Var aLeft, Var aRight);
        static Var LeftShift(Var aLeft, Var aRight);
        static Var RightShift(Var aLeft, Var aRight);
        static Var BitwiseAnd(Var aLeft, Var aRight);
        static Var BitwiseOr(Var aLeft, Var aRight);
        static Var BitwiseXor(Var aLeft, Var aRight);

        inline BOOL isNegative() { return m_isNegative; }

        // Canonical zero is one zero limb; a zero-length intermediate is also
        // treated as zero so callers never index past the digit array.
        bool IsZeroValue() const { return m_length == 0 || (m_length == 1 && m_digits[0] == 0); }

        static JavascriptBigInt * CreateZero(ScriptContext * scriptContext);
        static JavascriptBigInt * CreateZeroWithLength(digit_t length, ScriptContext * scriptContext);
        static JavascriptBigInt * CreateOne(ScriptContext * scriptContext);
        static JavascriptBigInt * Create(const char16 * content, charcount_t cchUseLength, bool isNegative, ScriptContext * scriptContext);
        // Conversion factories backing the BigInt constructor and ToBigInt.
        // Each returns nullptr when the input has no BigInt representation so
        // the caller owns the error class: CreateFromNumber rejects NaN,
        // +/-Infinity and non-integral values; CreateFromString rejects
        // everything outside the StringToBigInt grammar (decimal, or an
        // unsigned 0x/0o/0b form). ToBigInt(Number) throws before reaching
        // this factory; only the constructor's NumberToBigInt step uses it.
        static JavascriptBigInt * CreateFromNumber(double value, ScriptContext * scriptContext);
        static JavascriptBigInt * CreateFromString(const char16 * content, charcount_t cchContent, ScriptContext * scriptContext);
        // Exact 64-bit factories for BigInt64Array/BigUint64Array element
        // reads. They avoid routing through a double, so every bit pattern
        // (including INT64_MIN and values above 2**53) is exact.
        static JavascriptBigInt * CreateFromInt64(int64 value, ScriptContext * scriptContext);
        static JavascriptBigInt * CreateFromUInt64(uint64 value, ScriptContext * scriptContext);
        // Low 64 bits of the two's-complement representation (ToBigInt64 /
        // ToBigUint64). Negative values wrap modulo 2**64.
        uint64 ToUInt64Low() const;
        // Value hash for Map/Set/SameValueZero dictionaries. Equal canonical
        // BigInts hash identically regardless of object identity.
        hash_t GetHashCode() const;
        virtual RecyclableObject * CloneToScriptContext(ScriptContext* requestContext) override;
        virtual RecyclableObject* ToObject(ScriptContext * requestContext) override;
        virtual Var GetTypeOfString(ScriptContext * requestContext) override;

        JavascriptString* ToString(ScriptContext * requestContext);
        JavascriptString* ToString(uint32 radix, ScriptContext * requestContext);
        double ToDouble() const;
        static JavascriptBigInt * ThisBigIntValue(Var value, ScriptContext * scriptContext);

        // BigInt.asIntN / BigInt.asUintN (ES2020). `bits` has already passed
        // ToIndex; the result is a new value unless the input is already in
        // range and immutable (positive asUintN, either sign asIntN).
        static JavascriptBigInt * AsIntN(JavascriptBigInt * value, uint64 bits, ScriptContext * scriptContext);
        static JavascriptBigInt * AsUintN(JavascriptBigInt * value, uint64 bits, ScriptContext * scriptContext);

        class EntryInfo
        {
        public:
            static FunctionInfo NewInstance;
            static FunctionInfo AsIntN;
            static FunctionInfo AsUintN;
            static FunctionInfo ValueOf;
            static FunctionInfo ToString;
            static FunctionInfo ToLocaleString;
        };

        static Var NewInstance(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryAsIntN(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryAsUintN(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryValueOf(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryToString(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryToLocaleString(RecyclableObject* function, CallInfo callInfo, ...);

        virtual BOOL Equals(Var other, BOOL* value, ScriptContext * requestContext) override;
        virtual BOOL GetDiagValueString(StringBuilder<ArenaAllocator>* stringBuilder, ScriptContext* requestContext) override;
        virtual BOOL GetDiagTypeString(StringBuilder<ArenaAllocator>* stringBuilder, ScriptContext* requestContext) override;

        static digit_t AddDigit(digit_t a, digit_t b, digit_t * carry);
        static digit_t SubDigit(digit_t a, digit_t b, digit_t * borrow);
        static digit_t MulDigit(digit_t a, digit_t b, digit_t * high);

    private:
        template <typename EncodedChar>
        void InitFromCharDigits(const EncodedChar *prgch, uint32 cch, bool isNegative); // init from char of digits

        void MulThenAdd(digit_t luMul, digit_t luAdd);
        static uint32 DivModInPlace(digit_t * digits, digit_t& length, uint32 divisor);
        static bool IsZero(JavascriptBigInt * pbi);
        static void AbsoluteIncrement(JavascriptBigInt * pbi);
        static void AbsoluteDecrement(JavascriptBigInt * pbi);
        static void Increment(JavascriptBigInt * pbi);
        static void Decrement(JavascriptBigInt * pbi);
        static void Negate(JavascriptBigInt * pbi);
        static JavascriptBigInt * Add(JavascriptBigInt * pbi1, JavascriptBigInt * pbi2);
        static void AddAbsolute(JavascriptBigInt * pbi1, JavascriptBigInt * pbi2);
        static JavascriptBigInt * Sub(JavascriptBigInt * pbi1, JavascriptBigInt * pbi2);
        static void SubAbsolute(JavascriptBigInt * pbi1, JavascriptBigInt * pbi2);
        static JavascriptBigInt * Mul(JavascriptBigInt * pbi1, JavascriptBigInt * pbi2);
        static JavascriptBigInt * MulAbsolute(JavascriptBigInt * pbi1, JavascriptBigInt * pbi2);
        static JavascriptBigInt * DivMod(JavascriptBigInt * pbi1, JavascriptBigInt * pbi2, bool wantQuotient);
        static JavascriptBigInt * Exponentiate(JavascriptBigInt * pbiBase, JavascriptBigInt * pbiExponent);
        static JavascriptBigInt * ShiftLeftMagnitude(const JavascriptBigInt * pbi, uint64 bits);
        static JavascriptBigInt * ShiftRightMagnitudeFloor(const JavascriptBigInt * pbi, uint64 bits);
        static JavascriptBigInt * BitwiseOperation(const JavascriptBigInt * pbiLeft, const JavascriptBigInt * pbiRight, uint32 operation);
        static JavascriptBigInt * CreateFromUnits(const uint32 * units, uint32 unitCount, bool isNegative, ScriptContext * scriptContext);
        static uint64 GetMagnitudeAsUInt64(const JavascriptBigInt * pbi, bool* tooLarge);
        int Compare(JavascriptBigInt * pbi) const;
        int CompareAbsolute(JavascriptBigInt * pbi) const;
        int CompareMagnitudeToUInt64(uint64 value) const;
        void Resize(digit_t length);

        static uint64 ToIndex(Var value, ScriptContext * scriptContext);
        uint64 GetMagnitudeBitLength() const;
        bool GetMagnitudeBit(uint64 bit) const;
        bool HasSingleMagnitudeBit(uint64 bit) const;
        // Low `bits` bits of the magnitude as a canonical positive BigInt.
        JavascriptBigInt * TruncateMagnitude(uint64 bits, ScriptContext * scriptContext) const;
        // 2**bits - magnitude, for a non-zero magnitude < 2**bits.
        static JavascriptBigInt * TwosComplement(const JavascriptBigInt * magnitude, uint64 bits, ScriptContext * scriptContext);
        static void MakeNegative(JavascriptBigInt * value);

        static JavascriptBigInt * New(JavascriptBigInt * pbi, ScriptContext * scriptContext);
    };
    
    template <> inline bool VarIsImpl<JavascriptBigInt>(RecyclableObject* obj)
    {
        return JavascriptOperators::GetTypeId(obj) == TypeIds_BigInt;
    }
}
