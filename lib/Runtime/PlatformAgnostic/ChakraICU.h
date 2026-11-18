//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Copyright (c) ChakraCore Project Contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#pragma once

#include "CommonDefines.h"

#ifdef HAS_ICU
#ifdef WINDOWS10_ICU
// if WINDOWS10_ICU is defined, pretend like we are building for recent Redstone,
// even if that isn't necessarily true
#pragma push_macro("NTDDI_VERSION")
#undef NTDDI_VERSION
#define NTDDI_VERSION NTDDI_WIN10_RS5
#include <icu.h>
#pragma pop_macro("NTDDI_VERSION")
#else // ifdef WINDOWS10_ICU
// Normalize ICU_VERSION for non-Kit ICU
#ifndef ICU_VERSION
#include "unicode/uvernum.h"
#define ICU_VERSION U_ICU_VERSION_MAJOR_NUM
#endif

// Make non-Windows Kit ICU look and act like Windows Kit ICU for better compat
#define U_SHOW_CPLUSPLUS_API 0
// ICU 55 (Ubuntu 16.04 system default) has uloc_toUnicodeLocale* marked as draft, which is required for Intl
#if ICU_VERSION > 56
#define U_DEFAULT_SHOW_DRAFT 0
#define U_HIDE_DRAFT_API 1
#endif
#define U_HIDE_DEPRECATED_API 1
#define U_HIDE_OBSOLETE_API 1
#define U_HIDE_INTERNAL_API 1

#include "unicode/ubrk.h"
#include "unicode/ucal.h"
#include "unicode/uchar.h"
#include "unicode/uclean.h"
#include "unicode/ucol.h"
#include "unicode/ucurr.h"
#include "unicode/udat.h"
#include "unicode/udatpg.h"
#include "unicode/udisplaycontext.h"
#include "unicode/uformattedvalue.h"
#include "unicode/uldnames.h"
#include "unicode/ulistformatter.h"
#include "unicode/uloc.h"
#include "unicode/ulocale.h"
#include "unicode/ulocbuilder.h"
#include "unicode/ulocdata.h"
#include "unicode/unumberformatter.h"
#include "unicode/unumsys.h"
#include "unicode/ureldatefmt.h"
#include "unicode/ures.h"
#include "unicode/uscript.h"
#include "unicode/ustring.h"
#include "unicode/unorm2.h"
#include "unicode/upluralrules.h"
#endif // ifdef WINDOWS10_ICU

// Different assertion code is used in ChakraFull that enforces that messages are char literals
#ifdef _CHAKRACOREBUILD
#define ICU_ERRORMESSAGE(e) u_errorName(e)
#else
#define ICU_ERRORMESSAGE(e) "Bad status returned from ICU"
#endif

#define ICU_FAILURE(e) (U_FAILURE(e) || e == U_STRING_NOT_TERMINATED_WARNING)
#define ICU_BUFFER_FAILURE(e) (e == U_BUFFER_OVERFLOW_ERROR || e == U_STRING_NOT_TERMINATED_WARNING)

namespace PlatformAgnostic
{
    namespace ICUHelpers
    {
        template<typename TObject, void(__cdecl * CloseFunction)(TObject)>
        class ScopedICUObject
        {
        private:
            TObject object;
        public:
            explicit ScopedICUObject(TObject object) : object(object)
            {

            }
            ScopedICUObject(const ScopedICUObject &other) = delete;
            ScopedICUObject(ScopedICUObject &&other) = delete;
            ScopedICUObject &operator=(const ScopedICUObject &other) = delete;
            ScopedICUObject &operator=(ScopedICUObject &&other) = delete;
            ~ScopedICUObject()
            {
                if (object != nullptr)
                {
                    CloseFunction(object);
                }
            }

            operator TObject()
            {
                return object;
            }
        };

        typedef ScopedICUObject<UEnumeration *, uenum_close> ScopedUEnumeration;
        typedef ScopedICUObject<UCollator *, ucol_close> ScopedUCollator;
        typedef ScopedICUObject<UDateFormat *, udat_close> ScopedUDateFormat;
        typedef ScopedICUObject<UNumberFormat *, unum_close> ScopedUNumberFormat;
        typedef ScopedICUObject<UNumberingSystem *, unumsys_close> ScopedUNumberingSystem;
        typedef ScopedICUObject<UDateTimePatternGenerator *, udatpg_close> ScopedUDateTimePatternGenerator;
        typedef ScopedICUObject<UFieldPositionIterator *, ufieldpositer_close> ScopedUFieldPositionIterator;
        typedef ScopedICUObject<ULocale *, ulocale_close> ScopedULocale;
        typedef ScopedICUObject<ULocaleBuilder *, ulocbld_close> ScopedULocaleBuilder;
        typedef ScopedICUObject<UListFormatter *, ulistfmt_close> ScopedUListFormatter;
        typedef ScopedICUObject<UFormattedList *, ulistfmt_closeResult> ScopedUFormattedList;
        typedef ScopedICUObject<URelativeDateTimeFormatter *, ureldatefmt_close> ScopedURelativeDateTimeFormatter;
        typedef ScopedICUObject<UFormattedRelativeDateTime *, ureldatefmt_closeResult> ScopedUFormattedRelativeDateTime;
        typedef ScopedICUObject<UConstrainedFieldPosition *, ucfpos_close> ScopedUConstrainedFieldPosition;
        typedef ScopedICUObject<ULocaleDisplayNames *, uldn_close> ScopedULocaleDisplayNames;
        typedef ScopedICUObject<UBreakIterator *, ubrk_close> ScopedUBreakIterator;
        typedef ScopedICUObject<UResourceBundle *, ures_close> ScopedUResourceBundle;
        typedef ScopedICUObject<UNumberFormatter *, unumf_close> ScopedUNumberFormatter;
        typedef ScopedICUObject<UFormattedNumber *, unumf_closeResult> ScopedUFormattedNumber;

        inline int GetICUMajorVersion()
        {
            UVersionInfo version = { 0 };
            u_getVersion(version);
            return version[0];
        }
    }
}
#endif // ifdef HAS_ICU
