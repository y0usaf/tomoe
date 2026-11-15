#include <string.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <ctype.h>

#include "lisp-internal.h"

void print_integer(struct lisp_buf *buf, int64_t value)
{
    char tmp[32];
    int n = snprintf(tmp, sizeof(tmp), "%lld", (long long)value);
    if (n > 0)
        buf_putn(buf, tmp, (size_t)n);
}

static bool double_roundtrips(const char *text, double value)
{
    char *end = NULL;
    double back = strtod(text, &end);
    if (!end || *end)
        return false;
    return back == value;
}

void print_double(struct lisp_buf *buf, double value)
{
    if (isnan(value)) {
        buf_puts(buf, "#<DOUBLE-FLOAT quiet NaN>");
        return;
    }
    if (isinf(value)) {
        buf_puts(buf, value > 0 ? "#<DOUBLE-FLOAT positive infinity>" :
                                  "#<DOUBLE-FLOAT negative infinity>");
        return;
    }
    char tmp[64];
    if (value == trunc(value) && fabs(value) < 9007199254740992.0 && !(value == 0 && signbit(value))) {
        print_integer(buf, (int64_t)value);
        buf_puts(buf, ".0d0");
        return;
    }
    if (value == trunc(value) && fabs(value) < 1e16) {
        snprintf(tmp, sizeof(tmp), "%.0f", value);
        if (double_roundtrips(tmp, value)) {
            buf_puts(buf, tmp);
            buf_puts(buf, ".0d0");
            return;
        }
    }
    const char *chosen = NULL;
    char best[64];
    for (int precision = 1; precision <= 17; precision++) {
        snprintf(best, sizeof(best), "%.*g", precision, value);
        if (double_roundtrips(best, value)) {
            chosen = best;
            break;
        }
    }
    if (!chosen)
        snprintf(best, sizeof(best), "%.17g", value);
    char mantissa[64];
    const char *exp = strchr(best, 'e');
    size_t mlen;
    if (exp) {
        mlen = (size_t)(exp - best);
        if (mlen >= sizeof(mantissa))
            mlen = sizeof(mantissa) - 1;
        memcpy(mantissa, best, mlen);
        mantissa[mlen] = '\0';
    } else {
        snprintf(mantissa, sizeof(mantissa), "%s", best);
    }
    if (!strchr(mantissa, '.')) {
        size_t l = strlen(mantissa);
        mantissa[l] = '.';
        mantissa[l + 1] = '0';
        mantissa[l + 2] = '\0';
    }
    buf_puts(buf, mantissa);
    if (exp) {
        const char *p = exp + 1;
        char sign = '+';
        if (*p == '+' || *p == '-')
            sign = *p++;
        while (*p == '0' && p[1])
            p++;
        buf_puts(buf, "d");
        buf_putc(buf, sign);
        buf_puts(buf, p);
    } else {
        buf_puts(buf, "d0");
    }
}

void number_print(struct lisp_buf *buf, lisp_value v, bool readably)
{
    (void)readably;
    switch (TYPE(v)) {
    case LT_INT:
        print_integer(buf, INTVAL(v));
        break;
    case LT_RATIO:
        print_integer(buf, PTR(v, lisp_ratio)->num);
        buf_putc(buf, '/');
        print_integer(buf, PTR(v, lisp_ratio)->den);
        break;
    case LT_DOUBLE:
        print_double(buf, DBLVAL(v));
        break;
    default:
        buf_puts(buf, "#<NOT-A-NUMBER>");
        break;
    }
}

static bool symbol_needs_escape(const char *name, size_t len)
{
    if (len == 0)
        return true;
    if (isdigit((unsigned char)name[0]))
        return true;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c >= 'A' && c <= 'Z')
            continue;
        if (isdigit(c))
            continue;
        if (strchr("+-*/<>=?!&_~^%$.:#/", c))
            continue;
        return true;
    }
    return false;
}

static void print_symbol_name(struct lisp_buf *buf, const char *name, size_t len)
{
    if (!symbol_needs_escape(name, len)) {
        buf_putn(buf, name, len);
        return;
    }
    buf_putc(buf, '|');
    for (size_t i = 0; i < len; i++) {
        if (name[i] == '|' || name[i] == '\\')
            buf_putc(buf, '\\');
        buf_putc(buf, name[i]);
    }
    buf_putc(buf, '|');
}

static void print_char(struct lisp_buf *buf, uint32_t cp, bool readably)
{
    if (!readably) {
        char tmp[4];
        size_t n = utf8_encode(cp, tmp);
        buf_putn(buf, tmp, n);
        return;
    }
    buf_puts(buf, "#\\");
    if (cp == ' ')
        buf_puts(buf, "Space");
    else if (cp == '\n')
        buf_puts(buf, "Newline");
    else if (cp == '\t')
        buf_puts(buf, "Tab");
    else if (cp == '\r')
        buf_puts(buf, "Return");
    else {
        char tmp[4];
        size_t n = utf8_encode(cp, tmp);
        buf_putn(buf, tmp, n);
    }
}

