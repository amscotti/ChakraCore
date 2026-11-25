//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#pragma once

#include "Float16.h"

namespace Js
{
    namespace DataViewDetail
    {
        template<size_t Size>
        struct UnsignedStorage;

        template<>
        struct UnsignedStorage<1>
        {
            using Type = uint8;
            static Type Swap(Type value) { return value; }
        };

        template<>
        struct UnsignedStorage<2>
        {
            using Type = uint16;
            static Type Swap(Type value) { return RtlUshortByteSwap(value); }
        };

        template<>
        struct UnsignedStorage<4>
        {
            using Type = uint32;
            static Type Swap(Type value) { return RtlUlongByteSwap(value); }
        };

        template<>
        struct UnsignedStorage<8>
        {
            using Type = uint64;
            static Type Swap(Type value) { return RtlUlonglongByteSwap(value); }
        };
    }

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
        // ToIndex(requestIndex) is performed before the detached/range checks
        // (test262 getFloat16/setFloat16 detached-buffer ordering), so these
        // helpers are shared by the templates and the Float16 entries.
        uint32 GetViewIndex(Var offset, const char16* funcName);
        uint32 GetWritableViewIndex(Var offset, const char16* funcName);
        void CheckViewRange(uint32 byteOffset, uint32 elementSize, const char16* funcName);

        template<typename TypeName>
        TypeName ReadViewBytesAt(uint32 byteOffset, BOOL isLittleEndian, const char16* funcName)
        {
            CheckViewRange(byteOffset, sizeof(TypeName), funcName);

            using Storage = DataViewDetail::UnsignedStorage<sizeof(TypeName)>;
            typename Storage::Type bits = 0;
            memcpy(&bits, this->buffer + byteOffset, sizeof(TypeName));
            if (!isLittleEndian)
            {
                bits = Storage::Swap(bits);
            }
            TypeName item;
            memcpy(&item, &bits, sizeof(TypeName));
            return item;
        }

        template<typename TypeName>
        Var GetValueAt(uint32 byteOffset, const char16* funcName, BOOL isLittleEndian = FALSE)
        {
            TypeName item = ReadViewBytesAt<TypeName>(byteOffset, isLittleEndian, funcName);
            return JavascriptNumber::ToVar(item, GetScriptContext());
        }

        template<typename TypeName>
        inline Var GetValueAtWithCheck(uint32 byteOffset, const char16* funcName, BOOL isLittleEndian = FALSE)
        {
            TypeName item = ReadViewBytesAt<TypeName>(byteOffset, isLittleEndian, funcName);
            return JavascriptNumber::ToVarWithCheck(item, GetScriptContext());
        }

        // SetViewValue performs ToIndex(requestIndex) before converting the
        // value, so setters convert the offset first (GetWritableViewIndex) and
        // write through SetValueAt with the resolved byte offset.
        template<typename TypeName>
        inline void SetValueAt(uint32 byteOffset, TypeName value, const char16 *funcName, BOOL isLittleEndian = FALSE)
        {
            CheckViewRange(byteOffset, sizeof(TypeName), funcName);

            using Storage = DataViewDetail::UnsignedStorage<sizeof(TypeName)>;
            typename Storage::Type bits = 0;
            memcpy(&bits, &value, sizeof(TypeName));
            if (!isLittleEndian)
            {
                bits = Storage::Swap(bits);
            }
            memcpy(this->buffer + byteOffset, &bits, sizeof(TypeName));
        }

        template<typename TypeName, Var (DataView::*GetFn)(uint32, const char16*, BOOL)>
        static Var EntryGetValue(ScriptContext* scriptContext, const Arguments& args, const char16* funcName);

        template<typename TypeName, TypeName (*ConvertFn)(Var, ScriptContext*)>
        static Var EntrySetValue(ScriptContext* scriptContext, const Arguments& args, const char16* funcName);

        Field(uint32) byteOffset;
        Field(BYTE*) buffer;   // beginning of buffer
        Field(bool) lengthTracking; // No explicit byteLength: length follows the buffer

    };

    template <> inline bool VarIsImpl<DataView>(RecyclableObject* obj)
    {
        return JavascriptOperators::GetTypeId(obj) == TypeIds_DataView;
    }
}
