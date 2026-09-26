#ifndef __STRING_H__
#define __STRING_H__

#include "object.h"

#define NEW_STRING(vm, value, len) AS_OBJECT(newString(vm, value, len))
#define AS_STRING(object) ((MyMoString *)object)
#define STRING_VAL(object) ((MyMoString *)object)->value
#define IS_STRING(object) (object->type == OBJ_STRING)

// UTF-8 text. `length` counts bytes, `chars` code points (equal for
// ASCII); indexing, slicing, len() and iteration work in code points.
typedef struct MyMoString
{
    MyMoObject object;
    int length;
    int chars;
    char *value;
} MyMoString;

MyMoString *newString(MVM *vm, const char *chars, int length);

void printString(MyMoString *string);

// Code points in bytes[0..length) (continuation bytes don't count).
int utf8Count(const char *bytes, int length);
// Byte offset where code point `index` (0..chars) starts.
int stringByteOffset(MyMoString *s, int index);
// Code point index of the character starting at byte `offset`.
int stringCharIndex(MyMoString *s, int offset);
// Byte length of the code point starting at s->value[offset].
int stringCharBytes(MyMoString *s, int offset);

void defineStringClass(MVM *vm);

#endif