// format.c — the format-spec mini-language used by f-strings
// ({value:spec}), format(value, spec) and str.format().
//
//     [[fill]align][sign][#][0][width][grouping][.precision][type]
//
// align: < left, > right, ^ centre, = pad after the sign.
// sign: + (always), - (negatives only, default), space.
// # adds 0x/0o/0b prefixes; 0 pads numbers with zeros after the sign.
// grouping: , or _ between thousands.
// type: s (string); d x X o b c (integers); f F e E g G % (floats).
// Without a type, ints format like d, doubles like repr (or g when a
// precision is given) and everything else like str().

#include "format.h"
#include "repr.h"
#include "vm.h"
#include "datatypes/datatypes.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
    char fill;
    char align; // 0 = default for the type
    char sign;  // '+', '-' or ' '
    bool alternate;
    int width;
    char grouping; // 0, ',' or '_'
    int precision; // -1 = not given
    char type;     // 0 = default
} Spec;

static bool parseSpec(MVM *vm, const char *s, int len, Spec *spec)
{
    *spec = (Spec){.fill = ' ', .sign = '-', .precision = -1};
    int i = 0;
    if (len >= 2 && strchr("<>^=", s[1]))
    {
        spec->fill = s[0];
        spec->align = s[1];
        i = 2;
    }
    else if (len >= 1 && strchr("<>^=", s[0]))
    {
        spec->align = s[0];
        i = 1;
    }
    if (i < len && strchr("+- ", s[i]))
        spec->sign = s[i++];
    if (i < len && s[i] == '#')
    {
        spec->alternate = true;
        i++;
    }
    if (i < len && s[i] == '0')
    {
        if (!spec->align)
        {
            spec->fill = '0';
            spec->align = '=';
        }
        i++;
    }
    while (i < len && isdigit((unsigned char)s[i]))
        spec->width = spec->width * 10 + (s[i++] - '0');
    if (i < len && (s[i] == ',' || s[i] == '_'))
        spec->grouping = s[i++];
    if (i < len && s[i] == '.')
    {
        i++;
        if (i >= len || !isdigit((unsigned char)s[i]))
        {
            runtimeError(vm, "ValueError: format spec '%.*s' needs digits after '.'", len, s);
            return false;
        }
        spec->precision = 0;
        while (i < len && isdigit((unsigned char)s[i]))
            spec->precision = spec->precision * 10 + (s[i++] - '0');
    }
    if (i < len && strchr("sdxXobcfFeEgG%n", s[i]))
        spec->type = s[i++];
    if (i != len)
    {
        runtimeError(vm, "ValueError: invalid format spec '%.*s'", len, s);
        return false;
    }
    return true;
}

// Insert `sep` every three digits of the leading run of digits in
// digits[0..len) (the integer part), writing into `out`.
static void groupDigits(StrBuf *out, const char *digits, int len, char sep)
{
    int intLen = 0;
    while (intLen < len && isdigit((unsigned char)digits[intLen]))
        intLen++;
    for (int i = 0; i < intLen; i++)
    {
        if (i > 0 && (intLen - i) % 3 == 0)
            strbufAppend(out, &sep, 1);
        strbufAppend(out, digits + i, 1);
    }
    strbufAppend(out, digits + intLen, len - intLen);
}

// Pad `body` (with its sign/prefix `head` kept in front for '=' alignment).
static void pad(StrBuf *out, const Spec *spec, const char *head, int headLen, const char *body, int bodyLen,
                char defaultAlign)
{
    char align = spec->align ? spec->align : defaultAlign;
    int total = headLen + bodyLen;
    int padding = spec->width > total ? spec->width - total : 0;
    int left = align == '>' ? padding : align == '^' ? padding / 2 : 0;
    int right = align == '<' ? padding : align == '^' ? padding - padding / 2 : 0;
    for (int i = 0; i < left; i++)
        strbufAppend(out, &spec->fill, 1);
    strbufAppend(out, head, headLen);
    if (align == '=')
        for (int i = 0; i < padding; i++)
            strbufAppend(out, &spec->fill, 1);
    strbufAppend(out, body, bodyLen);
    for (int i = 0; i < right; i++)
        strbufAppend(out, &spec->fill, 1);
}

static void signHead(char *head, int *headLen, bool negative, const Spec *spec)
{
    if (negative)
        head[(*headLen)++] = '-';
    else if (spec->sign == '+')
        head[(*headLen)++] = '+';
    else if (spec->sign == ' ')
        head[(*headLen)++] = ' ';
}

