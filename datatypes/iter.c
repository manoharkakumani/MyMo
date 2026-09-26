#include "iter.h"
#include "../vm.h"
#include "datatypes.h"

MyMoIter *newIter(MVM *vm, MyMoObject *object)
{
  MyMoIter *iter = AllocateObject(vm, MyMoIter, OBJ_ITER);
  iter->index = 0;
  if (object->type == OBJ_DICT)
  {
    // Iterate a snapshot of the keys (in insertion order), so the loop
    // body may add or delete keys safely.
    MyMoDict *dict = AS_DICT(object);
    MyMoTuple *keys = newTuple(vm);
    iter->iterator = AS_OBJECT(keys);
    Entry *e;
    DICT_FOREACH(dict, e)
      writeValueArray(vm, &keys->values, objectToValue(e->key));
    return iter;
  }
  iter->iterator = object;
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
  case OBJ_RANGE:
  {
    MyMoRange *range = AS_RANGE(iterator);
    if (object->index >= rangeLength(range))
      return V_EMPTY_VAL;
    return valueFromLong(vm, rangeAt(range, object->index++));
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