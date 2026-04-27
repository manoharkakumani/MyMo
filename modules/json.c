// modules/json.c — built-in `json` module: encode/decode JSON over the
// standard MyMo value types.
//
// Exports:
//   json.encode(value) -> string
//   json.decode(s)     -> any (int / double / string / bool / nil / list / dict)
//
// Mapping:
//   MyMo int / double  <-> JSON number
//   MyMo string        <-> JSON string
//   MyMo True / False  <-> true / false
//   MyMo Nil           <-> null
//   MyMo list          <-> JSON array
//   MyMo dict          <-> JSON object  (string keys only on encode)

#include "../include/mymo_module.h"
#include "../datatypes/dict.h"
#include "../datatypes/list.h"
#include "../datatypes/tuple.h"
#include "../datatypes/string.h"
#include "../datatypes/int.h"
#include "../datatypes/double.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

// --------------------------------------------------------------------
// Encoder (object -> string)
// --------------------------------------------------------------------

typedef struct
{
    char  *data;
    size_t len;
    size_t cap;
} Buf;

static int buf_reserve(Buf *b, size_t need)
{
    if (b->len + need + 1 <= b->cap) return 0;
    size_t newcap = b->cap ? b->cap : 256;
    while (newcap < b->len + need + 1) newcap *= 2;
    char *p = realloc(b->data, newcap);
    if (!p) return -1;
    b->data = p;
    b->cap  = newcap;
    return 0;
}

static int buf_putc(Buf *b, char c) { if (buf_reserve(b, 1)) return -1; b->data[b->len++] = c; return 0; }
static int buf_puts(Buf *b, const char *s, size_t n) { if (buf_reserve(b, n)) return -1; memcpy(b->data + b->len, s, n); b->len += n; return 0; }

static int enc_string(Buf *b, const char *s, int len)
{
    if (buf_putc(b, '"')) return -1;
    for (int i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
            case '"':  buf_puts(b, "\\\"", 2); break;
            case '\\': buf_puts(b, "\\\\", 2); break;
            case '\b': buf_puts(b, "\\b", 2);  break;
            case '\f': buf_puts(b, "\\f", 2);  break;
            case '\n': buf_puts(b, "\\n", 2);  break;
            case '\r': buf_puts(b, "\\r", 2);  break;
            case '\t': buf_puts(b, "\\t", 2);  break;
            default:
                if (c < 0x20) {
                    char esc[8];
                    int n = snprintf(esc, sizeof(esc), "\\u%04x", c);
                    buf_puts(b, esc, (size_t)n);
                } else {
                    buf_putc(b, (char)c);
                }
        }
    }
    return buf_putc(b, '"');
}

static int enc_value(Buf *b, MyMoObject *v);

static int enc_array(Buf *b, MyMoObjectArray *arr)
{
    if (buf_putc(b, '[')) return -1;
    for (int i = 0; i < arr->count; i++) {
        if (i > 0) buf_putc(b, ',');
        if (enc_value(b, arr->objects[i])) return -1;
    }
    return buf_putc(b, ']');
}

static int enc_dict(Buf *b, MyMoDict *d)
{
    if (buf_putc(b, '{')) return -1;
    int first = 1;
    for (int i = 0; i <= d->capacity; i++) {
        Entry *e = &d->entries[i];
        if (!e->key) continue;
        if (!first) buf_putc(b, ',');
        first = 0;
        if (e->key->type != OBJ_STRING) return -2;  // non-string key
        MyMoString *k = (MyMoString *)e->key;
        if (enc_string(b, k->value, k->length)) return -1;
        buf_putc(b, ':');
        // Value can be inline (Value) — go through V_*
        if (V_IS_INT(e->value)) {
            char num[32];
            int n = snprintf(num, sizeof(num), "%d", V_AS_INT(e->value));
            buf_puts(b, num, (size_t)n);
        } else if (V_IS_OBJ(e->value)) {
            if (enc_value(b, V_AS_OBJ(e->value))) return -1;
        } else if (V_IS_NIL(e->value)) {
            buf_puts(b, "null", 4);
        } else if (V_IS_TRUE(e->value)) {
            buf_puts(b, "true", 4);
        } else if (V_IS_FALSE(e->value)) {
            buf_puts(b, "false", 5);
        } else if (V_IS_DOUBLE(e->value)) {
            char num[32];
            int n = snprintf(num, sizeof(num), "%.17g", V_AS_DOUBLE(e->value));
            buf_puts(b, num, (size_t)n);
        }
    }
    return buf_putc(b, '}');
}

