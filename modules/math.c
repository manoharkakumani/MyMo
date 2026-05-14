#include "modules.h"
#include <math.h>


#define FLOAT_TOLERANCE 0.00001


MyMoObject *floorfn(MVM *vm, uint argc, MyMoObject *argv[])
{
    if (argc != 1)
    {
        runtimeError(vm, "TypeError: math.floor()  takes 1 argument (%d given).", argc);
        return NEW_EMPTY;
    }
    if (!IS_NUMBER(argv[0]))
    {
        runtimeError(vm, "TypeError: must be <object 'int'> or <object 'double'>, not (%s)",getType(argv[0]));
        return NEW_EMPTY;
    }
    // NUMBER_VAL is a macro that expands its argument three times,
    // so `NUMBER_VAL(pop(vm))` used to pop three values off the
    // stack and segfault. Pop once into a local, then read it.
    MyMoObject *object = pop(vm);
    return NEW_INT(vm, floor(NUMBER_VAL(object)));
}

MyMoObject *ceilfn(MVM *vm, uint argc, MyMoObject *argv[]) 
{
    if (argc != 1) 
    {
        runtimeError(vm, "TypeError: math.ceil() takes 1 argument (%d given).", argc);
        return NEW_EMPTY;
    }

    if (!IS_NUMBER(argv[0])) 
    {
        runtimeError(vm, "TypeError: must be <object 'int'> or <object 'double'>, not (%s)",getType(argv[0]));
        return NEW_EMPTY;
    }
    MyMoObject *object = pop(vm);
    return NEW_INT(vm, ceil(NUMBER_VAL(object)));
}

MyMoObject *sqrtfn(MVM *vm, uint argc, MyMoObject *argv[]) {
    if (argc != 1) {
        runtimeError(vm, "TypeError: math.sqrt() takes 1 argument (%d given).", argc);
        return NEW_EMPTY;
    }
    if (!IS_NUMBER(argv[0])) {
        runtimeError(vm, "TypeError: must be <object 'int'> or <object 'double'>, not (%s)",getType(argv[0]));
        return NEW_EMPTY;
    }
    MyMoObject *object = pop(vm);
    return NEW_DOUBLE(vm, sqrt(NUMBER_VAL(object)));
}

MyMoObject *sinfn(MVM *vm, uint argc, MyMoObject *argv[]) {
    if (argc != 1) {
        runtimeError(vm, "TypeError: math.sin() takes 1 argument (%d given).", argc);
        return NEW_EMPTY;
    }
    if (!IS_NUMBER(argv[0])) {
        runtimeError(vm, "A non-number value passed to sin()");
        return NEW_EMPTY;
    }
    MyMoObject *object = pop(vm);
    return NEW_DOUBLE(vm, sin(NUMBER_VAL(object)));
}

MyMoObject *cosfn(MVM *vm, uint argc, MyMoObject *argv[]) {
    if (argc != 1) {
        runtimeError(vm, "TypeError: math.cos() takes 1 argument (%d given).", argc);
        return NEW_EMPTY;
    }

    if (!IS_NUMBER(argv[0])) {
        runtimeError(vm, "A non-number value passed to cos()");
        return NEW_EMPTY;
    }
    MyMoObject *object = pop(vm);
    return NEW_DOUBLE(vm, cos(NUMBER_VAL(object)));
}

MyMoObject *tanfn(MVM *vm, uint argc, MyMoObject *argv[]) {
    if (argc != 1) {
        runtimeError(vm, "TypeError: math.tan() takes 1 argument (%d given).", argc);
        return NEW_EMPTY;
    }

    if (!IS_NUMBER(argv[0])) {
        runtimeError(vm, "A non-number value passed to tan()");
        return NEW_EMPTY;
    }
    MyMoObject *object = pop(vm);
    return NEW_DOUBLE(vm, tan(NUMBER_VAL(object)));
}

// static long long gcd(long long a, long long b) {
//     long long r;
//     while (b > 0) {
//         r = a % b;
//         a = b;
//         b = r;
//     }
//     return a;
// }

// MyMoObject *gcdfn(MVM *vm, uint argc, MyMoObject *argv[]) {
//     char* argcError = "gcd() requires 2 or more arguments (%d given).";
//     char* nonNumberError = "gcd() argument at index %d is not a number";
//     char* notWholeError = "gcd() argument (%f) at index %d is not a whole number";

//     if (argc == 1 && IS_LIST(argv[0])) {
//         argcError = "List passed to gcd() must have 2 or more elements (%d given).";
//         nonNumberError = "The element at index %d of the list passed to gcd() is not a number";
//         notWholeError = "The element (%f) at index %d of the list passed to gcd() is not a whole number";
//         ObjList *list = AS_LIST(argv[0]);
//         argc = list->values.count;
//         argv = list->values.values;
//     }

//     if (argc < 2) {
//         runtimeError(vm, argcError, argc);
//         return NEW_EMPTY;
//     }

