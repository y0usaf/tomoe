#include <string.h>
#include <math.h>
#include <stdio.h>

#include "lisp-internal.h"

static lisp_value alloc_obj(uint8_t type, size_t size)
{
    struct lisp_obj *obj = heap_alloc(size);
    obj->type = type;
    obj->flags = 0;
    obj->aux = 0;
    obj->size = (uint32_t)((size + 7u) & ~(size_t)7u);
    return VAL(obj);
}

lisp_value cons(lisp_value car, lisp_value cdr)
{
    struct lisp_cons *out = PTR(alloc_obj(LT_CONS, sizeof(*out)), lisp_cons);
    out->car = car;
    out->cdr = cdr;
    return VAL(out);
}

#define SMALL_INT_MIN (-4096)
#define SMALL_INT_MAX 65536
#define SMALL_INT_COUNT (SMALL_INT_MAX - SMALL_INT_MIN + 1)

static struct lisp_int small_ints[SMALL_INT_COUNT];
static bool small_ints_ready;

static struct lisp_char ascii_chars[128];

void lisp_init_small_ints(void)
{
    for (int64_t value = SMALL_INT_MIN; value <= SMALL_INT_MAX; value++) {
        struct lisp_int *slot = &small_ints[value - SMALL_INT_MIN];
        slot->h.type = LT_INT;
        slot->h.flags = 0;
        slot->h.aux = 0;
        slot->h.size = sizeof(*slot);
        slot->value = value;
    }
    for (uint32_t code = 0; code < 128; code++) {
        struct lisp_char *slot = &ascii_chars[code];
        slot->h.type = LT_CHAR;
        slot->h.flags = 0;
        slot->h.aux = 0;
        slot->h.size = sizeof(*slot);
        slot->code = code;
    }
    small_ints_ready = true;
}

lisp_value make_int(int64_t v)
{
    if (small_ints_ready && v >= SMALL_INT_MIN && v <= SMALL_INT_MAX)
        return VAL(&small_ints[v - SMALL_INT_MIN]);
    struct lisp_int *out = PTR(alloc_obj(LT_INT, sizeof(*out)), lisp_int);
    out->value = v;
    return VAL(out);
}

static int64_t gcd64(int64_t a, int64_t b)
{
    if (a < 0)
        a = -a;
    if (b < 0)
        b = -b;
    while (b) {
        int64_t t = a % b;
        a = b;
        b = t;
    }
    return a;
}

lisp_value make_ratio(int64_t num, int64_t den)
{
    if (den == 0)
        lisp_error("division by zero");
    if (den < 0) {
        if (num == INT64_MIN || den == INT64_MIN)
            lisp_error("arithmetic overflow");
        num = -num;
        den = -den;
    }
    int64_t g = gcd64(num, den);
    if (g > 1) {
        num /= g;
        den /= g;
    }
    if (den == 1)
        return make_int(num);
    struct lisp_ratio *out = PTR(alloc_obj(LT_RATIO, sizeof(*out)), lisp_ratio);
    out->num = num;
    out->den = den;
    return VAL(out);
}

lisp_value make_double(double v)
{
    struct lisp_double *out = PTR(alloc_obj(LT_DOUBLE, sizeof(*out)), lisp_double);
    out->value = v;
    return VAL(out);
}

lisp_value make_char(uint32_t cp)
{
    if (small_ints_ready && cp < 128)
        return VAL(&ascii_chars[cp]);
    struct lisp_char *out = PTR(alloc_obj(LT_CHAR, sizeof(*out)), lisp_char);
    out->code = cp;
    return VAL(out);
}

lisp_value make_string_raw(const char *data, size_t bytes, uint32_t cp_len)
{
    size_t size = offsetof(struct lisp_string, data) + bytes + 1;
    struct lisp_string *out = PTR(alloc_obj(LT_STRING, size), lisp_string);
    out->len = cp_len;
    out->byte_len = (uint32_t)bytes;
    memcpy(out->data, data, bytes);
    out->data[bytes] = '\0';
    return VAL(out);
}

