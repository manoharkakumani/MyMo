// unicase.c — see unicase.h.

#include "unicase.h"

#include <stddef.h>

uint32_t ucDecode(const char *str, int len, int *i)
{
    const unsigned char *s = (const unsigned char *)str;
    unsigned char c = s[*i];
    int n = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
    if (n <= 1 || *i + n > len)
    {
        (*i)++;
        return c;
    }
    uint32_t cp = c & (0x7F >> n);
    for (int k = 1; k < n; k++)
    {
        if ((s[*i + k] & 0xC0) != 0x80)
        {
            (*i)++;
            return c;
        }
        cp = (cp << 6) | (s[*i + k] & 0x3F);
    }
    *i += n;
    return cp;
}

int ucEncode(uint32_t cp, char *buf)
{
    unsigned char *b = (unsigned char *)buf;
    if (cp < 0x80)
    {
        b[0] = (unsigned char)cp;
        return 1;
    }
    if (cp < 0x800)
    {
        b[0] = (unsigned char)(0xC0 | (cp >> 6));
        b[1] = (unsigned char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000)
    {
        b[0] = (unsigned char)(0xE0 | (cp >> 12));
        b[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
        b[2] = (unsigned char)(0x80 | (cp & 0x3F));
        return 3;
    }
    b[0] = (unsigned char)(0xF0 | (cp >> 18));
    b[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
    b[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
    b[3] = (unsigned char)(0x80 | (cp & 0x3F));
    return 4;
}

// ------------------------------------------------------------ case tables

// Upper-case ranges and how they map to lower case:
//   DELTA  lo..hi -> cp + delta
//   EVEN   even code points are upper, the next odd one is the lower
//   ODD    odd code points are upper, the next even one is the lower
// LOWER_ONLY entries map to lower case but are not reversed by upper().
enum { DELTA, EVEN, ODD };
typedef struct
{
    uint32_t lo, hi;
    int kind;
    int32_t delta;
    bool lowerOnly;
} CaseRange;

static const CaseRange CASES[] = {
    {0x41, 0x5A, DELTA, 32, false},
    {0xC0, 0xD6, DELTA, 32, false},
    {0xD8, 0xDE, DELTA, 32, false},
    {0x100, 0x12F, EVEN, 0, false},
    {0x130, 0x130, DELTA, 0x69 - 0x130, true}, // İ -> i
    {0x132, 0x137, EVEN, 0, false},
    {0x139, 0x148, ODD, 0, false},
    {0x14A, 0x177, EVEN, 0, false},
    {0x178, 0x178, DELTA, 0xFF - 0x178, false}, // Ÿ <-> ÿ
    {0x179, 0x17E, ODD, 0, false},
    {0x1C4, 0x1C4, DELTA, 2, false}, // Ǆ ǅ ǆ, Ǉ ǈ ǉ, Ǌ ǋ ǌ, Ǳ ǲ ǳ digraphs;
    {0x1C5, 0x1C5, DELTA, 1, true},  // the middle (title-case) form
    {0x1C7, 0x1C7, DELTA, 2, false}, // lowers but upper() skips it
    {0x1C8, 0x1C8, DELTA, 1, true},
    {0x1CA, 0x1CA, DELTA, 2, false},
    {0x1CB, 0x1CB, DELTA, 1, true},
    {0x1CD, 0x1DC, ODD, 0, false},
    {0x1DE, 0x1EF, EVEN, 0, false},
    {0x1F1, 0x1F1, DELTA, 2, false},
    {0x1F2, 0x1F2, DELTA, 1, true},
    {0x1F8, 0x21F, EVEN, 0, false},
    {0x222, 0x233, EVEN, 0, false},
    {0x386, 0x386, DELTA, 38, false},
    {0x388, 0x38A, DELTA, 37, false},
    {0x38C, 0x38C, DELTA, 64, false},
    {0x38E, 0x38F, DELTA, 63, false},
    {0x391, 0x3A1, DELTA, 32, false},
    {0x3A3, 0x3AB, DELTA, 32, false},
    {0x3D8, 0x3EF, EVEN, 0, false},
    {0x400, 0x40F, DELTA, 80, false},
    {0x410, 0x42F, DELTA, 32, false},
    {0x460, 0x481, EVEN, 0, false},
    {0x48A, 0x4BF, EVEN, 0, false},
    {0x4C0, 0x4C0, DELTA, 15, false},
    {0x4C1, 0x4CE, ODD, 0, false},
    {0x4D0, 0x52F, EVEN, 0, false},
    {0x531, 0x556, DELTA, 48, false},
    {0x10A0, 0x10C5, DELTA, 0x2D00 - 0x10A0, false},
    {0x1E00, 0x1E95, EVEN, 0, false},
    {0x1EA0, 0x1EFF, EVEN, 0, false},
    {0x1F08, 0x1F0F, DELTA, -8, false},
    {0x1F18, 0x1F1D, DELTA, -8, false},
    {0x1F28, 0x1F2F, DELTA, -8, false},
    {0x1F38, 0x1F3F, DELTA, -8, false},
    {0x1F48, 0x1F4D, DELTA, -8, false},
    {0x1F68, 0x1F6F, DELTA, -8, false},
    {0x2C00, 0x2C2F, DELTA, 48, false},
    {0xA640, 0xA66D, EVEN, 0, false},
    {0xA680, 0xA69B, EVEN, 0, false},
    {0xFF21, 0xFF3A, DELTA, 32, false},
};
#define CASE_COUNT (sizeof(CASES) / sizeof(CASES[0]))

uint32_t ucToLower(uint32_t cp)
{
    if (cp < 0x80)
        return cp >= 'A' && cp <= 'Z' ? cp + 32 : cp;
    for (size_t i = 0; i < CASE_COUNT; i++)
    {
        const CaseRange *r = &CASES[i];
        if (cp < r->lo || cp > r->hi)
            continue;
        switch (r->kind)
        {
        case DELTA:
            return (uint32_t)((int32_t)cp + r->delta);
        case EVEN:
            return (cp & 1) == 0 ? cp + 1 : cp;
        default:
            return (cp & 1) == 1 ? cp + 1 : cp;
        }
    }
    return cp;
}

uint32_t ucToUpper(uint32_t cp)
{
    if (cp < 0x80)
        return cp >= 'a' && cp <= 'z' ? cp - 32 : cp;
    switch (cp)
    {
    case 0xB5: return 0x39C;  // µ -> Μ
    case 0x131: return 'I';   // dotless ı
    case 0x17F: return 'S';   // long ſ
    case 0x3C2: return 0x3A3; // final ς -> Σ
    case 0x1C5: case 0x1C8: case 0x1CB: case 0x1F2:
        return cp - 1; // title-case digraph -> upper
    }
    for (size_t i = 0; i < CASE_COUNT; i++)
    {
        const CaseRange *r = &CASES[i];
        if (r->lowerOnly)
            continue;
        switch (r->kind)
        {
        case DELTA:
        {
            int64_t lo = (int64_t)r->lo + r->delta, hi = (int64_t)r->hi + r->delta;
            if ((int64_t)cp >= lo && (int64_t)cp <= hi)
                return (uint32_t)((int32_t)cp - r->delta);
            break;
        }
        case EVEN:
            if (cp > r->lo && cp <= r->hi && (cp & 1) == 1)
                return cp - 1;
            break;
        default:
            if (cp > r->lo && cp <= r->hi + 1 && (cp & 1) == 0)
                return cp - 1;
            break;
        }
    }
    return cp;
}

bool ucIsUpper(uint32_t cp) { return ucToLower(cp) != cp; }
bool ucIsLower(uint32_t cp) { return ucToUpper(cp) != cp || cp == 0xDF; }

// ------------------------------------------------------- character classes

typedef struct
{
    uint32_t lo, hi;
} Range;

static bool inRanges(uint32_t cp, const Range *ranges, size_t n)
{
    size_t lo = 0, hi = n;
    while (lo < hi)
    {
        size_t mid = (lo + hi) / 2;
        if (cp < ranges[mid].lo)
            hi = mid;
        else if (cp > ranges[mid].hi)
            lo = mid + 1;
        else
            return true;
    }
    return false;
}

// Letters (Unicode L*), sorted, for the scripts people commonly write.
static const Range ALPHA[] = {
    {0x41, 0x5A}, {0x61, 0x7A}, {0xAA, 0xAA}, {0xB5, 0xB5}, {0xBA, 0xBA},
    {0xC0, 0xD6}, {0xD8, 0xF6}, {0xF8, 0x2C1}, {0x2C6, 0x2D1}, {0x2E0, 0x2E4},
    {0x370, 0x374}, {0x376, 0x377}, {0x37A, 0x37D}, {0x37F, 0x37F}, {0x386, 0x386},
    {0x388, 0x38A}, {0x38C, 0x38C}, {0x38E, 0x3A1}, {0x3A3, 0x3F5}, {0x3F7, 0x481},
    {0x48A, 0x52F}, {0x531, 0x556}, {0x560, 0x588}, {0x5D0, 0x5EA}, {0x620, 0x64A},
    {0x671, 0x6D3}, {0x904, 0x939}, {0x93D, 0x93D}, {0x950, 0x950}, {0x958, 0x961},
    {0x985, 0x9B9}, {0xA05, 0xA39}, {0xA85, 0xAB9}, {0xB05, 0xB39}, {0xB85, 0xBB9},
    {0xC05, 0xC39}, {0xC85, 0xCB9}, {0xD05, 0xD3A}, {0xE01, 0xE30}, {0xE32, 0xE33},
    {0xE40, 0xE46}, {0x10A0, 0x10FF}, {0x1100, 0x11FF}, {0x1E00, 0x1F15}, {0x1F18, 0x1F1D},
    {0x1F20, 0x1F45}, {0x1F48, 0x1F4D}, {0x1F50, 0x1F7D}, {0x1F80, 0x1FBC}, {0x1FC2, 0x1FCC},
    {0x1FD0, 0x1FDB}, {0x1FE0, 0x1FEC}, {0x1FF2, 0x1FFC}, {0x2C00, 0x2C5F}, {0x2D00, 0x2D25},
    {0x3041, 0x3096}, {0x309D, 0x309F}, {0x30A1, 0x30FA}, {0x30FC, 0x30FF}, {0x3131, 0x318E},
    {0x3400, 0x4DBF}, {0x4E00, 0x9FFF}, {0xA640, 0xA66D}, {0xA680, 0xA69D}, {0xAC00, 0xD7A3},
    {0xF900, 0xFAFF}, {0xFF21, 0xFF3A}, {0xFF41, 0xFF5A}, {0xFF66, 0xFF9D}, {0x20000, 0x2FA1F},
};

// Digits (Python's isdigit: decimal digits plus super/subscripts).
static const Range DIGIT[] = {
    {0x30, 0x39}, {0xB2, 0xB3}, {0xB9, 0xB9}, {0x660, 0x669}, {0x6F0, 0x6F9},
    {0x966, 0x96F}, {0x9E6, 0x9EF}, {0xA66, 0xA6F}, {0xAE6, 0xAEF}, {0xB66, 0xB6F},
    {0xBE6, 0xBEF}, {0xC66, 0xC6F}, {0xCE6, 0xCEF}, {0xD66, 0xD6F}, {0xE50, 0xE59},
    {0x2070, 0x2070}, {0x2074, 0x2079}, {0x2080, 0x2089}, {0x2460, 0x2468}, {0xFF10, 0xFF19},
};

static const Range SPACE[] = {
    {0x09, 0x0D}, {0x1C, 0x20}, {0x85, 0x85}, {0xA0, 0xA0}, {0x1680, 0x1680},
    {0x2000, 0x200A}, {0x2028, 0x2029}, {0x202F, 0x202F}, {0x205F, 0x205F}, {0x3000, 0x3000},
};

#define RANGES(t) t, sizeof(t) / sizeof(t[0])

bool ucIsAlpha(uint32_t cp) { return inRanges(cp, RANGES(ALPHA)); }
bool ucIsDigit(uint32_t cp) { return inRanges(cp, RANGES(DIGIT)); }
bool ucIsSpace(uint32_t cp) { return inRanges(cp, RANGES(SPACE)); }
