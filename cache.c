#include "cache.h"
#include "datatypes/datatypes.h"
#include "debug.h"

void intSerialize(MyMoInt *number, FILE *stream)
{
    fwrite(&number->value, sizeof(number->value), 1, stream);
}

MyMoObject *intDeserialize(MVM *vm, FILE *stream)
{
    long value;
    fread(&value, sizeof(value), 1, stream);
    return NEW_INT(vm, value);
}

void doubleSerialize(MyMoDouble *number, FILE *stream)
{
    fwrite(&number->value, sizeof(number->value), 1, stream);
}

MyMoObject *doubleDeserialize(MVM *vm, FILE *stream)
{
    double value;
    fread(&value, sizeof(value), 1, stream);
    return NEW_DOUBLE(vm, value);
}

void stringSerialize(MyMoString *string, FILE *stream)
{
    fwrite(&string->length, sizeof(string->length), 1, stream);
    fwrite(string->value, string->length + 1, 1, stream);
}

MyMoObject *stringDeserialize(MVM *vm, FILE *stream)
{
    int length;
    fread(&length, sizeof(length), 1, stream);
    char *value = Allocate(vm, char, length + 1);
    fread(value, length + 1, 1, stream);
    MyMoObject *object = NEW_STRING(vm, value, length);
    Free(vm, char, value);
    return object;
}

void objectSerialize(MyMoObject *object, FILE *stream)
{
    fwrite(&object->type, sizeof(object->type), 1, stream);
    switch (object->type)
    {
    case OBJ_INT:
        intSerialize(AS_INT(object), stream);
        break;
    case OBJ_DOUBLE:
        doubleSerialize(AS_DOUBLE(object), stream);
        break;
    case OBJ_STRING:
        stringSerialize(AS_STRING(object), stream);
        break;
    case OBJ_FUNCTION:
        functionSerialize(AS_FUNCTION(object), stream);
        break;
    default:
        break;
    }
}

MyMoObject *objectDeserialize(MVM *vm, FILE *stream)
{
    MyMoObject *object = NULL;
    MyMoObjectType type;
    fread(&type, sizeof(type), 1, stream);
    switch (type)
    {
    case OBJ_INT:
    {
        object = intDeserialize(vm, stream);
        break;
    }
    case OBJ_DOUBLE:
    {
        object = doubleDeserialize(vm, stream);
        break;
    }
    case OBJ_STRING:
    {
        object = stringDeserialize(vm, stream);
        break;
    }
    case OBJ_FUNCTION:
    {
        object = AS_OBJECT(functionDeserialize(vm, stream));
        break;
    }
    default:
        break;
    }
    return object;
}

// Constant-pool serialization. The on-disk format is unchanged — each entry
// is still serialized as a heap-object record — but the in-memory pool is now
// a ValueArray, so each Value gets unwrapped via V_AS_OBJ before write and
// rewrapped via V_OBJ_VAL on read. Once inline Values land in the pool
// (steps 1.3+), this format will need a tag byte per entry; punted until
// then so the file layout stays compatible during the migration.
void arraySerialize(ValueArray *array, FILE *stream)
{
    fwrite(&array->capacity, sizeof(array->capacity), 1, stream);
    fwrite(&array->count, sizeof(array->capacity), 1, stream);
    for (int i = 0; i < array->count; i++)
    {
        objectSerialize(V_AS_OBJ(array->values[i]), stream);
    }
}

ValueArray arrayDeserialize(MVM *vm, FILE *stream)
{
    ValueArray array;
    fread(&array.capacity, sizeof(array.capacity), 1, stream);
    fread(&array.count, sizeof(array.count), 1, stream);
    array.values = New(Value, array.capacity);
    for (int i = 0; i < array.count; i++)
    {
        array.values[i] = V_OBJ_VAL(objectDeserialize(vm, stream));
    }
    return array;
}