static bool formatInteger(MVM *vm, StrBuf *out, long n, const Spec *spec)
{
    char type = spec->type ? spec->type : 'd';
    if (type == 'c')
    {
        if (n < 0 || n > 0x10FFFF)
        {
            runtimeError(vm, "OverflowError: %%c argument %ld is not a valid character", n);
            return false;
        }
        char c[4];
        int len = 0;
        if (n < 0x80)
            c[len++] = (char)n;
        else if (n < 0x800)
        {
            c[len++] = (char)(0xC0 | (n >> 6));
            c[len++] = (char)(0x80 | (n & 0x3F));
        }
        else if (n < 0x10000)
        {
            c[len++] = (char)(0xE0 | (n >> 12));
            c[len++] = (char)(0x80 | ((n >> 6) & 0x3F));
            c[len++] = (char)(0x80 | (n & 0x3F));
        }
        else
        {
            c[len++] = (char)(0xF0 | (n >> 18));
            c[len++] = (char)(0x80 | ((n >> 12) & 0x3F));
            c[len++] = (char)(0x80 | ((n >> 6) & 0x3F));
            c[len++] = (char)(0x80 | (n & 0x3F));
        }
        pad(out, spec, "", 0, c, len, '<');
        return true;
    }
    int base = type == 'x' || type == 'X' ? 16 : type == 'o' ? 8 : type == 'b' ? 2 : 10;
    unsigned long u = n < 0 ? 0UL - (unsigned long)n : (unsigned long)n;
    char digits[80];
    int len = 0;
    const char *alphabet = type == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
    do
    {
        digits[len++] = alphabet[u % (unsigned long)base];
        u /= (unsigned long)base;
    } while (u);
    for (int i = 0; i < len / 2; i++)
    {
        char t = digits[i];
        digits[i] = digits[len - 1 - i];
        digits[len - 1 - i] = t;
    }
    char head[8];
    int headLen = 0;
    signHead(head, &headLen, n < 0, spec);
    if (spec->alternate && base != 10)
    {
        head[headLen++] = '0';
        head[headLen++] = type == 'X' ? 'X' : type == 'x' ? 'x' : type;
    }
    StrBuf body;
    strbufInit(&body);
    if (spec->grouping)
        groupDigits(&body, digits, len, spec->grouping);
    else
        strbufAppend(&body, digits, len);
    pad(out, spec, head, headLen, body.data, body.length, '>');
    strbufFree(&body);
    return true;
}

static bool formatFloat(MVM *vm, StrBuf *out, double d, const Spec *spec)
{
    char type = spec->type;
    bool percent = type == '%';
    if (percent)
    {
        d *= 100;
        type = 'f';
    }
    char buf[512];
    bool negative = signbit(d) && !isnan(d);
    double magnitude = fabs(d);
    if (type == 0)
    {
        if (spec->precision >= 0)
            snprintf(buf, sizeof(buf), "%.*g", spec->precision == 0 ? 1 : spec->precision, magnitude);
        else
        {
            // Like repr: shortest round-trip, with ".0" on whole numbers.
            StrBuf r;
            strbufInit(&r);
            formatValue(vm, &r, V_DOUBLE_VAL(magnitude), true);
            snprintf(buf, sizeof(buf), "%s", r.data);
            strbufFree(&r);
        }
    }
    else
    {
        int precision = spec->precision >= 0 ? spec->precision : 6;
        if (precision > 300)
            precision = 300;
        char fmt[8] = {'%', '.', '*', type == 'F' ? 'f' : type, 0};
        snprintf(buf, sizeof(buf), fmt, precision, magnitude);
    }
    if (percent)
        strncat(buf, "%", sizeof(buf) - strlen(buf) - 1);
    char head[4];
    int headLen = 0;
    signHead(head, &headLen, negative, spec);
    StrBuf body;
    strbufInit(&body);
    if (spec->grouping && isdigit((unsigned char)buf[0]))
        groupDigits(&body, buf, (int)strlen(buf), spec->grouping);
    else
        strbufAppendC(&body, buf);
    pad(out, spec, head, headLen, body.data, body.length, '>');
    strbufFree(&body);
    return true;
}