static int enc_value(Buf *b, MyMoObject *v)
{
    if (!v) { buf_puts(b, "null", 4); return 0; }
    switch (v->type) {
        case OBJ_NIL:    return buf_puts(b, "null", 4);
        case OBJ_BOOL:   return buf_puts(b,
                            ((MyMoBool *)v)->value ? "true" : "false",
                            ((MyMoBool *)v)->value ? 4 : 5);
        case OBJ_INT: {
            char num[32];
            int n = snprintf(num, sizeof(num), "%ld", ((MyMoInt *)v)->value);
            return buf_puts(b, num, (size_t)n);
        }
        case OBJ_DOUBLE: {
            char num[32];
            int n = snprintf(num, sizeof(num), "%.17g", ((MyMoDouble *)v)->value);
            return buf_puts(b, num, (size_t)n);
        }
        case OBJ_STRING: {
            MyMoString *s = (MyMoString *)v;
            return enc_string(b, s->value, s->length);
        }
        case OBJ_LIST:   return enc_array(b, &((MyMoList *)v)->values);
        case OBJ_TUPLE:  return enc_array(b, &((MyMoTuple *)v)->values);
        case OBJ_DICT:   return enc_dict(b, (MyMoDict *)v);
        default:
            return -2;  // unsupported type
    }
}

static MyMoObject *json_encode(MVM *vm, uint argc, MyMoObject *argv[])
{
    if (!mymo_check_args(vm, "json.encode", argc, 1)) return MYMO_ERROR;
    Buf b = {0};
    int rc = enc_value(&b, argv[0]);
    if (rc != 0) {
        free(b.data);
        runtimeError(vm, "json.encode(): %s",
                     rc == -2 ? "value not JSON-encodable (e.g. function or non-string dict key)"
                              : "out of memory");
        return MYMO_ERROR;
    }
    MyMoObject *s = mymo_strn(vm, b.data ? b.data : "", (int)b.len);
    free(b.data);
    return s;
}

// --------------------------------------------------------------------
// Decoder (string -> object)
// --------------------------------------------------------------------

typedef struct
{
    const char *s;
    size_t      len;
    size_t      pos;
    MVM        *vm;
    const char *err;
} Parser;