lisp_value make_string_len(const char *data, size_t bytes)
{
    return make_string_raw(data, bytes, utf8_strlen(data, bytes));
}

lisp_value make_hashtable(lisp_value test)
{
    struct lisp_hashtable *out = PTR(alloc_obj(LT_HASHTABLE, sizeof(*out)), lisp_hashtable);
    out->count = 0;
    out->test = test;
    out->entries = NIL_VAL;
    return VAL(out);
}

lisp_value make_condition_obj(const char *message)
{
    struct lisp_condition *out = PTR(alloc_obj(LT_CONDITION, sizeof(*out)), lisp_condition);
    out->class = VAL(lisp.sym_error);
    out->message = make_string_len(message, strlen(message));
    return VAL(out);
}

static bool eql_double(double a, double b)
{
    return a == b || (isnan(a) && isnan(b));
}

bool eql_value(lisp_value a, lisp_value b)
{
    if (a == b)
        return true;
    uint8_t ta = TYPE(a), tb = TYPE(b);
    if (ta != tb)
        return false;
    switch (ta) {
    case LT_INT:
        return INTVAL(a) == INTVAL(b);
    case LT_RATIO:
        return PTR(a, lisp_ratio)->num == PTR(b, lisp_ratio)->num &&
               PTR(a, lisp_ratio)->den == PTR(b, lisp_ratio)->den;
    case LT_DOUBLE:
        return eql_double(DBLVAL(a), DBLVAL(b));
    case LT_CHAR:
        return PTR(a, lisp_char)->code == PTR(b, lisp_char)->code;
    default:
        return false;
    }
}

static bool equal_inner(lisp_value a, lisp_value b, int depth)
{
    if (a == b)
        return true;
    if (depth > 1000)
        return false;
    check_steps();
    uint8_t ta = TYPE(a), tb = TYPE(b);
    if (ta != tb) {
        if (numberp(a) && numberp(b))
            return false;
        return false;
    }
    switch (ta) {
    case LT_CONS:
        return equal_inner(CAR(a), CAR(b), depth + 1) && equal_inner(CDR(a), CDR(b), depth + 1);
    case LT_STRING: {
        struct lisp_string *x = PTR(a, lisp_string), *y = PTR(b, lisp_string);
        if (x->byte_len != y->byte_len)
            return false;
        return memcmp(x->data, y->data, x->byte_len) == 0;
    }
    case LT_SYMBOL:
        return false;
    case LT_HASHTABLE:
        return equal_inner(PTR(a, lisp_hashtable)->entries, PTR(b, lisp_hashtable)->entries, depth + 1);
    case LT_CONDITION:
        return equal_inner(PTR(a, lisp_condition)->message, PTR(b, lisp_condition)->message, depth + 1);
    default:
        return eql_value(a, b);
    }
}

bool equal_value(lisp_value a, lisp_value b)
{
    return equal_inner(a, b, 0);
}

bool hash_table_match(lisp_value table, lisp_value a, lisp_value b)
{
    lisp_value test = PTR(table, lisp_hashtable)->test;
    if (nilp(test))
        return eql_value(a, b);
    lisp_value args[2] = { a, b };
    return truthy(apply_function(test, 2, args));
}

bool lisp_is_true(lisp_value v)
{
    return v != NIL_VAL;
}

int64_t require_int(lisp_value v, const char *what)
{
    if (TYPE(v) != LT_INT)
        lisp_error("%s must be an integer", what);
    return INTVAL(v);
}

lisp_value require_number(lisp_value v, const char *what)
{
    if (!numberp(v))
    {
        char shown[120];
        lisp_print(v, true, shown, sizeof(shown));
        lisp_error("%s must be a number, got %s", what, shown);
    }
    return v;
}

const char *require_string(lisp_value v, size_t *len, const char *what)
{
    if (TYPE(v) != LT_STRING)
        lisp_error("%s must be a string", what);
    if (len)
        *len = PTR(v, lisp_string)->byte_len;
    return PTR(v, lisp_string)->data;
}

uint32_t require_char(lisp_value v, const char *what)
{
    if (TYPE(v) != LT_CHAR)
        lisp_error("%s must be a character", what);
    return PTR(v, lisp_char)->code;
}

