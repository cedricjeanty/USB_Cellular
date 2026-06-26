// Minimal OEM<->Unicode shim for FatFs LFN support (FF_USE_LFN=1, FF_CODE_PAGE=437).
//
// The full FatFs ffunicode.c carries conversion tables for every code page and is
// large. This project's filenames are ASCII (e.g. boot_0007.log,
// flightHistory__FLT00123.eaofh), so an ASCII-correct shim is sufficient and keeps
// the native/emulator FatFs build small. ASCII (0x00-0x7F) round-trips exactly; the
// CP437 high half (0x80-0xFF) passes through identity (not strictly correct for
// accented OEM glyphs, but those never appear in DSU/log filenames).
//
// Signatures match the stock FatFs module so ff.c links unchanged.

#include "ff.h"

#if FF_USE_LFN != 0

// OEM code -> Unicode (BMP). ASCII is identity; high half passed through.
WCHAR ff_oem2uni(WCHAR oem, WORD cp) {
    (void)cp;
    return oem;
}

// Unicode (BMP) -> OEM code. ASCII is identity; >0xFF is unconvertible (0).
WCHAR ff_uni2oem(DWORD uni, WORD cp) {
    (void)cp;
    return (uni <= 0xFF) ? (WCHAR)uni : 0;
}

// Unicode up-conversion. ASCII a-z -> A-Z; everything else unchanged. Adequate
// for the case-insensitive LFN matching FatFs does on ASCII names.
DWORD ff_wtoupper(DWORD uni) {
    if (uni >= 'a' && uni <= 'z') return uni - ('a' - 'A');
    return uni;
}

#endif // FF_USE_LFN
