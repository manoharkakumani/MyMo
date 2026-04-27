// mymath.c — example MyMo extension module exporting CONSTANTS in
// addition to functions.
//
// Demonstrates `MYMO_MODULE_EX` with an init block that registers
// module-level variables via mymo_set_double / mymo_set_str.
//
// Build (from this directory):
//   cc -shared -fPIC -o mymathmod.dylib mymath.c -I../../include   # macOS
//   cc -shared -fPIC -o mymathmod.so    mymath.c -I../../include   # Linux
//
// Use:
//   from "mymath" use PI, E, VERSION, area, circumference
//   print(PI)               # 3.14159265358979
//   print(area(5))          # 78.5398...
//   print(circumference(5)) # 31.4159...
//   print(VERSION)          # 1.0.0

#include "mymo_module.h"

static MyMoObject *area(MVM *vm, uint argc, MyMoObject *argv[])
{
    double r;
    if (!mymo_parse(vm, "area", argc, argv, "d", &r)) return MYMO_ERROR;
    return mymo_double(vm, 3.14159265358979 * r * r);
}

static MyMoObject *circumference(MVM *vm, uint argc, MyMoObject *argv[])
{
    double r;
    if (!mymo_parse(vm, "circumference", argc, argv, "d", &r)) return MYMO_ERROR;
    return mymo_double(vm, 2.0 * 3.14159265358979 * r);
}

MYMO_MODULE_EX(mymath,
    {
        mymo_set_double(vm, _mod, "PI",      3.14159265358979);
        mymo_set_double(vm, _mod, "E",       2.71828182845905);
        mymo_set_double(vm, _mod, "TAU",     6.28318530717958);
        mymo_set_int   (vm, _mod, "MAX_INT", 9223372036854775807L);
        mymo_set_str   (vm, _mod, "VERSION", "1.0.0");
    },
    MYMO_FN(area),
    MYMO_FN(circumference),
)
