#include <string.h>
#include <stdarg.h>
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>

#include "lisp-internal.h"

static bool is_delimiter(int c)
{
    return c == 0 || c == '(' || c == ')' || c == '"' || c == '\'' || c == ';' ||
           c == '`' || c == ',' || isspace(c);
}

void reader_init(struct lisp_reader *rd, const char *file, const char *text, size_t len, uint32_t package, bool allow_backquote)
{
    rd->text = text;
    rd->len = len;
    rd->pos = 0;
    rd->line = 1;
    rd->file = file ? file : "<lisp>";
    rd->package = package;
    rd->allow_backquote = allow_backquote && package == PKG_TOMOE;
}

bool reader_done(struct lisp_reader *rd)
{
    size_t pos = rd->pos;
    while (pos < rd->len) {
        char c = rd->text[pos];
        if (c == ';') {
            while (pos < rd->len && rd->text[pos] != '\n')
                pos++;
        } else if (isspace((unsigned char)c)) {
            pos++;
        } else {
            return false;
        }
    }
    return true;
}

void reader_error(struct lisp_reader *rd, const char *fmt, ...)
{
    char message[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(message, sizeof(message), fmt, ap);
    va_end(ap);
    lisp_error("%s:%d@%zu: %s", rd->file, rd->line, rd->pos, message);
}

static int peek(struct lisp_reader *rd)
{
    if (rd->pos >= rd->len)
        return 0;
    return (unsigned char)rd->text[rd->pos];
}

static int peek2(struct lisp_reader *rd)
{
    if (rd->pos + 1 >= rd->len)
        return 0;
    return (unsigned char)rd->text[rd->pos + 1];
}

static int advance(struct lisp_reader *rd)
{
    int c = peek(rd);
    if (rd->pos < rd->len) {
        if (rd->text[rd->pos] == '\n')
            rd->line++;
        rd->pos++;
    }
    return c;
}

static void skip_space(struct lisp_reader *rd)
{
    for (;;) {
        int c = peek(rd);
        if (c == 0)
            return;
        if (isspace(c)) {
            advance(rd);
        } else if (c == ';') {
            while (peek(rd) && peek(rd) != '\n')
                advance(rd);
        } else {
            return;
        }
    }
}

static lisp_value read_form(struct lisp_reader *rd, bool data_mode);

static char *read_token_bytes(struct lisp_reader *rd, size_t *len_out)
{
    size_t start = rd->pos;
    while (!is_delimiter(peek(rd)))
        advance(rd);
    *len_out = rd->pos - start;
    return (char *)rd->text + start;
}

static lisp_value read_escaped_name(struct lisp_reader *rd, bool keyword, bool uninterned)
{
    advance(rd);
    size_t start = rd->pos;
    while (peek(rd) && peek(rd) != '|')
        advance(rd);
    if (peek(rd) != '|')
        reader_error(rd, "unterminated escaped symbol");
    size_t len = rd->pos - start;
    advance(rd);
    const char *name = rd->text + start;
    return lisp_intern_token(rd->package, name, len, keyword, uninterned);
}

static lisp_value read_atom(struct lisp_reader *rd)
{
    int c = peek(rd);
    if (c == '|') {
        size_t save = rd->pos;
        lisp_value sym = read_escaped_name(rd, false, false);
        (void)save;
        return sym;
    }
    if (c == ':') {
        advance(rd);
        if (peek(rd) == '|')
            return read_escaped_name(rd, true, false);
        size_t len = 0;
        char *token = read_token_bytes(rd, &len);
        if (len == 0)
            reader_error(rd, "empty keyword");
        char buffer[512];
        if (len >= sizeof(buffer))
            reader_error(rd, "symbol name too long");
        for (size_t i = 0; i < len; i++)
            buffer[i] = (char)toupper((unsigned char)token[i]);
        buffer[len] = '\0';
        return lisp_intern_token(rd->package, buffer, len, true, false);
    }
    size_t len = 0;
    char *token = read_token_bytes(rd, &len);
    if (len == 0)
        reader_error(rd, "unexpected character");
    bool numeric = false;
    size_t i = 0;
    if (token[0] == '+' || token[0] == '-')
        i = 1;
    if (i < len && isdigit((unsigned char)token[i]))
        numeric = true;
    if (i < len && token[i] == '.' && i + 1 < len && isdigit((unsigned char)token[i + 1]))
        numeric = true;
    if (numeric) {
        char buffer[512];
        if (len >= sizeof(buffer))
            reader_error(rd, "number too long");
        memcpy(buffer, token, len);
        buffer[len] = '\0';
        bool ratio = false, floating = false;
        for (size_t k = 0; k < len; k++) {
            char ch = buffer[k];
            if (ch == '/')
                ratio = true;
            else if (ch == '.')
                floating = true;
            else if (ch == 'e' || ch == 'E' || ch == 'd' || ch == 'D' || ch == 'f' ||
                     ch == 'F' || ch == 's' || ch == 'S' || ch == 'l' || ch == 'L')
                floating = true;
        }
        if (ratio) {
            char *slash = strchr(buffer, '/');
            *slash = '\0';
            errno = 0;
            char *end1 = NULL, *end2 = NULL;
            long long num = strtoll(buffer, &end1, 10);
            bool ok1 = !errno && end1 && !*end1;
            errno = 0;
            long long den = strtoll(slash + 1, &end2, 10);
            bool ok2 = !errno && end2 && !*end2;
            if (ok1 && ok2)
                return make_ratio(num, den);
        } else if (floating) {
            for (size_t k = 0; k < len; k++) {
                char ch = buffer[k];
                if (ch == 'd' || ch == 'D' || ch == 'f' || ch == 'F' || ch == 's' ||
                    ch == 'S' || ch == 'l' || ch == 'L')
                    buffer[k] = 'e';
            }
            errno = 0;
            char *end = NULL;
            double value = strtod(buffer, &end);
            if (!errno && end && !*end)
                return make_double(value);
        } else {
            errno = 0;
            char *end = NULL;
            long long value = strtoll(buffer, &end, 10);
            if (errno == ERANGE)
                reader_error(rd, "integer out of range");
            if (!errno && end && !*end)
                return make_int(value);
        }
    }
    char buffer[1024];
    if (len >= sizeof(buffer))
        reader_error(rd, "symbol name too long");
    for (size_t k = 0; k < len; k++)
        buffer[k] = (char)toupper((unsigned char)token[k]);
    buffer[len] = '\0';
    return lisp_intern_token(rd->package, buffer, len, false, false);
}

static lisp_value read_string(struct lisp_reader *rd)
{
    advance(rd);
    size_t cap = 64;
    char *buffer = heap_alloc(cap);
    size_t len = 0;
    for (;;) {
        int c = peek(rd);
        if (c == 0)
            reader_error(rd, "unterminated string");
        advance(rd);
        if (c == '"')
            break;
        if (c == '\\') {
            int next = peek(rd);
            if (next == 0)
                reader_error(rd, "unterminated string");
            advance(rd);
            c = next == 'n' ? '\n' : next == 't' ? '\t' : next;
        }
        if (len + 1 >= cap) {
            size_t ncap = cap * 2;
            char *nb = heap_alloc(ncap);
            memcpy(nb, buffer, len);
            buffer = nb;
            cap = ncap;
        }
        buffer[len++] = (char)c;
    }
    buffer[len] = '\0';
    return make_string_raw(buffer, len, utf8_strlen(buffer, len));
}

static lisp_value read_char_literal(struct lisp_reader *rd)
{
    advance(rd);
    if (peek(rd) == 0)
        reader_error(rd, "missing character name");
    if (is_delimiter(peek(rd)))
        return make_char((uint32_t)advance(rd));
    size_t len = 0;
    char *token = read_token_bytes(rd, &len);
    if (len == 0)
        reader_error(rd, "missing character name");
    if (len == 1)
        return make_char((unsigned char)token[0]);
    char name[64];
    if (len >= sizeof(name))
        reader_error(rd, "unknown character name");
    for (size_t i = 0; i < len; i++)
        name[i] = (char)toupper((unsigned char)token[i]);
    name[len] = '\0';
    if (strcmp(name, "SPACE") == 0)
        return make_char(' ');
    if (strcmp(name, "NEWLINE") == 0)
        return make_char('\n');
    if (strcmp(name, "TAB") == 0)
        return make_char('\t');
    if (strcmp(name, "RETURN") == 0)
        return make_char('\r');
    if (strcmp(name, "RUBOUT") == 0)
        return make_char(0x7f);
    if (strcmp(name, "LINEFEED") == 0)
        return make_char('\n');
    size_t adv = 0;
    uint32_t cp = utf8_decode(token, len, &adv);
    if (adv == len)
        return make_char(cp);
    reader_error(rd, "unknown character name %s", name);
    return NIL_VAL;
}

static lisp_value read_hex(struct lisp_reader *rd)
{
    advance(rd);
    size_t len = 0;
    char *token = read_token_bytes(rd, &len);
    if (len == 0)
        reader_error(rd, "missing hex digits");
    char buffer[64];
    if (len >= sizeof(buffer))
        reader_error(rd, "hex literal too long");
    memcpy(buffer, token, len);
    buffer[len] = '\0';
    errno = 0;
    char *end = NULL;
    long long value = strtoll(buffer, &end, 16);
    if (errno == ERANGE || !end || *end)
        reader_error(rd, "bad hex literal");
    return make_int(value);
}

static lisp_value read_hash(struct lisp_reader *rd, bool data_mode)
{
    advance(rd);
    int c = peek(rd);
    if (c == '\\')
        return read_char_literal(rd);
    if (c == 'x' || c == 'X')
        return read_hex(rd);
    if (c == '\'') {
        if (data_mode)
            reader_error(rd, "function syntax not allowed in data");
        advance(rd);
        lisp_value fn = read_form(rd, data_mode);
        return LIST2(VAL(lisp.sym_function), fn);
    }
    if (c == ':') {
        advance(rd);
        if (peek(rd) == '|')
            return read_escaped_name(rd, false, true);
        size_t len = 0;
        char *token = read_token_bytes(rd, &len);
        char buffer[512];
        if (len >= sizeof(buffer) || len == 0)
            reader_error(rd, "bad uninterned symbol");
        for (size_t i = 0; i < len; i++)
            buffer[i] = (char)toupper((unsigned char)token[i]);
        buffer[len] = '\0';
        return lisp_intern_token(rd->package, buffer, len, false, true);
    }
    reader_error(rd, "unsupported reader syntax #%c", c);
    return NIL_VAL;
}

static lisp_value read_list(struct lisp_reader *rd, bool data_mode)
{
    advance(rd);
    lisp_value head = NIL_VAL, tail = NIL_VAL;
    for (;;) {
        skip_space(rd);
        int c = peek(rd);
        if (c == 0)
            reader_error(rd, "unterminated list");
        if (c == ')') {
            advance(rd);
            return head;
        }
        if (c == '.' && is_delimiter(peek2(rd))) {
            advance(rd);
            if (nilp(head))
                reader_error(rd, "dot at start of list");
            lisp_value dotted = read_form(rd, data_mode);
            skip_space(rd);
            if (peek(rd) != ')')
                reader_error(rd, "more than one object after dot");
            advance(rd);
            CDR(tail) = dotted;
            return head;
        }
        lisp_value item = read_form(rd, data_mode);
        lisp_value cell = cons(item, NIL_VAL);
        if (nilp(head))
            head = cell;
        else
            CDR(tail) = cell;
        tail = cell;
    }
}

static lisp_value read_form(struct lisp_reader *rd, bool data_mode)
{
    skip_space(rd);
    int c = peek(rd);
    if (c == 0)
        reader_error(rd, "unexpected end of input");
    if (c == '(')
        return read_list(rd, data_mode);
    if (c == ')')
        reader_error(rd, "unexpected )");
    if (c == '\'') {
        if (data_mode)
            reader_error(rd, "quote syntax not allowed in data");
        advance(rd);
        return LIST2(VAL(lisp.sym_quote), read_form(rd, data_mode));
    }
    if (c == '`') {
        if (!rd->allow_backquote)
            reader_error(rd, "backquote is not supported");
        advance(rd);
        return LIST2(VAL(lisp.sym_quasiquote), read_form(rd, data_mode));
    }
    if (c == ',') {
        if (!rd->allow_backquote)
            reader_error(rd, "comma is not supported");
        advance(rd);
        if (peek(rd) == '@') {
            advance(rd);
            return LIST2(VAL(lisp.sym_unquote_splicing), read_form(rd, data_mode));
        }
        return LIST2(VAL(lisp.sym_unquote), read_form(rd, data_mode));
    }
    if (c == '"')
        return read_string(rd);
    if (c == '#')
        return read_hash(rd, data_mode);
    return read_atom(rd);
}

lisp_value reader_next(struct lisp_reader *rd, bool data_mode, int *line_out)
{
    skip_space(rd);
    if (rd->pos >= rd->len) {
        if (line_out)
            *line_out = rd->line;
        return NIL_VAL;
    }
    if (line_out)
        *line_out = rd->line;
    lisp_value form = read_form(rd, data_mode);
    return form;
}

int lisp_read_data(const char *text, size_t length, lisp_value *out, char *error, size_t error_size)
{
    if (!lisp.initialised) {
        if (error && error_size)
            snprintf(error, error_size, "lisp runtime not initialised");
        return -1;
    }
    if (!out) {
        if (error && error_size)
            snprintf(error, error_size, "no result buffer");
        return -1;
    }
    struct lisp_region *saved = lisp.cur_region;
    lisp.cur_region = &lisp.arena;
    int index = push_ctl(CTL_TOP);
    int result = 0;
    if (setjmp(lisp.ctl[index].jb) != 0) {
        lisp.ctl_top = index;
        if (error && error_size)
            snprintf(error, error_size, "%s", lisp_error_message());
        result = -1;
    } else {
        struct lisp_reader rd;
        reader_init(&rd, "<data>", text, length, PKG_TOMOE, false);
        if (reader_done(&rd)) {
            *out = NIL_VAL;
        } else {
            *out = reader_next(&rd, true, NULL);
            if (!reader_done(&rd)) {
                if (error && error_size)
                    snprintf(error, error_size, "trailing data");
                result = -1;
            }
        }
    }
    pop_ctl(index);
    lisp.cur_region = saved;
    lisp_clear_location();
    return result;
}
