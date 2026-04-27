#ifndef __CHUNK_H__
#define __CHUNK_H__

#include "common.h"
#include "opcodes.h"
#include "value.h"
#include "datatypes/object.h"

typedef struct chunk
{
    int count;
    int capacity;
    u32 *lines;
    u32 *cols;
    u8 *code;
    ValueArray constants;
} Chunk;

Chunk *newChunk(MVM *vm);
void initChunk(MVM *vm, Chunk *chunk);
void writeChunk(MVM *vm, Chunk *chunk, u8 byte, u32 line, u32 col);
void freeChunk(MVM *vm, Chunk *chunk);
void printChunk(Chunk *chunk);

// Legacy emit path: stores `object` as `V_OBJ_VAL(object)` in the pool.
int addConstant(MVM *vm, Chunk *chunk, MyMoObject *object);

// Value-native emit path. Used by future steps when ints / doubles / nil /
// bool literals are emitted as inline Values rather than heap objects.
int addConstantV(MVM *vm, Chunk *chunk, Value v);

#endif