static void p_skipws(Parser *p)
{
    while (p->pos < p->len) {
        char c = p->s[p->pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') p->pos++;
        else break;
    }
}

static MyMoObject *parse_value(Parser *p);

static MyMoObject *parse_string(Parser *p)
{
    if (p->pos >= p->len || p->s[p->pos] != '"') { p->err = "expected '\"'"; return NULL; }
    p->pos++;
    Buf b = {0};
    while (p->pos < p->len) {
        char c = p->s[p->pos++];
        if (c == '"') {
            MyMoObject *r = mymo_strn(p->vm, b.data ? b.data : "", (int)b.len);
            free(b.data);
            return r;
        }
        if (c == '\\') {
            if (p->pos >= p->len) { free(b.data); p->err = "bad escape"; return NULL; }
            char e = p->s[p->pos++];
            switch (e) {
                case '"':  buf_putc(&b, '"'); break;
                case '\\': buf_putc(&b, '\\'); break;
                case '/':  buf_putc(&b, '/'); break;
                case 'b':  buf_putc(&b, '\b'); break;
                case 'f':  buf_putc(&b, '\f'); break;
                case 'n':  buf_putc(&b, '\n'); break;
                case 'r':  buf_putc(&b, '\r'); break;
                case 't':  buf_putc(&b, '\t'); break;
                case 'u': {
                    if (p->pos + 4 > p->len) { free(b.data); p->err = "bad \\u escape"; return NULL; }
                    unsigned cp = 0;
                    for (int k = 0; k < 4; k++) {
                        char h = p->s[p->pos++];
                        cp <<= 4;
                        if (h >= '0' && h <= '9') cp |= (unsigned)(h - '0');
                        else if (h >= 'a' && h <= 'f') cp |= (unsigned)(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') cp |= (unsigned)(h - 'A' + 10);
                        else { free(b.data); p->err = "bad \\u hex"; return NULL; }
                    }
                    // UTF-8 encode (BMP only — surrogate pairs not handled)
                    if (cp < 0x80) buf_putc(&b, (char)cp);
                    else if (cp < 0x800) {
                        buf_putc(&b, (char)(0xC0 | (cp >> 6)));
                        buf_putc(&b, (char)(0x80 | (cp & 0x3F)));
                    } else {
                        buf_putc(&b, (char)(0xE0 | (cp >> 12)));
                        buf_putc(&b, (char)(0x80 | ((cp >> 6) & 0x3F)));
                        buf_putc(&b, (char)(0x80 | (cp & 0x3F)));
                    }
                    break;
                }
                default: free(b.data); p->err = "bad escape"; return NULL;
            }
        } else {
            buf_putc(&b, c);
        }
    }
    free(b.data);
    p->err = "unterminated string";
    return NULL;
}

static MyMoObject *parse_number(Parser *p)
{
    size_t start = p->pos;
    if (p->s[p->pos] == '-') p->pos++;
    int is_float = 0;
    while (p->pos < p->len && isdigit((unsigned char)p->s[p->pos])) p->pos++;
    if (p->pos < p->len && p->s[p->pos] == '.') {
        is_float = 1; p->pos++;
        while (p->pos < p->len && isdigit((unsigned char)p->s[p->pos])) p->pos++;
    }
    if (p->pos < p->len && (p->s[p->pos] == 'e' || p->s[p->pos] == 'E')) {
        is_float = 1; p->pos++;
        if (p->pos < p->len && (p->s[p->pos] == '+' || p->s[p->pos] == '-')) p->pos++;
        while (p->pos < p->len && isdigit((unsigned char)p->s[p->pos])) p->pos++;
    }
    char tmp[64];
    size_t n = p->pos - start;
    if (n >= sizeof(tmp)) { p->err = "number too long"; return NULL; }
    memcpy(tmp, p->s + start, n);
    tmp[n] = '\0';
    if (is_float) return mymo_double(p->vm, strtod(tmp, NULL));
    return mymo_int(p->vm, strtol(tmp, NULL, 10));
}

static MyMoObject *parse_array(Parser *p)
{
    p->pos++;  // consume '['
    MyMoList *list = newList(p->vm);
    p_skipws(p);
    if (p->pos < p->len && p->s[p->pos] == ']') { p->pos++; return AS_OBJECT(list); }
    while (p->pos < p->len) {
        p_skipws(p);
        MyMoObject *v = parse_value(p);
        if (!v) return NULL;
        writeMyMoObjectArray(p->vm, &list->values, v);
        p_skipws(p);
        if (p->pos < p->len && p->s[p->pos] == ',') { p->pos++; continue; }
        if (p->pos < p->len && p->s[p->pos] == ']') { p->pos++; return AS_OBJECT(list); }
        p->err = "expected ',' or ']'";
        return NULL;
    }
    p->err = "unterminated array";
    return NULL;
}

static MyMoObject *parse_object(Parser *p)
{
    p->pos++;  // consume '{'
    MyMoDict *dict = newDict(p->vm);
    p_skipws(p);
    if (p->pos < p->len && p->s[p->pos] == '}') { p->pos++; return AS_OBJECT(dict); }
    while (p->pos < p->len) {
        p_skipws(p);
        MyMoObject *key = parse_string(p);
        if (!key) return NULL;
        p_skipws(p);
        if (p->pos >= p->len || p->s[p->pos] != ':') { p->err = "expected ':'"; return NULL; }
        p->pos++;
        p_skipws(p);
        MyMoObject *val = parse_value(p);
        if (!val) return NULL;
        setEntry(p->vm, dict, key, val);
        p_skipws(p);
        if (p->pos < p->len && p->s[p->pos] == ',') { p->pos++; continue; }
        if (p->pos < p->len && p->s[p->pos] == '}') { p->pos++; return AS_OBJECT(dict); }
        p->err = "expected ',' or '}'";
        return NULL;
    }
    p->err = "unterminated object";
    return NULL;
}

static MyMoObject *parse_value(Parser *p)
{
    p_skipws(p);
    if (p->pos >= p->len) { p->err = "unexpected end of input"; return NULL; }
    char c = p->s[p->pos];
    if (c == '"') return parse_string(p);
    if (c == '{') return parse_object(p);
    if (c == '[') return parse_array(p);
    if (c == '-' || (c >= '0' && c <= '9')) return parse_number(p);
    if (c == 't' && p->pos + 4 <= p->len && memcmp(p->s + p->pos, "true", 4) == 0)
        { p->pos += 4; return MYMO_TRUE; }
    if (c == 'f' && p->pos + 5 <= p->len && memcmp(p->s + p->pos, "false", 5) == 0)
        { p->pos += 5; return MYMO_FALSE; }
    if (c == 'n' && p->pos + 4 <= p->len && memcmp(p->s + p->pos, "null", 4) == 0)
        { p->pos += 4; return MYMO_NIL; }
    p->err = "unexpected character";
    return NULL;
}

static MyMoObject *json_decode(MVM *vm, uint argc, MyMoObject *argv[])
{
    const char *s; int slen;
    if (!mymo_parse(vm, "json.decode", argc, argv, "sn", &s, &slen))
        return MYMO_ERROR;
    Parser p = { s, (size_t)slen, 0, vm, NULL };
    MyMoObject *r = parse_value(&p);
    if (!r) {
        runtimeError(vm, "json.decode(): %s at byte %zu",
                     p.err ? p.err : "parse error", p.pos);
        return MYMO_ERROR;
    }
    p_skipws(&p);
    if (p.pos != p.len) {
        runtimeError(vm, "json.decode(): trailing data at byte %zu", p.pos);
        return MYMO_ERROR;
    }
    return r;
}

MyMoObject *jsonModule(MVM *vm)
{
    static MyMoModuleFunction fns[] = {
        {"encode", json_encode},
        {"decode", json_decode},
    };
    static MyMoModuleVariable vars[] = { {0, 0} };
    static MyMoModuleDef def = {
        "json", fns, vars,
        sizeof(fns) / sizeof(fns[0]), 0,
    };
    return defineBuiltInModule(vm, &def);
}
