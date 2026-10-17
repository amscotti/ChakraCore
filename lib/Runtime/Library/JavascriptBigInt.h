//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
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
        static Var Increment(Var aRight);
        static Var Add(Var aLeft, Var aRight);
        static Var Sub(Var aLeft, Var aRight);
        static Var Mul(Var aLeft, Var aRight);
        static Var Decrement(Var aRight);
        static Var Not(Var aRight);
        static Var Negate(Var aRight);

        inline BOOL isNegative() { return m_isNegative; }

        // Canonical zero is one zero limb; a zero-length intermediate is also
        // treated as zero so callers never index past the digit array.
        bool IsZeroValue() const { return m_length == 0 || (m_length == 1 && m_digits[0] == 0); }

        static JavascriptBigInt * CreateZero(ScriptContext * scriptContext);
        static JavascriptBigInt * CreateZeroWithLength(digit_t length, ScriptContext * scriptContext);
        static JavascriptBigInt * CreateOne(ScriptContext * scriptContext);
        static JavascriptBigInt * Create(const char16 * content, charcount_t cchUseLength, bool isNegative, ScriptContext * scriptContext);
        // Conversion factories backing ToBigInt. Each returns nullptr when the
        // input has no BigInt representation so the caller owns the error class:
        // CreateFromNumber rejects NaN, +/-Infinity and non-integral values;
        // CreateFromString rejects everything outside the StringToBigInt
        // grammar (decimal, or an unsigned 0x/0o/0b form).
        static JavascriptBigInt * CreateFromNumber(double value, ScriptContext * scriptContext);
        static JavascriptBigInt * CreateFromString(const char16 * content, charcount_t cchContent, ScriptContext * scriptContext);
        virtual RecyclableObject * CloneToScriptContext(ScriptContext* requestContext) override;
        virtual RecyclableObject* ToObject(ScriptContext * requestContext) override;
        virtual Var GetTypeOfString(ScriptContext * requestContext) override;

        JavascriptString* ToString(ScriptContext * requestContext);
        JavascriptString* ToString(uint32 radix, ScriptContext * requestContext);
        double ToDouble() const;
        static JavascriptBigInt * ThisBigIntValue(Var value, ScriptContext * scriptContext);

        class EntryInfo
        {
        public:
            static FunctionInfo NewInstance;
            static FunctionInfo ValueOf;
            static FunctionInfo ToString;
            static FunctionInfo ToLocaleString;
        };

        static Var NewInstance(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryValueOf(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryToString(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryToLocaleString(RecyclableObject* function, CallInfo callInfo, ...);

        virtual BOOL Equals(Var other, BOOL* value, ScriptContext * requestContext) override;

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
        int Compare(JavascriptBigInt * pbi);
        int CompareAbsolute(JavascriptBigInt * pbi);
        static BOOL Equals(JavascriptBigInt* left, Var right, BOOL* value, ScriptContext * requestContext);
        void Resize(digit_t length);

        static JavascriptBigInt * New(JavascriptBigInt * pbi, ScriptContext * scriptContext);
    };
    
    template <> inline bool VarIsImpl<JavascriptBigInt>(RecyclableObject* obj)
    {
        return JavascriptOperators::GetTypeId(obj) == TypeIds_BigInt;
    }
}
