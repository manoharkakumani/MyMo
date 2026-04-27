// modules/sqlite.c — built-in `sqlite` module: SQLite3 wrapper.
//
// Exports:
//   sqlite.open(path)         -> int handle  (use ":memory:" for in-memory)
//   sqlite.run(h, sql)        -> int rows-affected (DDL, INSERT, UPDATE, DELETE)
//   sqlite.query(h, sql)      -> list of dicts (SELECT)
//   sqlite.close(h)           -> nil
//
// Handles are small ints (slot indices into a static pool, max 64 open
// connections). Closing a handle frees the slot.
//
// Build dependency: -lsqlite3 (preinstalled on macOS; libsqlite3-dev on Linux).

#include "../include/mymo_module.h"
#include "../datatypes/list.h"
#include "../datatypes/dict.h"
#include "../datatypes/string.h"
#include <sqlite3.h>
#include <string.h>

#define DB_POOL_MAX 64
static sqlite3 *db_pool[DB_POOL_MAX];

static int alloc_slot(sqlite3 *db)
{
    for (int i = 0; i < DB_POOL_MAX; i++)
        if (!db_pool[i]) { db_pool[i] = db; return i; }
    return -1;
}

static sqlite3 *get_slot(MVM *vm, const char *fn, long h)
{
    if (h < 0 || h >= DB_POOL_MAX || !db_pool[h]) {
        runtimeError(vm, "%s(): invalid sqlite handle %ld", fn, h);
        return NULL;
    }
    return db_pool[h];
}

static MyMoObject *sql_open(MVM *vm, uint argc, MyMoObject *argv[])
{
    const char *path;
    if (!mymo_parse(vm, "sqlite.open", argc, argv, "s", &path)) return MYMO_ERROR;
    sqlite3 *db = NULL;
    if (sqlite3_open(path, &db) != SQLITE_OK) {
        const char *msg = db ? sqlite3_errmsg(db) : "open failed";
        char buf[256];
        snprintf(buf, sizeof(buf), "%s", msg);
        if (db) sqlite3_close(db);
        runtimeError(vm, "sqlite.open('%s'): %s", path, buf);
        return MYMO_ERROR;
    }
    int slot = alloc_slot(db);
    if (slot < 0) {
        sqlite3_close(db);
        runtimeError(vm, "sqlite.open(): too many open connections (max %d)", DB_POOL_MAX);
        return MYMO_ERROR;
    }
    return mymo_int(vm, slot);
}

static MyMoObject *sql_run(MVM *vm, uint argc, MyMoObject *argv[])
{
    long h;
    const char *sql;
    if (!mymo_parse(vm, "sqlite.run", argc, argv, "is", &h, &sql))
        return MYMO_ERROR;
    sqlite3 *db = get_slot(vm, "sqlite.run", h);
    if (!db) return MYMO_ERROR;
    char *err = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        char msg[512];
        snprintf(msg, sizeof(msg), "%s", err ? err : "run failed");
        sqlite3_free(err);
        runtimeError(vm, "sqlite.run(): %s", msg);
        return MYMO_ERROR;
    }
    return mymo_int(vm, sqlite3_changes(db));
}

static MyMoObject *column_to_object(MVM *vm, sqlite3_stmt *st, int i)
{
    switch (sqlite3_column_type(st, i)) {
        case SQLITE_INTEGER:
            return mymo_int(vm, (long)sqlite3_column_int64(st, i));
        case SQLITE_FLOAT:
            return mymo_double(vm, sqlite3_column_double(st, i));
        case SQLITE_TEXT: {
            const char *t = (const char *)sqlite3_column_text(st, i);
            int n = sqlite3_column_bytes(st, i);
            return mymo_strn(vm, t, n);
        }
        case SQLITE_NULL:
            return MYMO_NIL;
        case SQLITE_BLOB: {
            const void *blob = sqlite3_column_blob(st, i);
            int n = sqlite3_column_bytes(st, i);
            return mymo_strn(vm, (const char *)blob, n);
        }
        default:
            return MYMO_NIL;
    }
}

static MyMoObject *sql_query(MVM *vm, uint argc, MyMoObject *argv[])
{
    long h;
    const char *sql;
    if (!mymo_parse(vm, "sqlite.query", argc, argv, "is", &h, &sql))
        return MYMO_ERROR;
    sqlite3 *db = get_slot(vm, "sqlite.query", h);
    if (!db) return MYMO_ERROR;

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        runtimeError(vm, "sqlite.query(): %s", sqlite3_errmsg(db));
        return MYMO_ERROR;
    }
    MyMoList *rows = newList(vm);
    int ncols = sqlite3_column_count(st);
    while (sqlite3_step(st) == SQLITE_ROW) {
        MyMoDict *row = newDict(vm);
        for (int i = 0; i < ncols; i++) {
            const char *colname = sqlite3_column_name(st, i);
            MyMoObject *key = AS_OBJECT(newString(vm, colname, (int)strlen(colname)));
            setEntry(vm, row, key, column_to_object(vm, st, i));
        }
        writeMyMoObjectArray(vm, &rows->values, AS_OBJECT(row));
    }
    sqlite3_finalize(st);
    return AS_OBJECT(rows);
}

static MyMoObject *sql_close(MVM *vm, uint argc, MyMoObject *argv[])
{
    long h;
    if (!mymo_parse(vm, "sqlite.close", argc, argv, "i", &h)) return MYMO_ERROR;
    sqlite3 *db = get_slot(vm, "sqlite.close", h);
    if (!db) return MYMO_ERROR;
    sqlite3_close(db);
    db_pool[h] = NULL;
    return MYMO_NIL;
}

MyMoObject *sqliteModule(MVM *vm)
{
    static MyMoModuleFunction fns[] = {
        {"open",  sql_open},
        {"run",   sql_run},
        {"query", sql_query},
        {"close", sql_close},
    };
    static MyMoModuleVariable vars[] = { {0, 0} };
    static MyMoModuleDef def = {
        "sqlite", fns, vars,
        sizeof(fns) / sizeof(fns[0]), 0,
    };
    return defineBuiltInModule(vm, &def);
}
