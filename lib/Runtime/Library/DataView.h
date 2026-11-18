//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#pragma once

#include "Float16.h"

namespace Js
{
    class DataView : public ArrayBufferParent
    {
        friend ArrayBuffer;
    protected:
        DEFINE_VTABLE_CTOR(DataView, ArrayBufferParent);
        DEFINE_MARSHAL_OBJECT_TO_SCRIPT_CONTEXT(DataView);
    public:
        class EntryInfo
        {
        public:
            static FunctionInfo NewInstance;
            static FunctionInfo GetInt8;
            static FunctionInfo GetUint8;
            static FunctionInfo GetInt16;
            static FunctionInfo GetUint16;
            static FunctionInfo GetInt32;
            static FunctionInfo GetUint32;
            static FunctionInfo GetFloat32;
            static FunctionInfo GetFloat64;
            static FunctionInfo GetFloat16;
            static FunctionInfo GetBigInt64;
            static FunctionInfo GetBigUint64;
            static FunctionInfo SetInt8;
            static FunctionInfo SetUint8;
            static FunctionInfo SetInt16;
            static FunctionInfo SetUint16;
            static FunctionInfo SetInt32;
            static FunctionInfo SetUint32;
            static FunctionInfo SetFloat32;
            static FunctionInfo SetFloat64;
            static FunctionInfo SetFloat16;
            static FunctionInfo SetBigInt64;
            static FunctionInfo SetBigUint64;

            static FunctionInfo GetterBuffer;
            static FunctionInfo GetterByteLength;
            static FunctionInfo GetterByteOffset;
        };

        DataView(ArrayBufferBase* arrayBuffer, uint32 byteOffset, uint32 mappedLength, bool lengthTracking, DynamicType* type);

        uint32 GetByteOffset() const { return byteOffset; }
        bool IsLengthTracking() const { return lengthTracking; }
        bool IsOutOfBounds(uint32 bufferByteLength) const;
        uint32 GetEffectiveByteLength(uint32 bufferByteLength) const;
        void ClearLengthAndBufferOnDetach();