bool formatWithSpec(MVM *vm, StrBuf *out, Value v, const char *specText, int specLen)
{
    Spec spec;
    if (!parseSpec(vm, specText, specLen, &spec))
        return false;
    bool isInt = valueLooksLikeInt(v) || valueIsBool(v);
    bool isNumber = isInt || valueLooksLikeDouble(v);
    char type = spec.type;
    if (type == 'n')
        type = spec.type = isInt ? 'd' : 'g';
    if (type && strchr("dxXobc", type))
    {
        if (!isInt)
        {
            runtimeError(vm, "ValueError: format code '%c' needs an integer, got %s", type, valueTypeName(v));
            return false;
        }
        return formatInteger(vm, out, valueIsBool(v) ? valueAsBool(v) : valueToLong(v), &spec);
    }
    if (type && strchr("fFeEgG%", type))
    {
        if (!isNumber)
        {
            runtimeError(vm, "ValueError: format code '%c' needs a number, got %s", type, valueTypeName(v));
            return false;
        }
        return formatFloat(vm, out, valueIsBool(v) ? valueAsBool(v) : valueAsNumber(v), &spec);
    }
    if (type == 0 && isInt && !valueIsBool(v))
        return formatInteger(vm, out, valueToLong(v), &spec);
    if (type == 0 && valueLooksLikeDouble(v))
        return formatFloat(vm, out, valueToDouble(v), &spec);
    // Strings and everything else: str(), truncated to the precision.
    StrBuf text;
    strbufInit(&text);
    if (!formatValue(vm, &text, v, false))
    {
        strbufFree(&text);
        return false;
    }
    int len = text.length;
    if (spec.precision >= 0 && spec.precision < len)
        len = spec.precision;
    if (spec.align == '=')
    {
        strbufFree(&text);
        runtimeError(vm, "ValueError: '=' alignment is only for numbers");
        return false;
    }
    pad(out, &spec, "", 0, text.data ? text.data : "", len, '<');
    strbufFree(&text);
    return true;
}

Value formatValueToString(MVM *vm, Value v, const char *spec, int specLen)
{
    StrBuf b;
    strbufInit(&b);
    if (!formatWithSpec(vm, &b, v, spec, specLen))
    {
        strbufFree(&b);
        return V_EMPTY_VAL;
    }
    Value out = V_OBJ_VAL(AS_OBJECT(newString(vm, b.data ? b.data : "", b.length)));
    strbufFree(&b);
    return out;
}

// "{} and {name:>5} {0!r}".format(...): positional ({} or {n}) and
// keyword ({name}) fields, optional !r/!s conversion and :spec;
// "{{" and "}}" are literal braces.
Value formatTemplate(MVM *vm, MyMoString *pattern, int argc, Value *args, MyMoDict *keywords)
{
    StrBuf out;
    strbufInit(&out);
    const char *s = pattern->value;
    int len = pattern->length;
    int autoIndex = 0;
    for (int i = 0; i < len; i++)
    {
        char c = s[i];
        if (c == '}' )
        {
            if (i + 1 < len && s[i + 1] == '}')
                i++;
            else
            {
                runtimeError(vm, "ValueError: single '}' in format string");
                goto fail;
            }
            strbufAppend(&out, "}", 1);
            continue;
        }
        if (c != '{')
        {
            strbufAppend(&out, &c, 1);
            continue;
        }
        if (i + 1 < len && s[i + 1] == '{')
        {
            strbufAppend(&out, "{", 1);
            i++;
            continue;
        }
        int close = i + 1;
        while (close < len && s[close] != '}')
            close++;
        if (close >= len)
        {
            runtimeError(vm, "ValueError: unclosed '{' in format string");
            goto fail;
        }
        // field = name [!conv] [:spec]
        int fieldStart = i + 1, nameEnd = fieldStart;
        while (nameEnd < close && s[nameEnd] != '!' && s[nameEnd] != ':')
            nameEnd++;
        char conversion = 0;
        int specStart = close;
        int p = nameEnd;
        if (p < close && s[p] == '!')
        {
            conversion = p + 1 < close ? s[p + 1] : 0;
            p += 2;
        }
        if (p < close && s[p] == ':')
            specStart = p + 1;
        Value value;
        int nameLen = nameEnd - fieldStart;
        if (nameLen == 0 || isdigit((unsigned char)s[fieldStart]))
        {
            int index = nameLen == 0 ? autoIndex++ : atoi(s + fieldStart);
            if (index >= argc)
            {
                runtimeError(vm, "IndexError: format() has no positional argument %d", index);
                goto fail;
            }
            value = args[index];
        }
        else
        {
            MyMoObject *key = AS_OBJECT(newString(vm, s + fieldStart, nameLen));
            if (!keywords || !getEntryV(keywords, key, &value))
            {
                runtimeError(vm, "KeyError: format() has no keyword argument '%.*s'", nameLen, s + fieldStart);
                goto fail;
            }
        }
        if (conversion == 'r' || conversion == 's')
        {
            value = conversion == 'r' ? valueToRepr(vm, value) : valueToStr(vm, value);
            if (V_IS_EMPTY(value))
                goto fail;
        }
        else if (conversion)
        {
            runtimeError(vm, "ValueError: unknown conversion '!%c' in format string", conversion);
            goto fail;
        }
        if (!formatWithSpec(vm, &out, value, s + specStart, close - specStart))
            goto fail;
        i = close;
    }
    {
        Value result = V_OBJ_VAL(AS_OBJECT(newString(vm, out.data ? out.data : "", out.length)));
        strbufFree(&out);
        return result;
    }
fail:
    strbufFree(&out);
    return V_EMPTY_VAL;
}

