#include "chunk.h"
#include "memory.h"
#include "vm.h"
#include "common.h"

#include "debug.h"

Chunk *newChunk(MVM *vm)
{
    Chunk *chunk = New(Chunk, 1);
    initChunk(vm, chunk);
    return chunk;
}

void initChunk(MVM *vm, Chunk *chunk)
{
    chunk->count = 0;
    chunk->capacity = 0;
    chunk->code = NULL;
    chunk->lines = NULL;
    chunk->cols = NULL;
    initValueArray(vm, &chunk->constants);
}

void writeChunk(MVM *vm, Chunk *chunk, u8 byte, u32 line, u32 col)
{
    if (chunk->capacity < chunk->count + 1)
    {
        int oldCapacity = chunk->capacity;
        chunk->capacity = ResizeCapacity(oldCapacity);
        chunk->code = ResizeArray(vm, u8, chunk->code, oldCapacity, chunk->capacity);
        chunk->lines = ResizeArray(vm, u32, chunk->lines, oldCapacity, chunk->capacity);
        chunk->cols = ResizeArray(vm, u32, chunk->cols, oldCapacity, chunk->capacity);
    }
    chunk->lines[chunk->count] = line;
    chunk->cols[chunk->count] = col;
    chunk->code[chunk->count] = byte;
    chunk->count++;
}

void freeChunk(MVM *vm, Chunk *chunk)
{
    FreeArray(vm, u8, chunk->code, chunk->capacity);
    FreeArray(vm, u32, chunk->lines, chunk->capacity);
    FreeArray(vm, u32, chunk->cols, chunk->capacity);
    freeValueArray(vm, &chunk->constants);
    free(chunk);
}

int addConstant(MVM *vm, Chunk *chunk, MyMoObject *object)
{
    writeValueArray(vm, &chunk->constants, V_OBJ_VAL(object));
    return chunk->constants.count - 1;
}

int addConstantV(MVM *vm, Chunk *chunk, Value v)
{
    writeValueArray(vm, &chunk->constants, v);
    return chunk->constants.count - 1;
}

void printChunk(Chunk *chunk){
    for(int i=0;i<chunk->count;){
      i=disassembleInstruction(chunk,i);
    }
}