        static Var NewInstance(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryGetInt8(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryGetUint8(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryGetInt16(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryGetUint16(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryGetInt32(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryGetUint32(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryGetFloat32(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryGetFloat64(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryGetFloat16(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryGetBigInt64(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryGetBigUint64(RecyclableObject* function, CallInfo callInfo, ...);

        static Var EntrySetInt8(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntrySetUint8(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntrySetInt16(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntrySetUint16(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntrySetInt32(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntrySetUint32(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntrySetFloat32(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntrySetFloat64(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntrySetFloat16(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntrySetBigInt64(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntrySetBigUint64(RecyclableObject* function, CallInfo callInfo, ...);

        static Var EntryGetterBuffer(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryGetterByteLength(RecyclableObject* function, CallInfo callInfo, ...);
        static Var EntryGetterByteOffset(RecyclableObject* function, CallInfo callInfo, ...);

        // objectArray support
        virtual BOOL SetItemWithAttributes(uint32 index, Var value, PropertyAttributes attributes) override;
        virtual JavascriptEnumerator * GetIndexEnumerator(EnumeratorFlags flags, ScriptContext * requestContext) override
        {
            // Data View can not be an objectArray
            Assert(false);
            return nullptr;
        }
    private:
        template<typename TypeName>
        void SwapRoutine(TypeName* input, TypeName* dest);

        template<> void SwapRoutine(int8* input, int8* dest) {*dest =  *input; }
        template<> void SwapRoutine(uint8* input, uint8* dest) {*dest =  *input; }
        template<> void SwapRoutine(int16* input, int16* dest) {*dest =  RtlUshortByteSwap(*input); }
        template<> void SwapRoutine(uint16* input, uint16* dest) {*dest =  RtlUshortByteSwap(*input);}
        template<> void SwapRoutine(int32* input, int32* dest) {*dest =  RtlUlongByteSwap(*input);}
        template<> void SwapRoutine(uint32* input, uint32* dest) {*dest =  RtlUlongByteSwap(*input);}
        // we don't want type conversion here, we just want to swap the bytes.
        template<> void SwapRoutine(float* input, float* dest) { *((uint32*)dest) = RtlUlongByteSwap(*((uint32*)input)); }
        template<> void SwapRoutine(double* input, double* dest) {*((uint64*)dest) = RtlUlonglongByteSwap(*((uint64*)input)); }

        // ToIndex(requestIndex) is performed before the detached/range checks
        // (test262 getFloat16/setFloat16 detached-buffer ordering), so these
        // helpers are shared by the templates and the Float16 entries.
        uint32 GetViewIndex(Var offset, const char16* funcName);
        uint32 GetWritableViewIndex(Var offset, const char16* funcName);
        void CheckViewRange(uint32 byteOffset, uint32 elementSize, const char16* funcName);

        template<typename TypeName, typename PointerAccessTypeName>
        TypeName ReadViewBytesAt(uint32 byteOffset, BOOL isLittleEndian, const char16* funcName)
        {
            CheckViewRange(byteOffset, sizeof(TypeName), funcName);

            TypeName item;
            TypeName* typedBuffer = (TypeName*)(buffer + byteOffset);
            if (!isLittleEndian)
            {
                SwapRoutine<TypeName>(typedBuffer, &item);
            }
            else
            {
                item = *static_cast<PointerAccessTypeName>(typedBuffer);
            }
            return item;
        }

        template<typename TypeName>
        Var GetValueAt(uint32 byteOffset, const char16* funcName, BOOL isLittleEndian = FALSE)
        {
            return GetValueAtImpl<TypeName, TypeName*>(byteOffset, isLittleEndian, funcName);
        }

        template<typename TypeName, typename PointerAccessTypeName>
        Var GetValueAtImpl(uint32 byteOffset, BOOL isLittleEndian, const char16* funcName)
        {
            TypeName item = ReadViewBytesAt<TypeName, PointerAccessTypeName>(byteOffset, isLittleEndian, funcName);
            return JavascriptNumber::ToVar(item, GetScriptContext());
        }

        template<typename TypeName>
        inline Var GetValueAtWithCheck(uint32 byteOffset, const char16* funcName, BOOL isLittleEndian = FALSE)
        {
            return GetValueAtWithCheckImpl<TypeName, TypeName*>(byteOffset, isLittleEndian, funcName);
        }

        template<typename TypeName, typename PointerAccessTypeName>
        Var GetValueAtWithCheckImpl(uint32 byteOffset, BOOL isLittleEndian, const char16* funcName)
        {
            TypeName item = ReadViewBytesAt<TypeName, PointerAccessTypeName>(byteOffset, isLittleEndian, funcName);
            return JavascriptNumber::ToVarWithCheck(item, GetScriptContext());
        }

        // SetViewValue performs ToIndex(requestIndex) before converting the
        // value, so setters convert the offset first (GetWritableViewIndex) and
        // write through SetValueAt with the resolved byte offset.
        template<typename TypeName>
        inline void SetValueAt(uint32 byteOffset, TypeName value, const char16 *funcName, BOOL isLittleEndian = FALSE)
        {
            SetValueAtImpl<TypeName, TypeName*>(byteOffset, value, isLittleEndian, funcName);
        }

        template<typename TypeName, typename PointerAccessTypeName>
        void SetValueAtImpl(uint32 byteOffset, TypeName value, BOOL isLittleEndian, const char16 *funcName)
        {
            CheckViewRange(byteOffset, sizeof(TypeName), funcName);

            TypeName* typedBuffer = (TypeName*)(buffer + byteOffset);
            if (!isLittleEndian)
            {
                SwapRoutine<TypeName>(&value, typedBuffer);
            }
            else
            {
                *static_cast<PointerAccessTypeName>(typedBuffer) = value;
            }
        }

        template<typename TypeName, Var (DataView::*GetFn)(uint32, const char16*, BOOL)>
        static Var EntryGetValue(ScriptContext* scriptContext, const Arguments& args, const char16* funcName);

        template<typename TypeName, TypeName (*ConvertFn)(Var, ScriptContext*)>
        static Var EntrySetValue(ScriptContext* scriptContext, const Arguments& args, const char16* funcName);

#ifdef _M_ARM
        // For ARM, memory access for float/double address causes data alignment exception if the address is not aligned.
        // Provide template specialization (only) for these scenarios.
        template<> Var GetValueAtWithCheck<float>(uint32 byteOffset, const char16 *funcName, BOOL isLittleEndian /* = FALSE */);
        template<> Var GetValueAtWithCheck<double>(uint32 byteOffset, const char16 *funcName, BOOL isLittleEndian /* = FALSE */);
        template<> void SetValueAt<float>(uint32 byteOffset, float value, const char16 *funcName, BOOL isLittleEndian /* = FALSE */);
        template<> void SetValueAt<double>(uint32 byteOffset, double value, const char16 *funcName, BOOL isLittleEndian /* = FALSE */);
#endif

        Field(uint32) byteOffset;
        Field(BYTE*) buffer;   // beginning of buffer
        Field(bool) lengthTracking; // No explicit byteLength: length follows the buffer

    };

    template <> inline bool VarIsImpl<DataView>(RecyclableObject* obj)
    {
        return JavascriptOperators::GetTypeId(obj) == TypeIds_DataView;
    }
}
