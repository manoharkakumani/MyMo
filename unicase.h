#ifndef mymo_unicase_h
#define mymo_unicase_h

// Unicode case mapping and character classes for the str methods
// (upper, lower, title, isalpha, ...). Table-driven and locale-free:
// covers Latin (incl. Vietnamese), Greek, Cyrillic, Armenian and
// fullwidth forms for case, plus the letter/digit/space ranges of the
// major scripts. Code points outside the tables are uncased non-letters.

#include <stdbool.h>
#include <stdint.h>

// Decode one UTF-8 sequence at s[*i] (advancing *i); malformed bytes
// decode as themselves one at a time.
uint32_t ucDecode(const char *s, int len, int *i);
// Append cp as UTF-8 to buf; returns bytes written (1-4).
int ucEncode(uint32_t cp, char *buf);

uint32_t ucToUpper(uint32_t cp);
uint32_t ucToLower(uint32_t cp);
bool ucIsUpper(uint32_t cp);
bool ucIsLower(uint32_t cp);
bool ucIsAlpha(uint32_t cp);
bool ucIsDigit(uint32_t cp);
bool ucIsSpace(uint32_t cp);

#endif
