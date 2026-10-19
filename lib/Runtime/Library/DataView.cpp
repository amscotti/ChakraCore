//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
//  Implements typed array.
//----------------------------------------------------------------------------
#include "RuntimeLibraryPch.h"

namespace Js
{

    namespace
    {
        // Reinterpret the low 64 bits as a signed integer without relying on
        // an implementation-defined uint64-to-int64 conversion.
        inline int64 DataViewBigInt64Bits(uint64 bits)
        {
            const uint64 int64Max = 0x7fffffffffffffffULL;
            if (bits <= int64Max)
            {
                return static_cast<int64>(bits);
            }
            const int64 int64Min = -0x7fffffffffffffffLL - 1;
            return static_cast<int64>(bits - (int64Max + 1)) + int64Min;
        }

        // Byte-wise assembly keeps DataView BigInt accesses independent of
        // host alignment and endianness.
        inline uint64 DataViewReadUInt64(const BYTE* bytes, bool littleEndian)
        {
            uint64 value = 0;
            if (littleEndian)
            {
                for (uint32 i = 0; i < 8; i++)
                {
                    value |= static_cast<uint64>(bytes[i]) << (i * 8);
                }
            }
            else
            {
                for (uint32 i = 0; i < 8; i++)
                {
                    value = (value << 8) | bytes[i];
                }
            }
            return value;
        }

        inline void DataViewWriteUInt64(BYTE* bytes, uint64 value, bool littleEndian)
        {
            if (littleEndian)
            {
                for (uint32 i = 0; i < 8; i++)
                {
                    bytes[i] = static_cast<BYTE>(value >> (i * 8));
                }
            }
            else
            {
                for (uint32 i = 0; i < 8; i++)
                {
                    bytes[i] = static_cast<BYTE>(value >> ((7 - i) * 8));
                }
            }
        }
    }

