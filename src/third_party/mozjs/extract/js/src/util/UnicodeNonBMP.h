/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Generated by make_unicode.py DO NOT MODIFY */
/* Unicode version: 13.0.0 */

#ifndef util_UnicodeNonBMP_h
#define util_UnicodeNonBMP_h

// |MACRO| receives the following arguments
//   MACRO(FROM, TO, LEAD, TRAIL_FROM, TRAIL_TO, DIFF)
//     FROM:       code point where the range starts
//     TO:         code point where the range ends
//     LEAD:       common lead surrogate of FROM and TO
//     TRAIL_FROM: trail surrogate of FROM
//     TRAIL_FROM: trail surrogate of TO
//     DIFF:       the difference between the code point in the range and
//                 converted code point

// U+10400 DESERET CAPITAL LETTER LONG I .. U+10427 DESERET CAPITAL LETTER EW
// U+104B0 OSAGE CAPITAL LETTER A .. U+104D3 OSAGE CAPITAL LETTER ZHA
// U+10C80 OLD HUNGARIAN CAPITAL LETTER A .. U+10CB2 OLD HUNGARIAN CAPITAL LETTER US
// U+118A0 WARANG CITI CAPITAL LETTER NGAA .. U+118BF WARANG CITI CAPITAL LETTER VIYO
// U+16E40 MEDEFAIDRIN CAPITAL LETTER M .. U+16E5F MEDEFAIDRIN CAPITAL LETTER Y
// U+1E900 ADLAM CAPITAL LETTER ALIF .. U+1E921 ADLAM CAPITAL LETTER SHA
#define FOR_EACH_NON_BMP_LOWERCASE(MACRO) \
    MACRO(0x10400, 0x10427, 0xd801, 0xdc00, 0xdc27, 40) \
    MACRO(0x104b0, 0x104d3, 0xd801, 0xdcb0, 0xdcd3, 40) \
    MACRO(0x10c80, 0x10cb2, 0xd803, 0xdc80, 0xdcb2, 64) \
    MACRO(0x118a0, 0x118bf, 0xd806, 0xdca0, 0xdcbf, 32) \
    MACRO(0x16e40, 0x16e5f, 0xd81b, 0xde40, 0xde5f, 32) \
    MACRO(0x1e900, 0x1e921, 0xd83a, 0xdd00, 0xdd21, 34)

// U+10428 DESERET SMALL LETTER LONG I .. U+1044F DESERET SMALL LETTER EW
// U+104D8 OSAGE SMALL LETTER A .. U+104FB OSAGE SMALL LETTER ZHA
// U+10CC0 OLD HUNGARIAN SMALL LETTER A .. U+10CF2 OLD HUNGARIAN SMALL LETTER US
// U+118C0 WARANG CITI SMALL LETTER NGAA .. U+118DF WARANG CITI SMALL LETTER VIYO
// U+16E60 MEDEFAIDRIN SMALL LETTER M .. U+16E7F MEDEFAIDRIN SMALL LETTER Y
// U+1E922 ADLAM SMALL LETTER ALIF .. U+1E943 ADLAM SMALL LETTER SHA
#define FOR_EACH_NON_BMP_UPPERCASE(MACRO) \
    MACRO(0x10428, 0x1044f, 0xd801, 0xdc28, 0xdc4f, -40) \
    MACRO(0x104d8, 0x104fb, 0xd801, 0xdcd8, 0xdcfb, -40) \
    MACRO(0x10cc0, 0x10cf2, 0xd803, 0xdcc0, 0xdcf2, -64) \
    MACRO(0x118c0, 0x118df, 0xd806, 0xdcc0, 0xdcdf, -32) \
    MACRO(0x16e60, 0x16e7f, 0xd81b, 0xde60, 0xde7f, -32) \
    MACRO(0x1e922, 0x1e943, 0xd83a, 0xdd22, 0xdd43, -34)

#endif /* util_UnicodeNonBMP_h */
