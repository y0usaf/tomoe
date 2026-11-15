#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#include "lisp-internal.h"

struct fparam {
    bool present;
    bool is_char;
    char ch;
    int64_t value;
};

struct fstate {
    const char *s;
    size_t pos;
    size_t end;
    lisp_value *argv;
    int argc;
    int argi;
    struct lisp_buf *out;
    bool terminated;
};

static lisp_value next_arg(struct fstate *fs)
{
    if (fs->argi >= fs->argc)
        lisp_error("FORMAT: not enough arguments");
    return fs->argv[fs->argi++];
}

static struct fparam parse_param(struct fstate *fs)
{
    struct fparam p;
    p.present = false;
    p.is_char = false;
    p.ch = ' ';
    p.value = 0;
    if (fs->pos >= fs->end)
        return p;
    char c = fs->s[fs->pos];
    if (c == '\'') {
        fs->pos++;
        if (fs->pos >= fs->end)
            lisp_error("FORMAT: missing pad character");
        p.present = true;
        p.is_char = true;
        p.ch = fs->s[fs->pos++];
        return p;
    }
    if (c == 'v' || c == 'V') {
        fs->pos++;
        lisp_value v = next_arg(fs);
        p.present = true;
        p.value = require_int(v, "format parameter");
        return p;
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
        int64_t sign = 1;
        if (c == '-') {
            sign = -1;
            fs->pos++;
        }
        int64_t value = 0;
        bool any = false;
        while (fs->pos < fs->end && fs->s[fs->pos] >= '0' && fs->s[fs->pos] <= '9') {
            any = true;
            value = value * 10 + (fs->s[fs->pos] - '0');
            fs->pos++;
        }
        if (any) {
            p.present = true;
            p.value = sign * value;
        }
    }
    return p;
}

static size_t find_close_brace(const char *s, size_t pos, size_t end)
{
    int depth = 1;
    while (pos + 1 < end) {
        if (s[pos] == '~') {
            char d = s[pos + 1];
            if (d == '{')
                depth++;
            else if (d == '}') {
                depth--;
                if (depth == 0)
                    return pos;
            }
            pos += 2;
        } else {
            pos++;
        }
    }
    lisp_error("FORMAT: unterminated ~{");
}

static void format_range(struct fstate *fs);

static void pad_left(struct fstate *fs, const char *text, int64_t width)
{
    int64_t len = (int64_t)strlen(text);
    for (int64_t i = len; i < width; i++)
        buf_putc(fs->out, ' ');
    buf_puts(fs->out, text);
}