lisp_value bool_value(bool b)
{
    return b ? T_VAL : NIL_VAL;
}

lisp_value list_from_array(int n, lisp_value *items)
{
    lisp_value out = NIL_VAL;
    for (int i = n - 1; i >= 0; i--)
        out = cons(items[i], out);
    return out;
}

int list_length(lisp_value list)
{
    int n = 0;
    while (consp(list)) {
        check_steps();
        n++;
        list = CDR(list);
        if (n > 1000000)
            lisp_error("list too long");
    }
    return n;
}

lisp_value list_reverse_copy(lisp_value list)
{
    lisp_value out = NIL_VAL;
    while (consp(list)) {
        check_steps();
        out = cons(CAR(list), out);
        list = CDR(list);
    }
    return out;
}

lisp_value list_append2(lisp_value a, lisp_value b)
{
    if (nilp(a))
        return b;
    lisp_value head = NIL_VAL, tail = NIL_VAL;
    while (consp(a)) {
        check_steps();
        lisp_value cell = cons(CAR(a), NIL_VAL);
        if (nilp(head))
            head = cell;
        else
            CDR(tail) = cell;
        tail = cell;
        a = CDR(a);
    }
    CDR(tail) = b;
    return head;
}

lisp_value list_nth(lisp_value list, int64_t n)
{
    while (n > 0) {
        if (!consp(list))
            return NIL_VAL;
        check_steps();
        list = CDR(list);
        n--;
    }
    if (!consp(list))
        return NIL_VAL;
    return CAR(list);
}

lisp_value list_nthcdr(lisp_value list, int64_t n)
{
    while (n > 0) {
        if (!consp(list))
            return NIL_VAL;
        check_steps();
        list = CDR(list);
        n--;
    }
    return list;
}

lisp_value list_last(lisp_value list)
{
    if (!consp(list))
        return NIL_VAL;
    while (consp(CDR(list))) {
        check_steps();
        list = CDR(list);
    }
    return list;
}

int64_t list_count(lisp_value list)
{
    int64_t n = 0;
    while (consp(list)) {
        check_steps();
        n++;
        list = CDR(list);
    }
    return n;
}

void mv_reset(lisp_value primary)
{
    lisp.mv[0] = primary;
    lisp.mv_count = 1;
}

lisp_value mv_first(void)
{
    return lisp.mv[0];
}

void mv_return2(lisp_value a, lisp_value b)
{
    lisp.mv[0] = a;
    lisp.mv[1] = b;
    lisp.mv_count = 2;
}

static void check_overflow(__int128 v)
{
    if (v > INT64_MAX || v < INT64_MIN)
        lisp_error("arithmetic overflow");
}

static void ratio_parts(lisp_value v, int64_t *num, int64_t *den)
{
    if (TYPE(v) == LT_INT) {
        *num = INTVAL(v);
        *den = 1;
    } else if (TYPE(v) == LT_RATIO) {
        *num = PTR(v, lisp_ratio)->num;
        *den = PTR(v, lisp_ratio)->den;
    } else {
        char shown[120];
        lisp_print(v, true, shown, sizeof(shown));
        lisp_error("argument must be a number, got %s", shown);
    }
}

lisp_value number_add(lisp_value a, lisp_value b)
{
    if (doublep(a) || doublep(b))
        return make_double(number_to_double(a) + number_to_double(b));
    int64_t an, ad, bn, bd;
    ratio_parts(a, &an, &ad);
    ratio_parts(b, &bn, &bd);
    __int128 num = (__int128)an * bd + (__int128)bn * ad;
    __int128 den = (__int128)ad * bd;
    if (den == 1) {
        check_overflow(num);
        return make_int((int64_t)num);
    }
    check_overflow(num);
    check_overflow(den);
    return make_ratio((int64_t)num, (int64_t)den);
}