static void print_symbol(struct lisp_buf *buf, struct lisp_symbol *sym, bool readably)
{
    if (sym == lisp.sym_nil) {
        buf_puts(buf, "NIL");
        return;
    }
    if (sym == lisp.sym_t) {
        buf_puts(buf, "T");
        return;
    }
    if (readably && (sym->h.flags & SYM_KEYWORD))
        buf_putc(buf, ':');
    print_symbol_name(buf, sym->name, sym->name_len);
}

static void print_string(struct lisp_buf *buf, lisp_value v, bool readably)
{
    struct lisp_string *s = PTR(v, lisp_string);
    if (!readably) {
        buf_putn(buf, s->data, s->byte_len);
        return;
    }
    buf_putc(buf, '"');
    for (uint32_t i = 0; i < s->byte_len; i++) {
        char c = s->data[i];
        if (c == '"' || c == '\\')
            buf_putc(buf, '\\');
        buf_putc(buf, c);
    }
    buf_putc(buf, '"');
}

static void print_cons(struct lisp_buf *buf, lisp_value v, bool readably);

size_t print_value(lisp_value value, bool readably, struct lisp_buf *buf)
{
    switch (TYPE(value)) {
    case LT_SYMBOL:
        print_symbol(buf, SYM(value), readably);
        break;
    case LT_CONS:
        print_cons(buf, value, readably);
        break;
    case LT_INT:
    case LT_RATIO:
    case LT_DOUBLE:
        number_print(buf, value, readably);
        break;
    case LT_CHAR:
        print_char(buf, PTR(value, lisp_char)->code, readably);
        break;
    case LT_STRING:
        print_string(buf, value, readably);
        break;
    case LT_FUNCTION: {
        struct lisp_function *fn = PTR(value, lisp_function);
        if (fn->kind == FN_CLOSURE && fn->self_name && symbolp(fn->self_name)) {
            buf_puts(buf, "#<FUNCTION ");
            print_symbol(buf, SYM(fn->self_name), readably);
            buf_putc(buf, '>');
        } else if (fn->name) {
            buf_printf(buf, "#<FUNCTION %s>", fn->name);
        } else {
            buf_puts(buf, "#<FUNCTION>");
        }
        break;
    }
    case LT_CONDITION: {
        struct lisp_condition *c = PTR(value, lisp_condition);
        if (!readably) {
            print_value(c->message, false, buf);
        } else {
            buf_puts(buf, "#<CONDITION ");
            print_value(c->message, true, buf);
            buf_putc(buf, '>');
        }
        break;
    }
    case LT_HASHTABLE:
        buf_printf(buf, "#<HASH-TABLE :COUNT %u>", PTR(value, lisp_hashtable)->count);
        break;
    case LT_FRAME:
        buf_puts(buf, "#<ENVIRONMENT>");
        break;
    default:
        buf_puts(buf, "#<OBJECT>");
        break;
    }
    return buf->len;
}

static void print_cons(struct lisp_buf *buf, lisp_value v, bool readably)
{
    if (consp(v) && consp(CDR(v)) && nilp(CDR(CDR(v)))) {
        lisp_value head = CAR(v);
        if (head == VAL(lisp.sym_quote)) {
            buf_putc(buf, '\'');
            print_value(CAR(CDR(v)), readably, buf);
            return;
        }
        if (head == VAL(lisp.sym_function)) {
            buf_puts(buf, "#'");
            print_value(CAR(CDR(v)), readably, buf);
            return;
        }
        if (head == VAL(lisp.sym_quasiquote)) {
            buf_putc(buf, '`');
            print_value(CAR(CDR(v)), readably, buf);
            return;
        }
        if (head == VAL(lisp.sym_unquote)) {
            buf_putc(buf, ',');
            print_value(CAR(CDR(v)), readably, buf);
            return;
        }
        if (head == VAL(lisp.sym_unquote_splicing)) {
            buf_puts(buf, ",@");
            print_value(CAR(CDR(v)), readably, buf);
            return;
        }
    }
    buf_putc(buf, '(');
    print_value(CAR(v), readably, buf);
    lisp_value rest = CDR(v);
    while (consp(rest)) {
        buf_putc(buf, ' ');
        print_value(CAR(rest), readably, buf);
        rest = CDR(rest);
    }
    if (!nilp(rest)) {
        buf_puts(buf, " . ");
        print_value(rest, readably, buf);
    }
    buf_putc(buf, ')');
}

char *lisp_print_alloc(lisp_value value, bool readably)
{
    struct lisp_buf buf;
    if (!buf_init_growable(&buf, 256))
        return NULL;
    print_value(value, readably, &buf);
    if (!buf.owned) {
        free(buf.data);
        return NULL;
    }
    return buf.data;
}