Value percentFormat(MVM *vm, MyMoString *fmt, Value args)
{
    ValueArray single = {.values = &args, .count = 1};
    ValueArray *values = V_IS_OBJ_TYPE(args, OBJ_TUPLE) ? &AS_TUPLE(V_AS_OBJ(args))->values : &single;
    MyMoDict *mapping = V_IS_OBJ_TYPE(args, OBJ_DICT) ? AS_DICT(V_AS_OBJ(args)) : NULL;
    int next = 0;
    StrBuf out;
    strbufInit(&out);
    const char *s = fmt->value;
    int len = fmt->length;
    for (int i = 0; i < len; i++)
    {
        if (s[i] != '%')
        {
            strbufAppend(&out, s + i, 1);
            continue;
        }
        if (++i >= len)
        {
            runtimeError(vm, "ValueError: incomplete format at the end of the string");
            goto fail;
        }
        if (s[i] == '%')
        {
            strbufAppend(&out, "%", 1);
            continue;
        }
        Value value;
        bool haveValue = false;
        if (s[i] == '(')
        {
            int close = i + 1;
            while (close < len && s[close] != ')')
                close++;
            if (!mapping || close >= len)
            {
                runtimeError(vm, mapping ? "ValueError: unclosed '(' in format" : "TypeError: format requires a dict for %%(name)");
                goto fail;
            }
            MyMoObject *key = AS_OBJECT(newString(vm, s + i + 1, close - i - 1));
            if (!getEntryV(mapping, key, &value))
            {
                runtimeError(vm, "KeyError: \"%.*s\"", close - i - 1, s + i + 1);
                goto fail;
            }
            haveValue = true;
            i = close + 1;
        }
        // flags, width, precision -> a format spec
        char spec[64];
        int k = 0;
        char align = 0, sign = 0;
        bool zero = false, alternate = false;
        for (; i < len && strchr("-+ 0#", s[i]); i++)
        {
            if (s[i] == '-') align = '<';
            else if (s[i] == '+') sign = '+';
            else if (s[i] == ' ' && sign != '+') sign = ' ';
            else if (s[i] == '0') zero = true;
            else alternate = true;
        }
        if (align)
            spec[k++] = align;
        if (sign)
            spec[k++] = sign;
        if (alternate)
            spec[k++] = '#';
        if (zero && !align)
            spec[k++] = '0';
        while (i < len && isdigit((unsigned char)s[i]) && k < 40)
            spec[k++] = s[i++];
        if (i < len && s[i] == '.')
        {
            spec[k++] = s[i++];
            while (i < len && isdigit((unsigned char)s[i]) && k < 60)
                spec[k++] = s[i++];
        }
        if (i >= len)
        {
            runtimeError(vm, "ValueError: incomplete format at the end of the string");
            goto fail;
        }
        char type = s[i];
        if (!haveValue)
        {
            if (mapping || next >= values->count)
            {
                runtimeError(vm, "TypeError: not enough arguments for format string");
                goto fail;
            }
            value = values->values[next++];
        }
        switch (type)
        {
        case 'r':
            value = valueToRepr(vm, value);
            if (V_IS_EMPTY(value))
                goto fail;
            type = 's';
            break;
        case 'i':
        case 'u':
            type = 'd';
            /* fall through */
        case 'd':
            if (valueLooksLikeDouble(value))
                value = valueFromLong(vm, (long)valueToDouble(value));
            break;
        case 's':
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G':
        case 'x': case 'X': case 'o': case 'c':
            break;
        default:
            runtimeError(vm, "ValueError: unsupported format character '%c'", type);
            goto fail;
        }
        spec[k++] = type;
        if (!formatWithSpec(vm, &out, value, spec, k))
            goto fail;
    }
    if (!mapping && next < values->count && values != &single)
    {
        runtimeError(vm, "TypeError: not all arguments converted during string formatting");
        goto fail;
    }
    {
        Value result = V_OBJ_VAL(AS_OBJECT(newString(vm, out.data ? out.data : "", out.length)));
        strbufFree(&out);
        return result;
    }
fail:
    strbufFree(&out);
    return V_EMPTY_VAL;
}