//     for (int i = 0; i < argc; ++i)
//         if (!IS_NUMBER(argv[i])) {
//             runtimeError(vm, nonNumberError, i);
//             return NEW_EMPTY;
//         }

//     double* as_doubles = ALLOCATE(vm, double, argc);
//     for (int i = 0; i < argc; ++i) {
//         as_doubles[i] = NUMBER_VAL(argv[i]);
//         if (fabs(round(as_doubles[i]) - as_doubles[i]) > FLOAT_TOLERANCE) {
//             runtimeError(vm, notWholeError, as_doubles[i], i);
//             FREE_ARRAY(vm, double, as_doubles, argc);
//             return NEW_EMPTY;
//         }
//     }

//     long long* as_longlongs = ALLOCATE(vm, long long, argc);
//     for (int i = 0; i < argc; ++i) as_longlongs[i] = round(as_doubles[i]);

//     long long result = as_longlongs[0];
//     for (int i = 1; i < argc; ++i) result = gcd(result, as_longlongs[i]);

//     FREE_ARRAY(vm, double, as_doubles, argc);
//     FREE_ARRAY(vm, long long, as_longlongs, argc);
//     return NUMBER_VAL(result);
// }

// long long lcm(long long a, long long b) {
//     return (a * b) / gcd(a, b);
// }

// MyMoObject *lcmfn(MVM *vm, uint argc, MyMoObject *argv[]) {
//     char* argcError = "lcm() requires 2 or more arguments (%d given).";
//     char* nonNumberError = "lcm() argument at index %d is not a number";
//     char* notWholeError = "lcm() argument (%f) at index %d is not a whole number";

//     if (argc == 1 && IS_LIST(argv[0])) {
//         argcError = "List passed to lcm() must have 2 or more elements (%d given).";
//         nonNumberError = "The element at index %d of the list passed to lcm() is not a number";
//         notWholeError = "The element (%f) at index %d of the list passed to lcm() is not a whole number";
//         ObjList *list = AS_LIST(argv[0]);
//         argc = list->values.count;
//         argv = list->values.values;
//     }

//     if (argc < 2) {
//         runtimeError(vm, argcError, argc);
//         return NEW_EMPTY;
//     }

//     for (int i = 0; i < argc; ++i)
//         if (!IS_NUMBER(argv[i])) {
//             runtimeError(vm, nonNumberError, i);
//             return NEW_EMPTY;
//         }

//     double* as_doubles = ALLOCATE(vm, double, argc);
//     for (int i = 0; i < argc; ++i) {
//         as_doubles[i] = NUMBER_VAL(argv[i]);
//         if (fabs(round(as_doubles[i]) - as_doubles[i]) > FLOAT_TOLERANCE) {
//             runtimeError(vm, notWholeError, as_doubles[i], i);
//             FREE_ARRAY(vm, double, as_doubles, argc);
//             return NEW_EMPTY;
//         }
//     }

//     long long* as_longlongs = ALLOCATE(vm, long long, argc);
//     for (int i = 0; i < argc; ++i) as_longlongs[i] = round(as_doubles[i]);

//     long long result = as_longlongs[0];
//     for (int i = 1; i < argc; ++i) result = lcm(result, as_longlongs[i]);

//     FREE_ARRAY(vm, double, as_doubles, argc);
//     FREE_ARRAY(vm, long long, as_longlongs, argc);
//     return NUMBER_VAL(result);
// }

MODULE(math)
{
    MyMoModuleFunction functions [] = {
                                    {"floor", floorfn},
                                    {"ceil", ceilfn},
                                    {"sqrt", sqrtfn},
                                    {"sin", sinfn},
                                    {"cos", cosfn},
                                    {"tan", tanfn}
                                    };

    MyMoModuleVariable variables [] = {
                                    {"pi", NEW_DOUBLE(vm, 3.141592653589793238462643383279502884197)},
                                    {"logpi", NEW_DOUBLE(vm, 1.144729885849400174143427351353058711647)},
                                    {"e", NEW_DOUBLE(vm, 2.71828182845905)},
                                    {"phi", NEW_DOUBLE(vm, 1.61803398874989)},
                                    {"sqrt2", NEW_DOUBLE(vm, 1.41421356237309)},
                                    {"sqrte", NEW_DOUBLE(vm, 1.61803398874989)},
                                    {"sqrtpi", NEW_DOUBLE(vm, 1.77245385090551)},
                                    {"sqrtphi", NEW_DOUBLE(vm, 1.27201964951406)},
                                    {"ln2", NEW_DOUBLE(vm, 0.69314718055994)},
                                    {"ln10", NEW_DOUBLE(vm, 2.30258509299404)}
                                    };

    MyMoModuleDef moduleDef = {
        "math",
        functions,
        variables,
        sizeof functions / sizeof functions[0],
        sizeof variables / sizeof variables[0]
    };

   return defineBuiltInModule(vm, &moduleDef);
}