#include "../include/mymo_module.h"
#include "modules.h"
#include <math.h>


#define FLOAT_TOLERANCE 0.00001


// One numeric argument (int or double) in, result out as an inline value.
static bool mathArg(MVM *vm, const char *name, uint argc, Value argv[], double *x)
{
    return mymo_parse(vm, name, argc, argv, "d", x);
}

Value floorfn(MVM *vm, uint argc, Value argv[])
{
    double x;
    if (!mathArg(vm, "math.floor", argc, argv, &x)) return V_EMPTY_VAL;
    return valueFromLong(vm, (long)floor(x));
}

Value ceilfn(MVM *vm, uint argc, Value argv[])
{
    double x;
    if (!mathArg(vm, "math.ceil", argc, argv, &x)) return V_EMPTY_VAL;
    return valueFromLong(vm, (long)ceil(x));
}

Value sqrtfn(MVM *vm, uint argc, Value argv[])
{
    double x;
    if (!mathArg(vm, "math.sqrt", argc, argv, &x)) return V_EMPTY_VAL;
    return V_DOUBLE_VAL(sqrt(x));
}

Value sinfn(MVM *vm, uint argc, Value argv[])
{
    double x;
    if (!mathArg(vm, "math.sin", argc, argv, &x)) return V_EMPTY_VAL;
    return V_DOUBLE_VAL(sin(x));
}

Value cosfn(MVM *vm, uint argc, Value argv[])
{
    double x;
    if (!mathArg(vm, "math.cos", argc, argv, &x)) return V_EMPTY_VAL;
    return V_DOUBLE_VAL(cos(x));
}

Value tanfn(MVM *vm, uint argc, Value argv[])
{
    double x;
    if (!mathArg(vm, "math.tan", argc, argv, &x)) return V_EMPTY_VAL;
    return V_DOUBLE_VAL(tan(x));
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

// MyMoObject *gcdfn(MVM *vm, uint argc, Value argv[]) {
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

// MyMoObject *lcmfn(MVM *vm, uint argc, Value argv[]) {
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