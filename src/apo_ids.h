#pragma once

#include <guiddef.h>

// Our APO COM class. Keep this stable once an endpoint points at it.
// {6A8BE200-CD89-4E77-97F5-D51E8F16EF90}
//
// External linkage with selectany: ATL's CComCoClass takes the address of this
// constant as a template argument, which requires an external symbol.
extern "C" __declspec(selectany) const CLSID CLSID_BetterMicroApo =
{
    0x6a8be200,
    0xcd89,
    0x4e77,
    { 0x97, 0xf5, 0xd5, 0x1e, 0x8f, 0x16, 0xef, 0x90 }
};
