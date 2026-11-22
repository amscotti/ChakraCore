//-------------------------------------------------------------------------------------------------------
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#pragma once

namespace Js
{
    enum class ZipMode : uint8
    {
        Shortest = 0,
        Longest = 1,
        Strict = 2,
    };

    // One zipped underlying iterator entry plus optional ZipKeyed key and
    // longest-mode padding value. Stored in Recycler memory with Field(...)
    // barriers so GC traces active iterators, keys, and padding values.
    struct ZipIterEntry
    {
        Field(RecyclableObject*) iterator;
        Field(Var) nextMethod;
        Field(Var) padding;
        Field(const PropertyRecord*) keyPropertyRecord;
        Field(Var) keyVar;
        Field(bool) done;

        ZipIterEntry()
            : iterator(nullptr),
              nextMethod(nullptr),
              padding(nullptr),
              keyPropertyRecord(nullptr),
              keyVar(nullptr),
              done(false)
        {
        }
    };

    typedef JsUtil::List<ZipIterEntry*, Recycler> ZipIterList;

    // Recycler-managed state for Iterator.zip / Iterator.zipKeyed helpers.
    // Ownership transfers on creation to JavascriptIteratorHelper::zipState
    // (Field(...)) and is released (zipState = nullptr) once completed/closed.
    class JavascriptIteratorZipState
    {
    public:
        Field(ZipIterList*) entries;
        Field(Field(Var)*) resultsScratch;
        Field(uint32) iterCount;
        Field(uint32) openCount;
        Field(ZipMode) mode;
        Field(bool) isKeyed;

        JavascriptIteratorZipState(Recycler* recycler, ZipMode mode, bool isKeyed)
            : entries(RecyclerNew(recycler, ZipIterList, recycler)),
              resultsScratch(nullptr),
              iterCount(0),
              openCount(0),
              mode(mode),
              isKeyed(isKeyed)
        {
        }
    };
} // namespace Js