void chunkSerialize(Chunk *chunk, FILE *stream)
{
    fwrite(&chunk->capacity, sizeof(chunk->capacity), 1, stream);
    fwrite(&chunk->count, sizeof(chunk->count), 1, stream);
    for (u32 i = 0; i < chunk->count; i++)
    {
        fwrite(&chunk->lines[i], sizeof(uint), 1, stream);
        fwrite(&chunk->cols[i], sizeof(uint), 1, stream);
        fwrite(&chunk->code[i], sizeof(uint), 1, stream);
    }
    arraySerialize(&chunk->constants, stream);
}

void chunkDeserialize(MVM *vm, Chunk *chunk, FILE *stream)
{
    fread(&chunk->capacity, sizeof(chunk->capacity), 1, stream);
    fread(&chunk->count, sizeof(chunk->count), 1, stream);
    chunk->lines = New(uint, chunk->capacity);
    chunk->cols = New(uint, chunk->capacity);
    chunk->code = New(u8, chunk->capacity);
    for (u32 i = 0; i < chunk->count; i++)
    {
        fread(&chunk->lines[i], sizeof(uint), 1, stream);
        fread(&chunk->cols[i], sizeof(uint), 1, stream);
        fread(&chunk->code[i], sizeof(uint), 1, stream);
    }
    chunk->constants = arrayDeserialize(vm, stream);
    return;
}

// void entrySerialize(Entry *entry, FILE *stream)
// {
//     int keynull = 0;
//     if (entry->key == NULL)
//     {
//         keynull = 1;
//         fwrite(&keynull, sizeof(keynull), 1, stream);
//         return;
//     }
//     fwrite(&keynull, sizeof(keynull), 1, stream);
//     objectSerialize(entry->key, stream);
//     objectSerialize(entry->value, stream);
// }

// void entryDeserialize(MVM *vm, Entry *entry, FILE *stream)
// {
//     int keynull;
//     fread(&keynull, sizeof(keynull), 1, stream);
//     if (keynull)
//     {
//         entry->key = NULL;
//         entry->value = NEW_NIL;
//         return;
//     }
//     entry->key = objectDeserialize(vm, stream);
//     entry->value = objectDeserialize(vm, stream);
// }

// void DictSerialize(MyMoDict *dict, FILE *stream)
// {
//     fwrite(&dict->capacity, sizeof(dict->capacity), 1, stream);
//     fwrite(&dict->count, sizeof(dict->count), 1, stream);
//     for (int i = 0; i < dict->capacity; i++)
//     {
//         entrySerialize(&dict->entries[i], stream);
//     }
// }

// void DictDeserialize(MVM *vm, MyMoDict *dict, FILE *stream)
// {
//     fread(&dict->capacity, sizeof(dict->capacity), 1, stream);
//     fread(&dict->count, sizeof(dict->count), 1, stream);
//     if (dict->capacity < 0)
//     {
//         return;
//     }
//     dict->entries = New(Entry, dict->capacity);
//     for (int i = 0; i < dict->count; i++)
//     {
//         entryDeserialize(vm, &dict->entries[i], stream);
//     }
//     return;
// }

void functionSerialize(MyMoFunction *function, FILE *stream)
{
    fwrite(&function->type, sizeof(function->type), 1, stream);
    fwrite(&function->argc, sizeof(function->argc), 1, stream);
    stringSerialize(function->name, stream);
    for (int i = 0; i < function->argc; i++)
    {
        stringSerialize(function->argv[i], stream);
    }
    // DictSerialize(function->assiginedParameters, stream);
    // DictSerialize(function->variables, stream);
    chunkSerialize(function->chunk, stream);
}

MyMoFunction *functionDeserialize(MVM *vm, FILE *stream)
{
    MyMoFunction *function = newFunction(vm);
    fread(&function->type, sizeof(function->type), 1, stream);
    fread(&function->argc, sizeof(function->argc), 1, stream);
    function->name = (MyMoString *)stringDeserialize(vm, stream);
    for (int i = 0; i < function->argc; i++)
    {
        function->argv[i] = (MyMoString *)stringDeserialize(vm, stream);
    }
    // DictDeserialize(vm, function->assiginedParameters, stream);
    // DictDeserialize(vm, function->variables, stream);
    chunkDeserialize(vm, function->chunk, stream);
    return function;
}