lisp_value number_sub(lisp_value a, lisp_value b)
{
    if (doublep(a) || doublep(b))
        return make_double(number_to_double(a) - number_to_double(b));
    int64_t an, ad, bn, bd;
    ratio_parts(a, &an, &ad);
    ratio_parts(b, &bn, &bd);
    __int128 num = (__int128)an * bd - (__int128)bn * ad;
    __int128 den = (__int128)ad * bd;
    if (den == 1) {
        check_overflow(num);
        return make_int((int64_t)num);
    }
    check_overflow(num);
    check_overflow(den);
    return make_ratio((int64_t)num, (int64_t)den);
}

lisp_value number_mul(lisp_value a, lisp_value b)
{
    if (doublep(a) || doublep(b))
        return make_double(number_to_double(a) * number_to_double(b));
    int64_t an, ad, bn, bd;
    ratio_parts(a, &an, &ad);
    ratio_parts(b, &bn, &bd);
    __int128 num = (__int128)an * bn;
    __int128 den = (__int128)ad * bd;
    if (den == 1) {
        check_overflow(num);
        return make_int((int64_t)num);
    }
    check_overflow(num);
    check_overflow(den);
    return make_ratio((int64_t)num, (int64_t)den);
}

lisp_value number_div(lisp_value a, lisp_value b)
{
    if (doublep(a) || doublep(b))
        return make_double(number_to_double(a) / number_to_double(b));
    int64_t an, ad, bn, bd;
    ratio_parts(a, &an, &ad);
    ratio_parts(b, &bn, &bd);
    if (bn == 0)
        lisp_error("division by zero");
    __int128 num = (__int128)an * bd;
    __int128 den = (__int128)ad * bn;
    if (den == 1) {
        check_overflow(num);
        return make_int((int64_t)num);
    }
    check_overflow(num);
    check_overflow(den);
    return make_ratio((int64_t)num, (int64_t)den);
}

int number_compare(lisp_value a, lisp_value b, bool *ok)
{
    *ok = true;
    if (doublep(a) || doublep(b)) {
        double x = number_to_double(a), y = number_to_double(b);
        if (isnan(x) || isnan(y)) {
            *ok = false;
            return 0;
        }
        return x < y ? -1 : x > y ? 1 : 0;
    }
    int64_t an, ad, bn, bd;
    ratio_parts(a, &an, &ad);
    ratio_parts(b, &bn, &bd);
    __int128 left = (__int128)an * bd;
    __int128 right = (__int128)bn * ad;
    return left < right ? -1 : left > right ? 1 : 0;
}

double number_to_double(lisp_value v)
{
    switch (TYPE(v)) {
    case LT_INT:
        return (double)INTVAL(v);
    case LT_RATIO:
        return (double)PTR(v, lisp_ratio)->num / (double)PTR(v, lisp_ratio)->den;
    case LT_DOUBLE:
        return DBLVAL(v);
    default:
        lisp_error("not a number");
    }
}

bool number_finite(lisp_value v)
{
    return isfinite(number_to_double(v));
}

uint32_t utf8_decode(const char *s, size_t len, size_t *advance)
{
    if (len == 0) {
        if (advance)
            *advance = 0;
        return 0;
    }
    unsigned char c = (unsigned char)s[0];
    if (c < 0x80) {
        if (advance)
            *advance = 1;
        return c;
    }
    uint32_t cp;
    size_t n;
    if ((c & 0xe0) == 0xc0) {
        cp = c & 0x1f;
        n = 2;
    } else if ((c & 0xf0) == 0xe0) {
        cp = c & 0x0f;
        n = 3;
    } else if ((c & 0xf8) == 0xf0) {
        cp = c & 0x07;
        n = 4;
    } else {
        if (advance)
            *advance = 1;
        return c;
    }
    if (len < n) {
        if (advance)
            *advance = 1;
        return c;
    }
    for (size_t i = 1; i < n; i++) {
        unsigned char cc = (unsigned char)s[i];
        if ((cc & 0xc0) != 0x80) {
            if (advance)
                *advance = 1;
            return c;
        }
        cp = (cp << 6) | (cc & 0x3f);
    }
    if (advance)
        *advance = n;
    return cp;
}

