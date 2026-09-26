#include "iter.h"
#include "../vm.h"
#include "datatypes.h"

MyMoIter *newIter(MVM *vm, MyMoObject *object)
{
  MyMoIter *iter = AllocateObject(vm, MyMoIter, OBJ_ITER);
  iter->iterator = object;
  iter->index = 0;
  return iter;
}

// Next element as a Value (inline for ints/nil/bool/...), or V_EMPTY_VAL
// when the iteration is done.
Value nextIter(MVM *vm, MyMoIter *object)
{
  MyMoObject *iterator = object->iterator;
  switch (iterator->type)
  {
  case OBJ_STRING:
  {
    MyMoString *string = AS_STRING(iterator);
    if (object->index >= string->length)
    {
      return V_EMPTY_VAL;
    }
    return V_OBJ_VAL(NEW_STRING(vm, string->value + object->index++, 1));
  }
  case OBJ_LIST:
  {
    MyMoList *list = AS_LIST(iterator);
    if (object->index >= list->values.count)
    {
      return V_EMPTY_VAL;
    }
    return list->values.values[object->index++];
  }
  case OBJ_TUPLE:
  {
    MyMoTuple *tuple = AS_TUPLE(iterator);
    if (object->index >= tuple->values.count)
    {
      return V_EMPTY_VAL;
    }
    return tuple->values.values[object->index++];
  }
  case OBJ_DICT:
  {
    // Walk the entry table; skip empty (key==NULL) slots. Yields
    // keys in insertion order via the entries[] index, matching
    // for-in semantics over a list of keys. Stops at the first
    // slot past the table (capacity is the index mask: size - 1).
    MyMoDict *dict = AS_DICT(iterator);
    while (object->index <= dict->capacity)
    {
      Entry *e = &dict->entries[object->index++];
      if (e->key != NULL) return V_OBJ_VAL(e->key);
    }
    return V_EMPTY_VAL;
  }
  default:
    return V_EMPTY_VAL;
  }
}

void printIter(MyMoIter *object)
{
  switch (object->iterator->type)
  {
  case OBJ_STRING:
    printf("<string_iterator at %p>", object);
    break;
  case OBJ_LIST:
    printf("<list_iterator at %p>", object);
    break;
  case OBJ_TUPLE:
    printf("<tuple_iterator at %p>", object);
    break;
  case OBJ_DICT:
    printf("<dict_iterator at %p>", object);
    break;
  case OBJ_INSTANCE:
    printInstance(AS_INSTANCE(object->iterator));
    break;
  default:
    printf("Unknown type %d", object->iterator->type);
    printf("%p", object);
    break;
  }
}