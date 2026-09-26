#ifndef __DATATYPES_H__
#define __DATATYPES_H__

#include "object.h"
#include "nil.h"
#include "bool.h"
#include "int.h"
#include "double.h"
#include "string.h"
#include "list.h"
#include "tuple.h"
#include "dict.h"
#include "function.h"
#include "class.h"
#include "module.h"
#include "code.h"
#include "fiber.h"
#include "iter.h"
#include "range.h"
#include "set.h"

void defineBuiltInClasses(MVM *vm);

#endif