size_t utf8_encode(uint32_t cp, char *out)
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xc0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3f));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xe0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
        out[2] = (char)(0x80 | (cp & 0x3f));
        return 3;
    }
    out[0] = (char)(0xf0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
    out[3] = (char)(0x80 | (cp & 0x3f));
    return 4;
}

uint32_t utf8_strlen(const char *s, size_t len)
{
    uint32_t n = 0;
    size_t pos = 0;
    while (pos < len) {
        size_t adv = 0;
        utf8_decode(s + pos, len - pos, &adv);
        if (adv == 0)
            break;
        pos += adv;
        n++;
    }
    return n;
}

uint32_t string_char_at(lisp_value str, uint32_t index)
{
    struct lisp_string *s = PTR(str, lisp_string);
    if (s->len == s->byte_len && index < s->byte_len && (unsigned char)s->data[index] < 0x80)
        return (unsigned char)s->data[index];
    size_t pos = 0;
    uint32_t i = 0;
    while (pos < s->byte_len) {
        size_t adv = 0;
        uint32_t cp = utf8_decode(s->data + pos, s->byte_len - pos, &adv);
        if (i == index)
            return cp;
        pos += adv;
        i++;
    }
    lisp_error("string index out of range");
}

static size_t string_byte_offset(lisp_value str, uint32_t index)
{
    struct lisp_string *s = PTR(str, lisp_string);
    if (s->len == s->byte_len)
        return index < s->byte_len ? index : s->byte_len;
    size_t pos = 0;
    uint32_t i = 0;
    while (pos < s->byte_len && i < index) {
        size_t adv = 0;
        utf8_decode(s->data + pos, s->byte_len - pos, &adv);
        pos += adv;
        i++;
    }
    return pos;
}

lisp_value string_from_range(lisp_value str, uint32_t start, uint32_t end)
{
    struct lisp_string *s = PTR(str, lisp_string);
    if (end < start)
        end = start;
    size_t a = string_byte_offset(str, start);
    size_t b = string_byte_offset(str, end);
    return make_string_raw(s->data + a, b - a, end - start);
}

lisp_value string_subseq(lisp_value str, uint32_t start, uint32_t end)
{
    return string_from_range(str, start, end);
}

bool string_equal_to(lisp_value a, lisp_value b)
{
    struct lisp_string *x = PTR(a, lisp_string), *y = PTR(b, lisp_string);
    return x->byte_len == y->byte_len && memcmp(x->data, y->data, x->byte_len) == 0;
}

lisp_value string_concat(int count, lisp_value *items)
{
    size_t total = 0;
    for (int i = 0; i < count; i++) {
        if (TYPE(items[i]) != LT_STRING)
            lisp_error("concatenate of a non-string");
        total += PTR(items[i], lisp_string)->byte_len;
    }
    char *buffer = heap_alloc(total + 1);
    size_t at = 0;
    for (int i = 0; i < count; i++) {
        struct lisp_string *s = PTR(items[i], lisp_string);
        memcpy(buffer + at, s->data, s->byte_len);
        at += s->byte_len;
    }
    buffer[at] = '\0';
    return make_string_raw(buffer, at, utf8_strlen(buffer, at));
}

lisp_value keyword_get(lisp_value plist, lisp_value key, lisp_value deflt)
{
    if (symbolp(key)) {
        while (consp(plist) && consp(CDR(plist))) {
            check_steps();
            if (CAR(plist) == key)
                return CAR(CDR(plist));
            plist = CDR(CDR(plist));
        }
        return deflt;
    }
    while (consp(plist) && consp(CDR(plist))) {
        check_steps();
        if (eql_value(CAR(plist), key))
            return CAR(CDR(plist));
        plist = CDR(CDR(plist));
    }
    return deflt;
}

bool plist_lookup(lisp_value plist, const char *name, lisp_value *out)
{
    struct lisp_symbol *key = symbol_find(PKG_KEYWORD, name, strlen(name));
    if (!key)
        return false;
    lisp_value found = keyword_get(plist, VAL(key), VAL(lisp.sym_unbound));
    if (found == VAL(lisp.sym_unbound))
        return false;
    if (out)
        *out = found;
    return true;
}