static void format_directive(struct fstate *fs, char dir, struct fparam *params)
{
    switch (dir) {
    case 'a':
    case 'A': {
        lisp_value v = next_arg(fs);
        print_value(v, false, fs->out);
        int64_t width = params[0].present ? params[0].value : 0;
        if (width > 0) {
            size_t len = fs->out->len;
            (void)len;
        }
        break;
    }
    case 's':
    case 'S': {
        lisp_value v = next_arg(fs);
        print_value(v, true, fs->out);
        break;
    }
    case 'd':
    case 'D': {
        lisp_value v = next_arg(fs);
        int64_t value = require_int(v, "~D argument");
        char tmp[64];
        snprintf(tmp, sizeof(tmp), "%lld", (long long)value);
        char pad = params[1].present ? (params[1].is_char ? params[1].ch : ' ') : ' ';
        int64_t width = params[0].present ? params[0].value : 0;
        int64_t len = (int64_t)strlen(tmp);
        for (int64_t i = len; i < width; i++)
            buf_putc(fs->out, pad);
        buf_puts(fs->out, tmp);
        break;
    }
    case 'x':
    case 'X': {
        lisp_value v = next_arg(fs);
        int64_t value = require_int(v, "~X argument");
        char tmp[64];
        snprintf(tmp, sizeof(tmp), "%llX", (unsigned long long)value);
        char pad = params[1].present ? (params[1].is_char ? params[1].ch : ' ') : ' ';
        int64_t width = params[0].present ? params[0].value : 0;
        int64_t len = (int64_t)strlen(tmp);
        for (int64_t i = len; i < width; i++)
            buf_putc(fs->out, pad);
        buf_puts(fs->out, tmp);
        break;
    }
    case 'f':
    case 'F': {
        lisp_value v = next_arg(fs);
        double d = number_to_double(v);
        int64_t width = params[0].present ? params[0].value : 0;
        int64_t digits = params[1].present ? params[1].value : 0;
        char tmp[512];
        snprintf(tmp, sizeof(tmp), "%.*f", (int)digits, d);
        pad_left(fs, tmp, width);
        break;
    }
    case 'e':
    case 'E':
        lisp_error("FORMAT: ~E is not supported");
    case 'r':
    case 'R':
        lisp_error("FORMAT: ~R is not supported");
    case '(':
    case ')':
        lisp_error("FORMAT: case conversion is not supported");
    case '[':
    case ']':
        lisp_error("FORMAT: conditional is not supported");
    case '%':
        buf_putc(fs->out, '\n');
        break;
    case '~':
        buf_putc(fs->out, '~');
        break;
    case '^':
        if (fs->argi >= fs->argc) {
            fs->terminated = true;
            fs->pos = fs->end;
        }
        break;
    case '{': {
        size_t close = find_close_brace(fs->s, fs->pos, fs->end);
        lisp_value list = next_arg(fs);
        int count = 0;
        lisp_value *items = heap_alloc(sizeof(lisp_value) * 256);
        lisp_value scan = list;
        while (consp(scan) && count < 256) {
            items[count++] = CAR(scan);
            scan = CDR(scan);
        }
        int index = 0;
        while (index < count) {
            struct fstate sub;
            sub.s = fs->s;
            sub.pos = fs->pos;
            sub.end = close;
            sub.argv = items;
            sub.argc = count;
            sub.argi = index;
            sub.out = fs->out;
            sub.terminated = false;
            format_range(&sub);
            if (sub.terminated || sub.argi == index)
                break;
            index = sub.argi;
        }
        fs->pos = close + 2;
        break;
    }
    case '}':
        fs->terminated = true;
        fs->pos = fs->end;
        break;
    default:
        lisp_error("FORMAT: unsupported directive ~%c", dir);
    }
}

static void format_range(struct fstate *fs)
{
    while (fs->pos < fs->end) {
        char c = fs->s[fs->pos];
        if (c != '~') {
            buf_putc(fs->out, c);
            fs->pos++;
            continue;
        }
        fs->pos++;
        struct fparam params[3];
        for (int i = 0; i < 3; i++) {
            params[i].present = false;
            params[i].is_char = false;
            params[i].ch = ' ';
            params[i].value = 0;
        }
        int pindex = 0;
        for (;;) {
            if (fs->pos >= fs->end)
                lisp_error("FORMAT: unterminated directive");
            char next = fs->s[fs->pos];
            if ((next >= '0' && next <= '9') || next == '\'' || next == 'v' || next == 'V' ||
                next == '-' || next == '+' || next == ',') {
                if (next == ',') {
                    fs->pos++;
                    if (pindex < 2)
                        pindex++;
                    continue;
                }
                params[pindex] = parse_param(fs);
                continue;
            }
            break;
        }
        if (fs->pos >= fs->end)
            lisp_error("FORMAT: unterminated directive");
        char dir = fs->s[fs->pos++];
        if (dir == ':' || dir == '@')
            lisp_error("FORMAT: modifiers are not supported");
        format_directive(fs, dir, params);
        if (fs->terminated)
            return;
    }
}

lisp_value lisp_format(const char *control, size_t len, int argc, lisp_value *argv)
{
    size_t cap = len * 2 + 64;
    char *buffer = heap_alloc(cap);
    struct lisp_buf out;
    buf_init(&out, buffer, cap);
    struct fstate fs;
    fs.s = control;
    fs.pos = 0;
    fs.end = len;
    fs.argv = argv;
    fs.argc = argc;
    fs.argi = 0;
    fs.out = &out;
    fs.terminated = false;
    format_range(&fs);
    return make_string_raw(out.data, out.len, utf8_strlen(out.data, out.len));
}

lisp_value builtin_format(int argc, lisp_value *argv)
{
    if (argc < 2)
        lisp_error("FORMAT needs a destination and a control string");
    if (!nilp(argv[0]))
        lisp_error("FORMAT: only NIL destinations are supported");
    size_t len = 0;
    const char *control = require_string(argv[1], &len, "format control");
    return lisp_format(control, len, argc - 2, argv + 2);
}
