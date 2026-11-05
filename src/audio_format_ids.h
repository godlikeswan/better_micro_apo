#pragma once

#include <guiddef.h>

// Format subtype GUIDs used by UNCOMPRESSEDAUDIOFORMAT::guidFormatType.
// These are the well-known KSDATAFORMAT_SUBTYPE_* values. Defining them here
// keeps the APO free of ksmedia.h and its extra dependencies.
// {00000001-0000-0010-8000-00AA00389B71}
inline constexpr GUID SUBTYPE_PCM =
{
    0x00000001,
    0x0000,
    0x0010,
    { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 }
};

// {00000003-0000-0010-8000-00AA00389B71}
inline constexpr GUID SUBTYPE_IEEE_FLOAT =
{
    0x00000003,
    0x0000,
    0x0010,
    { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 }
};