    Var DataView::NewInstance(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();

        AssertMsg(args.Info.Count > 0, "Should always have implicit 'this'");

        Var newTarget = args.GetNewTarget();
        bool isCtorSuperCall = JavascriptOperators::GetAndAssertIsConstructorSuperCall(args);
        uint32 byteLength = 0;
        uint32 mappedLength;
        int32 offset = 0;
        ArrayBufferBase* arrayBuffer = nullptr;
        DataView* dataView;
        bool lengthTracking = !(args.Info.Count > 3 && !JavascriptOperators::IsUndefinedObject(args[3]));

        //1.    If NewTarget is undefined, throw a TypeError exception.
        if (!(callInfo.Flags & CallFlags_New) || (newTarget && JavascriptOperators::IsUndefinedObject(newTarget)))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_ClassConstructorCannotBeCalledWithoutNew, _u("DataView"));
        }

        if (args.Info.Count < 2)
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_DataView_NeedArgument, _u("buffer"));
        }

        //2.    If Type(buffer) is not Object, throw a TypeError exception.
        //3.    If buffer does not have an [[ArrayBufferData]] internal slot, throw a TypeError exception.
        if (arrayBuffer == nullptr)
        {
            if (VarIs<ArrayBufferBase>(args[1]))
            {
                arrayBuffer = VarTo<ArrayBufferBase>(args[1]);
            }
            else
            {
                JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedArrayBufferObject, _u("buffer"));
            }
        }

        //4.    Let offset be ToIndex(byteOffset).
        if (args.Info.Count > 2)
        {
            Var secondArgument = args[2];
            offset = ArrayBuffer::ToIndex(secondArgument, JSERR_ArrayLengthConstructIncorrect, scriptContext, ArrayBuffer::MaxArrayBufferLength, false);
        }

        //5.    If IsDetachedBuffer(buffer) is true, throw a TypeError exception.
        if (arrayBuffer->IsDetached())
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_DetachedTypedArray);
        }

        //6.    Let bufferByteLength be the value of buffer's[[ArrayBufferByteLength]] internal slot.
        //7.   If offset > bufferByteLength, throw a RangeError exception.
        byteLength = arrayBuffer->GetByteLength();
        if ((uint32)offset > byteLength)
        {
            JavascriptError::ThrowRangeError(
                scriptContext, JSERR_DataView_InvalidArgument, _u("byteOffset"));
        }

        //8.   If byteLength is either not present or is undefined, then
        //      a.  Let viewByteLength be bufferByteLength - offset.
        //9.   Else,
        //      a.  Let viewByteLength be ToIndex(byteLength).
        //      b.  If offset + viewByteLength > bufferByteLength, throw a RangeError exception.
        if (args.Info.Count > 3 && !JavascriptOperators::IsUndefinedObject(args[3]))
        {
            Var thirdArgument = args[3];
            mappedLength = ArrayBuffer::ToIndex(thirdArgument, JSERR_ArrayLengthConstructIncorrect, scriptContext, ArrayBuffer::MaxArrayBufferLength, false);
            uint32 viewRange = mappedLength + offset;

            if (viewRange > byteLength || viewRange < mappedLength) // overflow indicates out-of-range
            {
                JavascriptError::ThrowRangeError(
                    scriptContext, JSERR_DataView_InvalidArgument, _u("byteLength"));
            }
        }
        else
        {
            mappedLength = byteLength - offset;
        }

        // Evaluation of the argument(s) above is reentrant and can detach or
        // resize the buffer. Re-read its current length after ToIndex(byteLength)
        // and never create an out-of-bounds view.
        if (arrayBuffer->IsDetached())
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_DetachedTypedArray);
        }

        byteLength = arrayBuffer->GetByteLength();
        if ((uint32)offset > byteLength)
        {
            JavascriptError::ThrowRangeError(
                scriptContext, JSERR_DataView_InvalidArgument, _u("byteOffset"));
        }
        if (lengthTracking)
        {
            mappedLength = byteLength - offset;
        }
        else if (mappedLength > byteLength - (uint32)offset)
        {
            JavascriptError::ThrowRangeError(
                scriptContext, JSERR_DataView_InvalidArgument, _u("byteLength"));
        }

        //10.   Let O be OrdinaryCreateFromConstructor(NewTarget, "%DataViewPrototype%", [[DataView]], [[ViewedArrayBuffer]], [[ByteLength]], [[ByteOffset]]).
        //11.   Set O's[[DataView]] internal slot to true.
        //12.   Set O's[[ViewedArrayBuffer]] internal slot to buffer.
        //13.   Set O's[[ByteLength]] internal slot to viewByteLength.
        //14.   Set O's[[ByteOffset]] internal slot to offset.
        //15.   Return O.
        dataView = scriptContext->GetLibrary()->CreateDataView(arrayBuffer, offset, mappedLength, lengthTracking);
        Var result = isCtorSuperCall ?
            JavascriptOperators::OrdinaryCreateFromConstructor(VarTo<RecyclableObject>(newTarget), dataView, nullptr, scriptContext) :
            dataView;

        // OrdinaryCreateFromConstructor runs newTarget.prototype user code after
        // the view was built. Revalidate once more so an already-OOB view is
        // never returned.
        if (arrayBuffer->IsDetached())
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_DetachedTypedArray);
        }
        uint32 finalByteLength = arrayBuffer->GetByteLength();
        if ((uint32)offset > finalByteLength)
        {
            JavascriptError::ThrowRangeError(
                scriptContext, JSERR_DataView_InvalidArgument, _u("byteOffset"));
        }
        if (!lengthTracking && mappedLength > finalByteLength - (uint32)offset)
        {
            JavascriptError::ThrowRangeError(
                scriptContext, JSERR_DataView_InvalidArgument, _u("byteLength"));
        }

        return result;
    }

    DataView::DataView(ArrayBufferBase* arrayBuffer, uint32 byteoffset, uint32 mappedLength, bool lengthTracking, DynamicType* type)
        : ArrayBufferParent(type, mappedLength, arrayBuffer),
          byteOffset(byteoffset),
          lengthTracking(lengthTracking)
    {
        buffer = arrayBuffer->GetBuffer() + byteoffset;
    }

    bool DataView::IsOutOfBounds(uint32 bufferByteLength) const
    {
        uint32 offset = this->GetByteOffset();
        if (offset > bufferByteLength)
        {
            return true;
        }
        // An auto-length view tracks the buffer; a fixed-length view is OOB
        // once its captured range no longer fits.
        return !this->lengthTracking && this->GetLength() > bufferByteLength - offset;
    }

    uint32 DataView::GetEffectiveByteLength(uint32 bufferByteLength) const
    {
        if (!this->lengthTracking)
        {
            return this->GetLength();
        }
        uint32 offset = this->GetByteOffset();
        return offset < bufferByteLength ? bufferByteLength - offset : 0;
    }

    uint32 DataView::GetViewIndex(Var offset, const char16* funcName)
    {
        return ArrayBuffer::ToIndex(offset, JSERR_DataView_InvalidOffset, GetScriptContext(), ArrayBuffer::MaxArrayBufferLength, false);
    }

    void DataView::CheckViewRange(uint32 byteOffset, uint32 elementSize, const char16* funcName)
    {
        ScriptContext* scriptContext = GetScriptContext();
        if (this->GetArrayBuffer()->IsDetached())
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_DetachedTypedArray, funcName);
        }

        // A DataView on a resizable buffer becomes out of bounds when the
        // buffer shrinks below a fixed view's range, or below an auto-length
        // view's offset. Web-reality semantics throw TypeError, before the
        // in-view RangeError check below.
        uint32 bufferByteLength = this->GetArrayBuffer()->GetByteLength();
        if (this->IsOutOfBounds(bufferByteLength))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_DetachedTypedArray, funcName);
        }

        uint32 length = this->GetEffectiveByteLength(bufferByteLength);
        if (length < elementSize || byteOffset > length - elementSize)
        {
            JavascriptError::ThrowRangeError(scriptContext, JSERR_DataView_InvalidOffset, funcName);
        }
    }

    Var DataView::EntryGetInt8(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();

        Assert(!(callInfo.Flags & CallFlags_New));

        if (args.Info.Count == 0 || !VarIs<DataView>(args[0]))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_This_NeedDataView);
        }
        if (args.Info.Count < 2)
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_DataView_NeedArgument, _u("offset"));
        }

        DataView* dataView = VarTo<DataView>(args[0]);
        return dataView->template GetValue<int8>(args[1], _u("DataView.prototype.GetInt8"), FALSE);
    }

    Var DataView::EntryGetUint8(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();

        Assert(!(callInfo.Flags & CallFlags_New));

        if (args.Info.Count == 0 || !VarIs<DataView>(args[0]))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_This_NeedDataView);
        }
        if (args.Info.Count < 2)
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_DataView_NeedArgument, _u("offset"));
        }

        DataView* dataView = VarTo<DataView>(args[0]);
        return dataView->GetValue<uint8>(args[1], _u("DataView.prototype.GetUint8"), FALSE);
    }

    Var DataView::EntryGetInt16(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();
        BOOL isLittleEndian = FALSE;

        Assert(!(callInfo.Flags & CallFlags_New));

        if (args.Info.Count == 0 || !VarIs<DataView>(args[0]))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_This_NeedDataView);
        }
        if (args.Info.Count < 2)
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_DataView_NeedArgument, _u("offset"));
        }
        if (args.Info.Count > 2)
        {
            isLittleEndian = JavascriptConversion::ToBoolean(args[2], scriptContext);
        }

        DataView* dataView = VarTo<DataView>(args[0]);
        return dataView->GetValue<int16>(args[1], _u("DataView.prototype.GetInt16"), isLittleEndian);
    }

    Var DataView::EntryGetUint16(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();
        BOOL isLittleEndian = FALSE;

        Assert(!(callInfo.Flags & CallFlags_New));

        if (args.Info.Count == 0 || !VarIs<DataView>(args[0]))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_This_NeedDataView);
        }
        if (args.Info.Count < 2)
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_DataView_NeedArgument, _u("offset"));
        }
        if (args.Info.Count > 2)
        {
            isLittleEndian = JavascriptConversion::ToBoolean(args[2], scriptContext);
        }

        DataView* dataView = VarTo<DataView>(args[0]);
        return dataView->template GetValue<uint16>(args[1], _u("DataView.prototype.GetUint16"), isLittleEndian);
    }

    Var DataView::EntryGetUint32(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();
        BOOL isLittleEndian = FALSE;

        Assert(!(callInfo.Flags & CallFlags_New));

        if (args.Info.Count == 0 || !VarIs<DataView>(args[0]))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_This_NeedDataView);
        }
        if (args.Info.Count < 2)
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_DataView_NeedArgument, _u("offset"));
        }
        if (args.Info.Count > 2)
        {
            isLittleEndian = JavascriptConversion::ToBoolean(args[2], scriptContext);
        }

        DataView* dataView = VarTo<DataView>(args[0]);
        return dataView->GetValue<uint32>(args[1], _u("DataView.prototype.GetUint32"), isLittleEndian);
    }

    Var DataView::EntryGetInt32(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();
        BOOL isLittleEndian = FALSE;

        Assert(!(callInfo.Flags & CallFlags_New));

        if (args.Info.Count == 0 || !VarIs<DataView>(args[0]))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_This_NeedDataView);
        }
        if (args.Info.Count < 2)
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_DataView_NeedArgument, _u("offset"));
        }
        if (args.Info.Count > 2)
        {
            isLittleEndian = JavascriptConversion::ToBoolean(args[2], scriptContext);
        }

        DataView* dataView = VarTo<DataView>(args[0]);
        return dataView->GetValue<int32>(args[1], _u("DataView.prototype.GetInt32"), isLittleEndian);
    }

    Var DataView::EntryGetFloat32(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();
        BOOL isLittleEndian = FALSE;

        Assert(!(callInfo.Flags & CallFlags_New));

        if (args.Info.Count == 0 || !VarIs<DataView>(args[0]))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_This_NeedDataView);
        }
        if (args.Info.Count < 2)
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_DataView_NeedArgument, _u("offset"));
        }
        if (args.Info.Count > 2)
        {
            isLittleEndian = JavascriptConversion::ToBoolean(args[2], scriptContext);
        }

        DataView* dataView = VarTo<DataView>(args[0]);
        return dataView->GetValueWithCheck<float>(args[1], _u("DataView.prototype.GetFloat32"), isLittleEndian);
    }

    Var DataView::EntryGetFloat64(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();
        BOOL isLittleEndian = FALSE;

        Assert(!(callInfo.Flags & CallFlags_New));

        if (args.Info.Count == 0 || !VarIs<DataView>(args[0]))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_This_NeedDataView);
        }
        if (args.Info.Count < 2)
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_DataView_NeedArgument, _u("offset"));
        }
        if (args.Info.Count > 2)
        {
            isLittleEndian = JavascriptConversion::ToBoolean(args[2], scriptContext);
        }

        DataView* dataView = VarTo<DataView>(args[0]);
        return dataView->GetValueWithCheck<double>(args[1], _u("DataView.prototype.GetFloat64"), isLittleEndian);
    }

    Var DataView::EntryGetFloat16(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();
        BOOL isLittleEndian = FALSE;

        Assert(!(callInfo.Flags & CallFlags_New));

        if (args.Info.Count == 0 || !VarIs<DataView>(args[0]))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_This_NeedDataView);
        }
        if (args.Info.Count > 2)
        {
            isLittleEndian = JavascriptConversion::ToBoolean(args[2], scriptContext);
        }

        DataView* dataView = VarTo<DataView>(args[0]);
        Var offset = args.Info.Count > 1 ? args[1] : scriptContext->GetLibrary()->GetUndefined();
        uint32 byteOffset = dataView->GetViewIndex(offset, _u("DataView.prototype.getFloat16"));
        dataView->CheckViewRange(byteOffset, sizeof(uint16), _u("DataView.prototype.getFloat16"));

        uint16 half = isLittleEndian
            ? (uint16)(dataView->buffer[byteOffset] | (dataView->buffer[byteOffset + 1] << 8))
            : (uint16)((dataView->buffer[byteOffset] << 8) | dataView->buffer[byteOffset + 1]);

        return JavascriptNumber::ToVarWithCheck(Float16::ToDouble(half), scriptContext);
    }

    Var DataView::EntryGetBigInt64(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();

        Assert(!(callInfo.Flags & CallFlags_New));

        if (args.Info.Count == 0 || !VarIs<DataView>(args[0]))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_This_NeedDataView);
        }

        DataView* dataView = VarTo<DataView>(args[0]);
        Var offset = args.Info.Count > 1 ? args[1] : scriptContext->GetLibrary()->GetUndefined();

        // GetViewValue order: ToIndex(requestIndex), ToBoolean(littleEndian),
        // then the detached/out-of-bounds checks.
        uint32 byteOffset = dataView->GetViewIndex(offset, _u("DataView.prototype.getBigInt64"));

        BOOL isLittleEndian = FALSE;
        if (args.Info.Count > 2)
        {
            isLittleEndian = JavascriptConversion::ToBoolean(args[2], scriptContext);
        }

        dataView->CheckViewRange(byteOffset, sizeof(uint64), _u("DataView.prototype.getBigInt64"));

        uint64 bits = DataViewReadUInt64(dataView->buffer + byteOffset, isLittleEndian != FALSE);
        return JavascriptBigInt::CreateFromInt64(DataViewBigInt64Bits(bits), scriptContext);
    }

    Var DataView::EntryGetBigUint64(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();

        Assert(!(callInfo.Flags & CallFlags_New));

        if (args.Info.Count == 0 || !VarIs<DataView>(args[0]))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_This_NeedDataView);
        }

        DataView* dataView = VarTo<DataView>(args[0]);
        Var offset = args.Info.Count > 1 ? args[1] : scriptContext->GetLibrary()->GetUndefined();
        uint32 byteOffset = dataView->GetViewIndex(offset, _u("DataView.prototype.getBigUint64"));

        BOOL isLittleEndian = FALSE;
        if (args.Info.Count > 2)
        {
            isLittleEndian = JavascriptConversion::ToBoolean(args[2], scriptContext);
        }

        dataView->CheckViewRange(byteOffset, sizeof(uint64), _u("DataView.prototype.getBigUint64"));

        uint64 bits = DataViewReadUInt64(dataView->buffer + byteOffset, isLittleEndian != FALSE);
        return JavascriptBigInt::CreateFromUInt64(bits, scriptContext);
    }

    Var DataView::EntrySetInt8(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();

        Assert(!(callInfo.Flags & CallFlags_New));

        int8 value = 0;
        switch (args.Info.Count)
        {
        case 0:
            JavascriptError::ThrowTypeError(scriptContext, JSERR_This_NeedDataView);
            break;
        case 1:
            JavascriptError::ThrowTypeError(scriptContext, JSERR_DataView_NeedArgument, _u("offset"));
            break;
        default:
            if (!VarIs<DataView>(args[0]))
            {
                JavascriptError::ThrowTypeError(scriptContext, JSERR_This_NeedDataView);
                break;
            }
            if (args.Info.Count > 2)
            {
                value = JavascriptConversion::ToInt8(args[2], scriptContext);
            }
            break;
        }

        DataView* dataView = VarTo<DataView>(args[0]);
        dataView->SetValue<int8>(args[1], value, _u("DataView.prototype.SetInt8"));
        return scriptContext->GetLibrary()->GetUndefined();
    }

    Var DataView::EntrySetUint8(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();

        Assert(!(callInfo.Flags & CallFlags_New));

        uint8 value = 0;
        switch (args.Info.Count)
        {
        case 0:
            JavascriptError::ThrowTypeError(scriptContext, JSERR_This_NeedDataView);
            break;
        case 1:
            JavascriptError::ThrowTypeError(scriptContext, JSERR_DataView_NeedArgument, _u("offset"));
            break;
        default:
            if (!VarIs<DataView>(args[0]))
            {
                JavascriptError::ThrowTypeError(scriptContext, JSERR_This_NeedDataView);
                break;
            }
            if (args.Info.Count > 2)
            {
                value = JavascriptConversion::ToUInt8(args[2], scriptContext);
            }
            break;
        }

        DataView* dataView = VarTo<DataView>(args[0]);
        dataView->SetValue<uint8>(args[1], value, _u("DataView.prototype.SetUint8"));
        return scriptContext->GetLibrary()->GetUndefined();
    }

    Var DataView::EntrySetInt16(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();

        Assert(!(callInfo.Flags & CallFlags_New));

        BOOL isLittleEndian = FALSE;
        int16 value = 0;
        switch (args.Info.Count)
        {
        case 0:
            JavascriptError::ThrowTypeError(scriptContext, JSERR_This_NeedDataView);
            break;
        case 1:
            JavascriptError::ThrowTypeError(scriptContext, JSERR_DataView_NeedArgument, _u("offset"));
            break;
        default:
            if (!VarIs<DataView>(args[0]))
            {
                JavascriptError::ThrowTypeError(scriptContext, JSERR_This_NeedDataView);
                break;
            }
            if (args.Info.Count > 2)
            {
                if (!JavascriptOperators::IsUndefined(args[2]))
                {
                    value = JavascriptConversion::ToInt16(args[2], scriptContext);
                }
                if (args.Info.Count > 3)
                {
                    isLittleEndian = JavascriptConversion::ToBoolean(args[3], scriptContext);
                }
            }
            break;
        }

        DataView* dataView = VarTo<DataView>(args[0]);
        dataView->SetValue<int16>(args[1], value, _u("DataView.prototype.SetInt16"), isLittleEndian);
        return scriptContext->GetLibrary()->GetUndefined();
    }

    Var DataView::EntrySetUint16(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();

        Assert(!(callInfo.Flags & CallFlags_New));

        BOOL isLittleEndian = FALSE;
        uint16 value = 0;
        switch (args.Info.Count)
        {
        case 0:
            JavascriptError::ThrowTypeError(scriptContext, JSERR_This_NeedDataView);
            break;
        case 1:
            JavascriptError::ThrowTypeError(scriptContext, JSERR_DataView_NeedArgument, _u("offset"));
            break;
        default:
            if (!VarIs<DataView>(args[0]))
            {
                JavascriptError::ThrowTypeError(scriptContext, JSERR_This_NeedDataView);
                break;
            }
            if (args.Info.Count > 2)
            {
                if (!JavascriptOperators::IsUndefined(args[2]))
                {
                    value = JavascriptConversion::ToUInt16(args[2], scriptContext);
                }
                if (args.Info.Count > 3)
                {
                    isLittleEndian = JavascriptConversion::ToBoolean(args[3], scriptContext);
                }
            }
            break;
        }

        DataView* dataView = VarTo<DataView>(args[0]);
        dataView->SetValue<uint16>(args[1], value, _u("DataView.prototype.SetUint16"), isLittleEndian);
        return scriptContext->GetLibrary()->GetUndefined();
    }

    Var DataView::EntrySetInt32(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();

        Assert(!(callInfo.Flags & CallFlags_New));

        BOOL isLittleEndian = FALSE;
        int32 value = 0;
        switch (args.Info.Count)
        {
        case 0:
            JavascriptError::ThrowTypeError(scriptContext, JSERR_This_NeedDataView);
            break;
        case 1:
            JavascriptError::ThrowTypeError(scriptContext, JSERR_DataView_NeedArgument, _u("offset"));
            break;
        default:
            if (!VarIs<DataView>(args[0]))
            {
                JavascriptError::ThrowTypeError(scriptContext, JSERR_This_NeedDataView);
                break;
            }
            if (args.Info.Count > 2)
            {
                if (!JavascriptOperators::IsUndefined(args[2]))
                {
                    value = JavascriptConversion::ToInt32(args[2], scriptContext);
                }
                if (args.Info.Count > 3)
                {
                    isLittleEndian = JavascriptConversion::ToBoolean(args[3], scriptContext);
                }
            }
            break;
        }

        DataView* dataView = VarTo<DataView>(args[0]);
        dataView->SetValue<int32>(args[1], value, _u("DataView.prototype.SetInt32"), isLittleEndian);
        return scriptContext->GetLibrary()->GetUndefined();
    }

    Var DataView::EntrySetUint32(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();

        Assert(!(callInfo.Flags & CallFlags_New));

        BOOL isLittleEndian = FALSE;
        uint32 value = 0;
        switch (args.Info.Count)
        {
        case 0:
            JavascriptError::ThrowTypeError(scriptContext, JSERR_This_NeedDataView);
            break;
        case 1:
            JavascriptError::ThrowTypeError(scriptContext, JSERR_DataView_NeedArgument, _u("offset"));
            break;
        default:
            if (!VarIs<DataView>(args[0]))
            {
                JavascriptError::ThrowTypeError(scriptContext, JSERR_This_NeedDataView);
                break;
            }
            if (args.Info.Count > 2)
            {
                if (!JavascriptOperators::IsUndefined(args[2]))
                {
                    value = JavascriptConversion::ToUInt32(args[2], scriptContext);
                }
                if (args.Info.Count > 3)
                {
                    isLittleEndian = JavascriptConversion::ToBoolean(args[3], scriptContext);
                }
            }
            break;
        }

        DataView* dataView = VarTo<DataView>(args[0]);
        dataView->SetValue<uint32>(args[1], value, _u("DataView.prototype.SetUint32"), isLittleEndian);
        return scriptContext->GetLibrary()->GetUndefined();
    }

    Var DataView::EntrySetFloat32(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();

        Assert(!(callInfo.Flags & CallFlags_New));

        BOOL isLittleEndian = FALSE;
        float value = static_cast<float>(JavascriptNumber::NaN);
        switch (args.Info.Count)
        {
        case 0:
            JavascriptError::ThrowTypeError(scriptContext, JSERR_This_NeedDataView);
            break;
        case 1:
            JavascriptError::ThrowTypeError(scriptContext, JSERR_DataView_NeedArgument, _u("offset"));
            break;
        default:
            if (!VarIs<DataView>(args[0]))
            {
                JavascriptError::ThrowTypeError(scriptContext, JSERR_This_NeedDataView);
                break;
            }
            if (args.Info.Count > 2)
            {
                if (!JavascriptOperators::IsUndefined(args[2]))
                {
                    value = JavascriptConversion::ToFloat(args[2], scriptContext);
                }
                if (args.Info.Count > 3)
                {
                    isLittleEndian = JavascriptConversion::ToBoolean(args[3], scriptContext);
                }
            }
            break;
        }

        DataView* dataView = VarTo<DataView>(args[0]);
        dataView->SetValue<float>(args[1], value, _u("DataView.prototype.SetFloat32"), isLittleEndian);
        return scriptContext->GetLibrary()->GetUndefined();
    }

    Var DataView::EntrySetFloat64(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();

        Assert(!(callInfo.Flags & CallFlags_New));

        BOOL isLittleEndian = FALSE;
        double value = JavascriptNumber::NaN;
        switch (args.Info.Count)
        {
        case 0:
            JavascriptError::ThrowTypeError(scriptContext, JSERR_This_NeedDataView);
            break;
        case 1:
            JavascriptError::ThrowTypeError(scriptContext, JSERR_DataView_NeedArgument, _u("offset"));
            break;
        default:
            if (!VarIs<DataView>(args[0]))
            {
                JavascriptError::ThrowTypeError(scriptContext, JSERR_This_NeedDataView);
                break;
            }
            if (args.Info.Count > 2)
            {
                if (!JavascriptOperators::IsUndefined(args[2]))
                {
                    value = JavascriptConversion::ToNumber(args[2], scriptContext);
                }
                if (args.Info.Count > 3)
                {
                    isLittleEndian = JavascriptConversion::ToBoolean(args[3], scriptContext);
                }
            }
            break;
        }

        DataView* dataView = VarTo<DataView>(args[0]);
        dataView->SetValue<double>(args[1], value, _u("DataView.prototype.SetFloat64"), isLittleEndian);
        return scriptContext->GetLibrary()->GetUndefined();
    }

    Var DataView::EntrySetFloat16(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();

        Assert(!(callInfo.Flags & CallFlags_New));

        if (args.Info.Count == 0 || !VarIs<DataView>(args[0]))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_This_NeedDataView);
        }

        DataView* dataView = VarTo<DataView>(args[0]);
        Var offset = args.Info.Count > 1 ? args[1] : scriptContext->GetLibrary()->GetUndefined();

        // ToIndex(requestIndex) precedes ToNumber(value); the detached and
        // range checks follow the value conversion (test262 ordering).
        uint32 byteOffset = dataView->GetViewIndex(offset, _u("DataView.prototype.setFloat16"));

        double value = JavascriptNumber::NaN;
        if (args.Info.Count > 2)
        {
            value = JavascriptConversion::ToNumber(args[2], scriptContext);
        }

        BOOL isLittleEndian = FALSE;
        if (args.Info.Count > 3)
        {
            isLittleEndian = JavascriptConversion::ToBoolean(args[3], scriptContext);
        }

        dataView->CheckViewRange(byteOffset, sizeof(uint16), _u("DataView.prototype.setFloat16"));

        uint16 half = Float16::ToBits(value);
        if (isLittleEndian)
        {
            dataView->buffer[byteOffset] = (BYTE)half;
            dataView->buffer[byteOffset + 1] = (BYTE)(half >> 8);
        }
        else
        {
            dataView->buffer[byteOffset] = (BYTE)(half >> 8);
            dataView->buffer[byteOffset + 1] = (BYTE)half;
        }

        return scriptContext->GetLibrary()->GetUndefined();
    }

    Var DataView::EntrySetBigInt64(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();

        Assert(!(callInfo.Flags & CallFlags_New));

        if (args.Info.Count == 0 || !VarIs<DataView>(args[0]))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_This_NeedDataView);
        }

        DataView* dataView = VarTo<DataView>(args[0]);
        Var offset = args.Info.Count > 1 ? args[1] : scriptContext->GetLibrary()->GetUndefined();

        // SetViewValue order: ToIndex(byteOffset) before ToBigInt(value),
        // then ToBoolean(littleEndian), then the detached/range checks.
        uint32 byteOffset = dataView->GetViewIndex(offset, _u("DataView.prototype.setBigInt64"));

        Var value = args.Info.Count > 2 ? args[2] : scriptContext->GetLibrary()->GetUndefined();
        JavascriptBigInt* bigIntValue = JavascriptConversion::ToBigInt(value, scriptContext);

        BOOL isLittleEndian = FALSE;
        if (args.Info.Count > 3)
        {
            isLittleEndian = JavascriptConversion::ToBoolean(args[3], scriptContext);
        }

        dataView->CheckViewRange(byteOffset, sizeof(uint64), _u("DataView.prototype.setBigInt64"));
        DataViewWriteUInt64(dataView->buffer + byteOffset, bigIntValue->ToUInt64Low(), isLittleEndian != FALSE);

        return scriptContext->GetLibrary()->GetUndefined();
    }

    Var DataView::EntrySetBigUint64(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();

        Assert(!(callInfo.Flags & CallFlags_New));

        if (args.Info.Count == 0 || !VarIs<DataView>(args[0]))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_This_NeedDataView);
        }

        DataView* dataView = VarTo<DataView>(args[0]);
        Var offset = args.Info.Count > 1 ? args[1] : scriptContext->GetLibrary()->GetUndefined();
        uint32 byteOffset = dataView->GetViewIndex(offset, _u("DataView.prototype.setBigUint64"));

        Var value = args.Info.Count > 2 ? args[2] : scriptContext->GetLibrary()->GetUndefined();
        JavascriptBigInt* bigIntValue = JavascriptConversion::ToBigInt(value, scriptContext);

        BOOL isLittleEndian = FALSE;
        if (args.Info.Count > 3)
        {
            isLittleEndian = JavascriptConversion::ToBoolean(args[3], scriptContext);
        }

        dataView->CheckViewRange(byteOffset, sizeof(uint64), _u("DataView.prototype.setBigUint64"));
        DataViewWriteUInt64(dataView->buffer + byteOffset, bigIntValue->ToUInt64Low(), isLittleEndian != FALSE);

        return scriptContext->GetLibrary()->GetUndefined();
    }

    Var DataView::EntryGetterBuffer(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();

        Assert(!(callInfo.Flags & CallFlags_New));

        if (args.Info.Count == 0 || !VarIs<DataView>(args[0]))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_This_NeedDataView);
        }

        DataView* dataView = VarTo<DataView>(args[0]);
        ArrayBufferBase* arrayBuffer = dataView->GetArrayBuffer();

        if (arrayBuffer == nullptr)
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedArrayBufferObject);
        }
        return CrossSite::MarshalVar(scriptContext, arrayBuffer);
    }

    Var DataView::EntryGetterByteLength(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();

        Assert(!(callInfo.Flags & CallFlags_New));

        if (args.Info.Count == 0 || !VarIs<DataView>(args[0]))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_This_NeedDataView);
        }

        DataView* dataView = VarTo<DataView>(args[0]);
        ArrayBufferBase* arrayBuffer = dataView->GetArrayBuffer();

        if (arrayBuffer == nullptr)
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedArrayBufferObject);
        }
        else if (arrayBuffer->IsDetached())
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_DetachedTypedArray, _u("DataView.prototype.byteLength"));
        }

        uint32 bufferByteLength = arrayBuffer->GetByteLength();
        if (dataView->IsOutOfBounds(bufferByteLength))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_DetachedTypedArray, _u("DataView.prototype.byteLength"));
        }

        return JavascriptNumber::ToVar(dataView->GetEffectiveByteLength(bufferByteLength), scriptContext);
    }

    Var DataView::EntryGetterByteOffset(RecyclableObject* function, CallInfo callInfo, ...)
    {
        PROBE_STACK(function->GetScriptContext(), Js::Constants::MinStackDefault);

        ARGUMENTS(args, callInfo);
        ScriptContext* scriptContext = function->GetScriptContext();

        Assert(!(callInfo.Flags & CallFlags_New));

        if (args.Info.Count == 0 || !VarIs<DataView>(args[0]))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_This_NeedDataView);
        }

        DataView* dataView = VarTo<DataView>(args[0]);
        ArrayBufferBase* arrayBuffer = dataView->GetArrayBuffer();

        if (arrayBuffer == nullptr)
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_NeedArrayBufferObject);
        }
        else if (arrayBuffer->IsDetached())
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_DetachedTypedArray, _u("DataView.prototype.byteOffset"));
        }

        if (dataView->IsOutOfBounds(arrayBuffer->GetByteLength()))
        {
            JavascriptError::ThrowTypeError(scriptContext, JSERR_DetachedTypedArray, _u("DataView.prototype.byteOffset"));
        }

        return JavascriptNumber::ToVar(dataView->GetByteOffset(), scriptContext);
    }

    BOOL DataView::SetItemWithAttributes(uint32 index, Var value, PropertyAttributes attributes)
    {
        AssertMsg(false, "We don't need a DataView to serve as object's internal array");
        return FALSE;
    }

    void DataView::ClearLengthAndBufferOnDetach()
    {
        AssertMsg(this->GetArrayBuffer()->IsDetached(), "Array buffer should be detached if we're calling this method");

        this->length = 0;
#if INT32VAR
        this->buffer = (BYTE*)TaggedInt::ToVarUnchecked(0);
#else
        this->buffer = nullptr;
#endif
    }

#ifdef _M_ARM
    // Provide template specialization (only) for memory access at unaligned float/double address which causes data alignment exception otherwise.
    template<>
    Var DataView::GetValueWithCheck<float>(Var offset, const char16 *funcName, BOOL isLittleEndian)
    {
        return this->GetValueWithCheck<float, float UNALIGNED*>(offset, isLittleEndian, funcName);
    }

    template<>
    Var DataView::GetValueWithCheck<double>(Var offset, const char16 *funcName, BOOL isLittleEndian)
    {
        return this->GetValueWithCheck<double, double UNALIGNED*>(offset, isLittleEndian, funcName);
    }

    template<>
    void DataView::SetValue<float>(Var offset, float value, const char16 *funcName, BOOL isLittleEndian)
    {
        this->SetValue<float, float UNALIGNED*>(offset, value, isLittleEndian, funcName);
    }

    template<>
    void DataView::SetValue<double>(Var offset, double value, const char16 *funcName, BOOL isLittleEndian)
    {
        this->SetValue<double, double UNALIGNED*>(offset, value, isLittleEndian, funcName);
    }
#endif

}
