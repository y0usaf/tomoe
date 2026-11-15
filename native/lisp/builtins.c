#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include <ctype.h>

#include "lisp-internal.h"

static void kw_init(struct kw *kw)
{
    memset(kw, 0, sizeof(*kw));
}

enum kw_id {
    KW_TEST, KW_TEST_NOT, KW_KEY, KW_START, KW_END, KW_FROM_END, KW_RADIX, KW_JUNK_ALLOWED,
    KW_START2, KW_END2, KW_INITIAL_VALUE, KW_INITIAL_ELEMENT, KW_COUNT, KW_IDS
};

static const char *const kw_names[KW_IDS] = {
    "TEST", "TEST-NOT", "KEY", "START", "END", "FROM-END", "RADIX", "JUNK-ALLOWED",
    "START2", "END2", "INITIAL-VALUE", "INITIAL-ELEMENT", "COUNT"
};

static lisp_value kw_symbols[KW_IDS];

void kw_parse(int argc, lisp_value *argv, int base, struct kw *kw)
{
    kw_init(kw);
    int i = base;
    while (i < argc) {
        if (i + 1 >= argc)
            lisp_error("odd keyword arguments");
        lisp_value key = argv[i];
        lisp_value value = argv[i + 1];
        int id = 0;
        while (id < KW_IDS && kw_symbols[id] != key)
            id++;
        switch (id) {
        case KW_TEST: kw->has_test = true; kw->test = value; break;
        case KW_TEST_NOT: kw->has_test_not = true; kw->test_not = value; break;
        case KW_KEY: kw->has_key = true; kw->key = value; break;
        case KW_START: kw->has_start = true; kw->start = value; break;
        case KW_END: kw->has_end = true; kw->end = value; break;
        case KW_FROM_END: kw->has_from_end = true; kw->from_end = value; break;
        case KW_RADIX: kw->has_radix = true; kw->radix = value; break;
        case KW_JUNK_ALLOWED: kw->has_junk = true; kw->junk = truthy(value); break;
        case KW_START2: kw->has_start2 = true; kw->start2 = value; break;
        case KW_END2: kw->has_end2 = true; kw->end2 = value; break;
        case KW_INITIAL_VALUE: kw->has_initial_value = true; kw->initial_value = value; break;
        case KW_INITIAL_ELEMENT: kw->has_initial_element = true; kw->initial_element = value; break;
        case KW_COUNT: kw->has_count = true; kw->count = value; break;
        default: lisp_error("unsupported keyword argument %s", SYM(key)->name);
        }
        i += 2;
    }
}

static bool kw_direct_test(lisp_value fn, lisp_value a, lisp_value b, bool *result)
{
    if (TYPE(fn) != LT_FUNCTION || PTR(fn, lisp_function)->kind != FN_BUILTIN)
        return false;
    lisp_primitive prim = PTR(fn, lisp_function)->fn;
    if (prim == builtin_eq)
        *result = a == b;
    else if (prim == builtin_eql)
        *result = eql_value(a, b);
    else if (prim == builtin_equal)
        *result = equal_value(a, b);
    else
        return false;
    return true;
}

bool lisp_kw_test(struct kw *kw, lisp_value a, lisp_value b)
{
    bool direct;
    if (kw->has_test_not && !nilp(kw->test_not)) {
        if (kw_direct_test(kw->test_not, a, b, &direct))
            return !direct;
        lisp_value args[2] = { a, b };
        return !truthy(apply_function(kw->test_not, 2, args));
    }
    if (!kw->has_test || nilp(kw->test))
        return eql_value(a, b);
    if (kw_direct_test(kw->test, a, b, &direct))
        return direct;
    lisp_value args[2] = { a, b };
    return truthy(apply_function(kw->test, 2, args));
}

lisp_value lisp_kw_key(struct kw *kw, lisp_value v)
{
    if (!kw->has_key || nilp(kw->key))
        return v;
    lisp_value args[1] = { v };
    return apply_function(kw->key, 1, args);
}

static int64_t opt_int(struct kw *kw, bool present, lisp_value value, int64_t deflt)
{
    if (!present)
        return deflt;
    return require_int(value, "keyword value");
}

lisp_value builtin_values(int argc, lisp_value *argv)
{
    int count = argc > LISP_MAX_MV ? LISP_MAX_MV : argc;
    for (int i = 0; i < count; i++)
        lisp.mv[i] = argv[i];
    lisp.mv_count = count;
    return count > 0 ? argv[0] : NIL_VAL;
}

lisp_value builtin_error(int argc, lisp_value *argv)
{
    if (argc < 1)
        lisp_error("ERROR needs a control string");
    if (TYPE(argv[0]) == LT_CONDITION)
        throw_error_value(argv[0]);
    size_t len = 0;
    const char *control = require_string(argv[0], &len, "error control");
    lisp_value message = lisp_format(control, len, argc - 1, argv + 1);
    throw_error_value(make_condition_obj(PTR(message, lisp_string)->data));
}

lisp_value builtin_funcall(int argc, lisp_value *argv)
{
    if (argc < 1)
        lisp_error("FUNCALL needs a function");
    return apply_function(argv[0], argc - 1, argv + 1);
}

lisp_value builtin_apply(int argc, lisp_value *argv)
{
    if (argc < 2)
        lisp_error("APPLY needs a function and a list");
    lisp_value fn = argv[0];
    int count = argc - 2;
    int extra = list_length(argv[argc - 1]);
    int total = count + extra;
    lisp_value *args = heap_alloc(sizeof(lisp_value) * (size_t)(total + 1));
    for (int i = 0; i < count; i++)
        args[i] = argv[i + 1];
    lisp_value tail = argv[argc - 1];
    int at = count;
    while (consp(tail)) {
        args[at++] = CAR(tail);
        tail = CDR(tail);
    }
    return apply_function(fn, total, args);
}

lisp_value builtin_identity(int argc, lisp_value *argv)
{
    return argc > 0 ? argv[0] : NIL_VAL;
}

lisp_value list_to_array(lisp_value list, int *count_out)
{
    int n = list_length(list);
    lisp_value *items = heap_alloc(sizeof(lisp_value) * (size_t)(n + 1));
    int i = 0;
    while (consp(list)) {
        items[i++] = CAR(list);
        list = CDR(list);
    }
    *count_out = n;
    return VAL(items);
}

lisp_value builtin_mapcar(int argc, lisp_value *argv)
{
    if (argc < 2)
        lisp_error("MAPCAR needs a function and a list");
    lisp_value fn = argv[0];
    int lists_count = argc - 1;
    lisp_value lists[16];
    if (lists_count > 16)
        lisp_error("MAPCAR: too many lists");
    for (int i = 0; i < lists_count; i++)
        lists[i] = argv[i + 1];
    lisp_value out = NIL_VAL, tail = NIL_VAL;
    for (;;) {
        lisp_value args[16];
        bool any_empty = false;
        for (int i = 0; i < lists_count; i++) {
            if (!consp(lists[i])) {
                any_empty = true;
                break;
            }
            args[i] = CAR(lists[i]);
        }
        if (any_empty)
            break;
        lisp_value value = apply_function(fn, lists_count, args);
        lisp_value cell = cons(value, NIL_VAL);
        if (nilp(out))
            out = cell;
        else
            CDR(tail) = cell;
        tail = cell;
        for (int i = 0; i < lists_count; i++)
            lists[i] = CDR(lists[i]);
    }
    return out;
}

static lisp_value builtin_map_like(int argc, lisp_value *argv, int mode)
{
    if (argc < 2)
        lisp_error("MAP needs a function and a list");
    lisp_value fn = argv[0];
    int lists_count = argc - 1;
    lisp_value lists[16];
    if (lists_count > 16)
        lisp_error("MAP: too many lists");
    for (int i = 0; i < lists_count; i++)
        lists[i] = argv[i + 1];
    lisp_value out = NIL_VAL, tail = NIL_VAL;
    for (;;) {
        lisp_value args[16];
        bool any_empty = false;
        for (int i = 0; i < lists_count; i++) {
            if (!consp(lists[i])) {
                any_empty = true;
                break;
            }
            args[i] = CAR(lists[i]);
        }
        if (any_empty)
            break;
        lisp_value value = apply_function(fn, lists_count, args);
        if (mode == 0) {
            lisp_value cell = cons(value, NIL_VAL);
            if (nilp(out))
                out = cell;
            else
                CDR(tail) = cell;
            tail = cell;
        } else if (mode == 1) {
            if (!nilp(value)) {
                if (nilp(out)) {
                    out = value;
                } else {
                    CDR(tail) = value;
                }
                tail = list_last(value);
            }
        }
        for (int i = 0; i < lists_count; i++)
            lists[i] = CDR(lists[i]);
    }
    return out;
}

lisp_value builtin_reduce(int argc, lisp_value *argv)
{
    if (argc < 2)
        lisp_error("REDUCE needs a function and a list");
    lisp_value fn = argv[0];
    struct kw kw;
    kw_parse(argc, argv, 2, &kw);
    lisp_value list = argv[1];
    lisp_value acc = kw.has_initial_value ? kw.initial_value : VAL(lisp.sym_unbound);
    while (consp(list)) {
        lisp_value item = kw.has_key ? lisp_kw_key(&kw, CAR(list)) : CAR(list);
        list = CDR(list);
        if (acc == VAL(lisp.sym_unbound)) {
            acc = item;
            continue;
        }
        lisp_value args[2] = { acc, item };
        acc = apply_function(fn, 2, args);
    }
    if (acc == VAL(lisp.sym_unbound))
        return apply_function(fn, 0, NULL);
    return acc;
}

lisp_value builtin_remove(int argc, lisp_value *argv)
{
    if (argc < 2)
        lisp_error("REMOVE needs an item and a sequence");
    lisp_value item = argv[0];
    lisp_value seq = argv[1];
    struct kw kw;
    kw_parse(argc, argv, 2, &kw);
    if (stringp(seq)) {
        uint32_t len = PTR(seq, lisp_string)->len;
        uint32_t cap = PTR(seq, lisp_string)->byte_len + 1;
        char *buffer = heap_alloc(cap);
        size_t at = 0;
        for (uint32_t i = 0; i < len; i++) {
            lisp_value ch = make_char(string_char_at(seq, i));
            lisp_value key = lisp_kw_key(&kw, ch);
            if (!lisp_kw_test(&kw, item, key)) {
                char tmp[4];
                size_t n = utf8_encode(string_char_at(seq, i), tmp);
                memcpy(buffer + at, tmp, n);
                at += n;
            }
        }
        buffer[at] = '\0';
        return make_string_raw(buffer, at, utf8_strlen(buffer, at));
    }
    lisp_value out = NIL_VAL, tail = NIL_VAL;
    while (consp(seq)) {
        lisp_value value = CAR(seq);
        seq = CDR(seq);
        lisp_value key = lisp_kw_key(&kw, value);
        if (lisp_kw_test(&kw, item, key))
            continue;
        lisp_value cell = cons(value, NIL_VAL);
        if (nilp(out))
            out = cell;
        else
            CDR(tail) = cell;
        tail = cell;
    }
    return out;
}

static lisp_value filter_list(int argc, lisp_value *argv, int mode)
{
    if (argc < 2)
        lisp_error("REMOVE-IF needs a predicate and a list");
    lisp_value fn = argv[0];
    lisp_value seq = argv[1];
    struct kw kw;
    kw_parse(argc, argv, 2, &kw);
    if (stringp(seq)) {
        uint32_t len = PTR(seq, lisp_string)->len;
        char *buffer = heap_alloc(PTR(seq, lisp_string)->byte_len + 1);
        size_t at = 0;
        for (uint32_t i = 0; i < len; i++) {
            lisp_value ch = make_char(string_char_at(seq, i));
            lisp_value args[1] = { ch };
            bool hit = truthy(apply_function(fn, 1, args));
            bool keep = mode == 1 ? !hit : hit;
            if (!keep) {
                char tmp[4];
                size_t n = utf8_encode(string_char_at(seq, i), tmp);
                memcpy(buffer + at, tmp, n);
                at += n;
            }
        }
        buffer[at] = '\0';
        return make_string_raw(buffer, at, utf8_strlen(buffer, at));
    }
    lisp_value out = NIL_VAL, tail = NIL_VAL;
    while (consp(seq)) {
        lisp_value value = CAR(seq);
        seq = CDR(seq);
        lisp_value args[1] = { value };
        bool hit = truthy(apply_function(fn, 1, args));
        bool keep = mode == 1 ? !hit : hit;
        if (!keep)
            continue;
        lisp_value cell = cons(value, NIL_VAL);
        if (nilp(out))
            out = cell;
        else
            CDR(tail) = cell;
        tail = cell;
    }
    return out;
}

static bool search_hit(lisp_value probe, bool predicate, struct kw *kw, lisp_value value)
{
    lisp_value key = lisp_kw_key(kw, value);
    if (!predicate)
        return lisp_kw_test(kw, probe, key);
    lisp_value args[1] = { key };
    return truthy(apply_function(probe, 1, args));
}

static lisp_value sequence_search(int argc, lisp_value *argv, bool predicate, bool element)
{
    lisp_value probe = argv[0];
    lisp_value seq = argv[1];
    struct kw kw;
    kw_parse(argc, argv, 2, &kw);
    int64_t start = opt_int(&kw, kw.has_start, kw.start, 0);
    int64_t end = kw.has_end && !nilp(kw.end) ? require_int(kw.end, "keyword value") : INT64_MAX;
    bool from_end = kw.has_from_end && truthy(kw.from_end);
    if (stringp(seq)) {
        int64_t len = PTR(seq, lisp_string)->len;
        if (end > len)
            end = len;
        int64_t step = from_end ? -1 : 1;
        for (int64_t i = from_end ? end - 1 : start; i >= start && i < end; i += step) {
            lisp_value ch = make_char(string_char_at(seq, (uint32_t)i));
            if (search_hit(probe, predicate, &kw, ch))
                return element ? ch : make_int(i);
        }
        return NIL_VAL;
    }
    lisp_value found = NIL_VAL;
    int64_t found_at = -1;
    for (int64_t index = 0; consp(seq) && index < end; seq = CDR(seq), index++) {
        if (index < start || !search_hit(probe, predicate, &kw, CAR(seq)))
            continue;
        found = CAR(seq);
        found_at = index;
        if (!from_end)
            break;
    }
    if (found_at < 0)
        return NIL_VAL;
    return element ? found : make_int(found_at);
}

lisp_value builtin_find(int argc, lisp_value *argv)
{
    if (argc < 2)
        lisp_error("FIND needs an item and a sequence");
    return sequence_search(argc, argv, false, true);
}

static lisp_value find_if_impl(int argc, lisp_value *argv, bool find_mode)
{
    if (argc < 2)
        lisp_error("FIND-IF needs a predicate and a sequence");
    return sequence_search(argc, argv, true, find_mode);
}

lisp_value builtin_position(int argc, lisp_value *argv)
{
    if (argc < 2)
        lisp_error("POSITION needs an item and a sequence");
    return sequence_search(argc, argv, false, false);
}

lisp_value builtin_sort(int argc, lisp_value *argv)
{
    if (argc < 2)
        lisp_error("SORT needs a sequence and a predicate");
    lisp_value pred = argv[1];
    int count = 0;
    lisp_value arr = list_to_array(argv[0], &count);
    lisp_value *values = (lisp_value *)(uintptr_t)arr;
    struct kw kw;
    kw_parse(argc, argv, 2, &kw);
    for (int i = 1; i < count; i++) {
        lisp_value value = values[i];
        int j = i - 1;
        while (j >= 0) {
            lisp_value a = lisp_kw_key(&kw, values[j]);
            lisp_value b = lisp_kw_key(&kw, value);
            lisp_value args[2] = { b, a };
            if (!truthy(apply_function(pred, 2, args)))
                break;
            values[j + 1] = values[j];
            j--;
        }
        values[j + 1] = value;
    }
    return list_from_array(count, values);
}

lisp_value builtin_make_hash_table(int argc, lisp_value *argv)
{
    struct kw kw;
    kw_parse(argc, argv, 0, &kw);
    lisp_value test = kw.has_test ? kw.test : NIL_VAL;
    return make_hashtable(test);
}

static lisp_value *hash_entries(lisp_value table, lisp_value key, bool *found)
{
    lisp_value entries = PTR(table, lisp_hashtable)->entries;
    while (consp(entries)) {
        lisp_value pair = CAR(entries);
        if (hash_table_match(table, CAR(pair), key)) {
            *found = true;
            return &CDR(pair);
        }
        entries = CDR(entries);
    }
    *found = false;
    return NULL;
}

lisp_value builtin_gethash(int argc, lisp_value *argv)
{
    if (argc < 2)
        lisp_error("GETHASH needs a key and a table");
    bool found = false;
    lisp_value *slot = hash_entries(argv[1], argv[0], &found);
    if (found) {
        mv_return2(*slot, T_VAL);
        return *slot;
    }
    lisp_value deflt = argc > 2 ? argv[2] : NIL_VAL;
    mv_return2(deflt, NIL_VAL);
    return deflt;
}

lisp_value builtin_remhash(int argc, lisp_value *argv)
{
    if (argc < 2)
        lisp_error("REMHASH needs a key and a table");
    struct lisp_hashtable *table = PTR(argv[1], lisp_hashtable);
    lisp_value prev = NIL_VAL;
    lisp_value entries = table->entries;
    while (consp(entries)) {
        lisp_value pair = CAR(entries);
        if (hash_table_match(argv[1], CAR(pair), argv[0])) {
            if (nilp(prev))
                table->entries = CDR(entries);
            else
                CDR(prev) = CDR(entries);
            table->count--;
            return T_VAL;
        }
        prev = entries;
        entries = CDR(entries);
    }
    return NIL_VAL;
}

lisp_value builtin_clrhash(int argc, lisp_value *argv)
{
    if (argc < 1)
        lisp_error("CLRHASH needs a table");
    struct lisp_hashtable *table = PTR(argv[0], lisp_hashtable);
    table->entries = NIL_VAL;
    table->count = 0;
    return argv[0];
}

lisp_value builtin_hash_count(int argc, lisp_value *argv)
{
    if (argc < 1)
        lisp_error("HASH-TABLE-COUNT needs a table");
    return make_int(PTR(argv[0], lisp_hashtable)->count);
}

lisp_value builtin_maphash(int argc, lisp_value *argv)
{
    if (argc < 2)
        lisp_error("MAPHASH needs a function and a table");
    lisp_value entries = PTR(argv[1], lisp_hashtable)->entries;
    while (consp(entries)) {
        lisp_value pair = CAR(entries);
        lisp_value args[2] = { CAR(pair), CDR(pair) };
        apply_function(argv[0], 2, args);
        entries = CDR(entries);
    }
    return NIL_VAL;
}

lisp_value builtin_parse_integer(int argc, lisp_value *argv)
{
    if (argc < 1)
        lisp_error("PARSE-INTEGER needs a string");
    size_t bytes = 0;
    const char *text = require_string(argv[0], &bytes, "parse-integer");
    struct kw kw;
    kw_parse(argc, argv, 1, &kw);
    int64_t radix = opt_int(&kw, kw.has_radix, kw.radix, 10);
    uint32_t cps = PTR(argv[0], lisp_string)->len;
    int64_t start = opt_int(&kw, kw.has_start, kw.start, 0);
    int64_t end = opt_int(&kw, kw.has_end, kw.end, (int64_t)cps);
    if (start < 0 || end > (int64_t)cps || start > end)
        lisp_error("PARSE-INTEGER: bad bounds");
    uint32_t total = 0;
    int64_t pos = start;
    bool negative = false;
    if (pos < end) {
        uint32_t cp = string_char_at(argv[0], (uint32_t)pos);
        if (cp == '+' || cp == '-') {
            negative = cp == '-';
            pos++;
        }
    }
    bool any = false;
    int64_t value = 0;
    while (pos < end) {
        uint32_t cp = string_char_at(argv[0], (uint32_t)pos);
        int digit = -1;
        if (cp >= '0' && cp <= '9')
            digit = (int)(cp - '0');
        else if (cp >= 'a' && cp <= 'z')
            digit = (int)(cp - 'a' + 10);
        else if (cp >= 'A' && cp <= 'Z')
            digit = (int)(cp - 'A' + 10);
        if (digit < 0 || digit >= radix)
            break;
        if (__builtin_mul_overflow(value, radix, &value) ||
            __builtin_add_overflow(value, digit, &value))
            lisp_error("PARSE-INTEGER: integer overflow");
        any = true;
        pos++;
    }
    if (!any || (!kw.junk && pos != end))
        lisp_error("PARSE-INTEGER: junk in string");
    if (negative)
        value = -value;
    (void)total;
    (void)bytes;
    (void)text;
    mv_return2(make_int(value), make_int(pos));
    return make_int(value);
}

lisp_value builtin_read_from_string(int argc, lisp_value *argv)
{
    if (argc < 1)
        lisp_error("READ-FROM-STRING needs a string");
    size_t bytes = 0;
    const char *text = require_string(argv[0], &bytes, "read-from-string");
    bool eof_error = argc > 1 ? truthy(argv[1]) : true;
    lisp_value eof_value = argc > 2 ? argv[2] : NIL_VAL;
    struct kw kw;
    kw_parse(argc, argv, 3, &kw);
    int64_t start = opt_int(&kw, kw.has_start, kw.start, 0);
    uint32_t cps = PTR(argv[0], lisp_string)->len;
    if (start < 0 || start > (int64_t)cps)
        lisp_error("READ-FROM-STRING: bad start");
    size_t byte_start = 0;
    if (start > 0) {
        lisp_value prefix = string_from_range(argv[0], 0, (uint32_t)start);
        byte_start = PTR(prefix, lisp_string)->byte_len;
    }
    struct lisp_region *saved = lisp.cur_region;
    (void)saved;
    struct lisp_reader rd;
    reader_init(&rd, "<string>", text + byte_start, bytes - byte_start, lisp.cur_package, lisp.cur_package == PKG_TOMOE);
    if (reader_done(&rd)) {
        if (eof_error)
            lisp_error("READ-FROM-STRING: end of string");
        mv_return2(eof_value, make_int(cps));
        return eof_value;
    }
    (void)bytes;
    lisp_value form = reader_next(&rd, true, NULL);
    uint32_t consumed = utf8_strlen(text + byte_start, rd.pos - 0);
    (void)consumed;
    mv_return2(form, make_int((int64_t)(start + (int64_t)utf8_strlen(text + byte_start, rd.pos))));
    return form;
}

lisp_value builtin_intern(int argc, lisp_value *argv)
{
    if (argc < 1)
        lisp_error("INTERN needs a name");
    const char *name = NULL;
    size_t len = 0;
    char buffer[256];
    if (stringp(argv[0])) {
        name = require_string(argv[0], &len, "intern");
    } else if (symbolp(argv[0])) {
        name = SYM(argv[0])->name;
        len = SYM(argv[0])->name_len;
    } else {
        lisp_error("INTERN needs a string or symbol");
    }
    if (len >= sizeof(buffer))
        lisp_error("INTERN: name too long");
    uint32_t package = lisp.cur_package;
    if (argc > 1 && !nilp(argv[1])) {
        const char *pname = symbolp(argv[1]) ? SYM(argv[1])->name : NULL;
        if (pname && strcmp(pname, "KEYWORD") == 0)
            package = PKG_KEYWORD;
        else if (pname && strcmp(pname, "TOMOE") == 0)
            package = PKG_TOMOE;
        else
            package = lisp.cur_package;
        if (symbol_is_keyword(argv[1]))
            package = PKG_KEYWORD;
    }
    memcpy(buffer, name, len);
    buffer[len] = '\0';
    if (package == PKG_KEYWORD) {
        for (size_t i = 0; i < len; i++)
            buffer[i] = (char)toupper((unsigned char)buffer[i]);
        struct lisp_symbol *sym = symbol_find(PKG_KEYWORD, buffer, len);
        if (!sym)
            sym = symbol_intern(PKG_KEYWORD, buffer, len, false);
        sym->h.flags |= SYM_KEYWORD | SYM_CONSTANT;
        sym->value = VAL(sym);
        mv_return2(VAL(sym), NIL_VAL);
        return VAL(sym);
    }
    struct lisp_symbol *sym = symbol_intern(package, buffer, len, false);
    mv_return2(VAL(sym), NIL_VAL);
    return VAL(sym);
}

lisp_value builtin_symbol_name(int argc, lisp_value *argv)
{
    if (argc < 1 || !symbolp(argv[0]))
        lisp_error("SYMBOL-NAME needs a symbol");
    return make_string_len(SYM(argv[0])->name, SYM(argv[0])->name_len);
}

lisp_value builtin_gensym(int argc, lisp_value *argv)
{
    static uint64_t counter;
    char buffer[64];
    const char *prefix = "G";
    if (argc > 0 && stringp(argv[0])) {
        size_t len = 0;
        const char *text = require_string(argv[0], &len, "gensym prefix");
        char *copy = heap_alloc(len + 1);
        memcpy(copy, text, len);
        copy[len] = '\0';
        prefix = copy;
    }
    snprintf(buffer, sizeof(buffer), "%s%llu", prefix, (unsigned long long)++counter);
    struct lisp_symbol *sym = symbol_intern(PKG_SYSTEM, buffer, strlen(buffer), true);
    return VAL(sym);
}

static bool seq_compare(lisp_value a, lisp_value b, bool equal, bool ignore_case)
{
    if (!stringp(a) || !stringp(b))
        lisp_error("string comparison of a non-string");
    struct lisp_string *x = PTR(a, lisp_string), *y = PTR(b, lisp_string);
    size_t xb = 0, yb = 0;
    uint32_t xlen = x->len, ylen = y->len;
    uint32_t n = xlen < ylen ? xlen : ylen;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t cx = string_char_at(a, i);
        uint32_t cy = string_char_at(b, i);
        if (ignore_case) {
            if (cx < 128)
                cx = (uint32_t)toupper((unsigned char)cx);
            if (cy < 128)
                cy = (uint32_t)toupper((unsigned char)cy);
        }
        if (cx != cy)
            return false;
    }
    (void)xb;
    (void)yb;
    return xlen == ylen;
}

static int seq_order(lisp_value a, lisp_value b, bool ignore_case)
{
    uint32_t xlen = PTR(a, lisp_string)->len, ylen = PTR(b, lisp_string)->len;
    uint32_t n = xlen < ylen ? xlen : ylen;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t cx = string_char_at(a, i);
        uint32_t cy = string_char_at(b, i);
        if (ignore_case) {
            if (cx < 128)
                cx = (uint32_t)toupper((unsigned char)cx);
            if (cy < 128)
                cy = (uint32_t)toupper((unsigned char)cy);
        }
        if (cx < cy)
            return -1;
        if (cx > cy)
            return 1;
    }
    return xlen < ylen ? -1 : xlen > ylen ? 1 : 0;
}

lisp_value builtin_string_eq(int argc, lisp_value *argv)
{
    if (argc < 2)
        lisp_error("STRING= needs two strings");
    lisp_value a = argv[0], b = argv[1];
    struct kw kw;
    kw_parse(argc, argv, 2, &kw);
    if (kw.has_end2 || kw.has_start) {
        if (!stringp(a) || !stringp(b))
            lisp_error("STRING= needs strings");
        uint32_t blen = PTR(b, lisp_string)->len;
        uint32_t start = (uint32_t)opt_int(&kw, kw.has_start, kw.start, 0);
        uint32_t end = (uint32_t)opt_int(&kw, kw.has_end2, kw.end2, (int64_t)blen);
        lisp_value sub = string_from_range(b, start, end);
        return bool_value(seq_compare(a, sub, false, false));
    }
    return bool_value(seq_compare(a, b, false, false));
}

lisp_value builtin_string_equal(int argc, lisp_value *argv)
{
    if (argc < 2)
        lisp_error("STRING-EQUAL needs two strings");
    return bool_value(seq_compare(argv[0], argv[1], false, true));
}

lisp_value builtin_string_lt(int argc, lisp_value *argv)
{
    if (argc < 2)
        lisp_error("STRING< needs two strings");
    return bool_value(seq_order(argv[0], argv[1], false) < 0);
}

lisp_value builtin_string_gt(int argc, lisp_value *argv)
{
    if (argc < 2)
        lisp_error("STRING> needs two strings");
    return bool_value(seq_order(argv[0], argv[1], false) > 0);
}

lisp_value builtin_string_le(int argc, lisp_value *argv)
{
    if (argc < 2)
        lisp_error("STRING<= needs two strings");
    return bool_value(seq_order(argv[0], argv[1], false) <= 0);
}

lisp_value builtin_string_prefix_p(int argc, lisp_value *argv)
{
    if (argc < 2)
        lisp_error("STRING-PREFIX-P needs two strings");
    lisp_value a = argv[0], b = argv[1];
    if (PTR(a, lisp_string)->len > PTR(b, lisp_string)->len)
        return NIL_VAL;
    lisp_value prefix = string_from_range(b, 0, PTR(a, lisp_string)->len);
    return bool_value(seq_compare(a, prefix, false, false));
}

lisp_value builtin_string_suffix_p(int argc, lisp_value *argv)
{
    if (argc < 2)
        lisp_error("STRING-SUFFIX-P needs two strings");
    lisp_value a = argv[0], b = argv[1];
    uint32_t alen = PTR(a, lisp_string)->len, blen = PTR(b, lisp_string)->len;
    if (alen > blen)
        return NIL_VAL;
    lisp_value suffix = string_from_range(b, blen - alen, blen);
    return bool_value(seq_compare(a, suffix, false, false));
}

static lisp_value string_map_chars(lisp_value input, int mode)
{
    struct lisp_string *s = PTR(input, lisp_string);
    char *buffer = heap_alloc(s->byte_len + 1);
    size_t at = 0;
    for (uint32_t i = 0; i < s->len; i++) {
        uint32_t cp = string_char_at(input, i);
        if (cp < 128) {
            if (mode == 'U')
                cp = (uint32_t)toupper((unsigned char)cp);
            else
                cp = (uint32_t)tolower((unsigned char)cp);
        }
        size_t n = utf8_encode(cp, buffer + at);
        at += n;
    }
    buffer[at] = '\0';
    return make_string_raw(buffer, at, s->len);
}

lisp_value builtin_string_upcase(int argc, lisp_value *argv)
{
    if (argc < 1)
        lisp_error("STRING-UPCASE needs a string");
    if (symbolp(argv[0]))
        return string_map_chars(make_string_len(SYM(argv[0])->name, SYM(argv[0])->name_len), 'U');
    if (!stringp(argv[0]))
        lisp_error("STRING-UPCASE needs a string or symbol");
    return string_map_chars(argv[0], 'U');
}

lisp_value builtin_string_downcase(int argc, lisp_value *argv)
{
    if (argc < 1)
        lisp_error("STRING-DOWNCASE needs a string");
    if (symbolp(argv[0]))
        return string_map_chars(make_string_len(SYM(argv[0])->name, SYM(argv[0])->name_len), 'D');
    if (!stringp(argv[0]))
        lisp_error("STRING-DOWNCASE needs a string or symbol");
    return string_map_chars(argv[0], 'D');
}

lisp_value builtin_string_trim(int argc, lisp_value *argv)
{
    if (argc < 2)
        lisp_error("STRING-TRIM needs a character bag and a string");
    lisp_value bag = argv[0];
    lisp_value str = argv[1];
    if (!stringp(str))
        lisp_error("STRING-TRIM needs a string");
    if (symbolp(bag))
        bag = make_string_len(SYM(bag)->name, SYM(bag)->name_len);
    uint32_t len = PTR(str, lisp_string)->len;
    uint32_t start = 0, end = len;
    while (start < end) {
        uint32_t cp = string_char_at(str, start);
        bool hit = false;
        if (stringp(bag)) {
            uint32_t baglen = PTR(bag, lisp_string)->len;
            for (uint32_t i = 0; i < baglen; i++)
                if (string_char_at(bag, i) == cp)
                    hit = true;
        } else {
            for (lisp_value scan = bag; consp(scan); scan = CDR(scan))
                if (charp(CAR(scan)) && PTR(CAR(scan), lisp_char)->code == cp)
                    hit = true;
        }
        if (!hit)
            break;
        start++;
    }
    while (end > start) {
        uint32_t cp = string_char_at(str, end - 1);
        bool hit = false;
        if (stringp(bag)) {
            uint32_t baglen = PTR(bag, lisp_string)->len;
            for (uint32_t i = 0; i < baglen; i++)
                if (string_char_at(bag, i) == cp)
                    hit = true;
        } else {
            for (lisp_value scan = bag; consp(scan); scan = CDR(scan))
                if (charp(CAR(scan)) && PTR(CAR(scan), lisp_char)->code == cp)
                    hit = true;
        }
        if (!hit)
            break;
        end--;
    }
    return string_from_range(str, start, end);
}

lisp_value builtin_concatenate(int argc, lisp_value *argv)
{
    if (argc < 1)
        lisp_error("CONCATENATE needs a result type");
    if (!symbolp(argv[0]) || strcmp(SYM(argv[0])->name, "STRING") != 0)
        lisp_error("CONCATENATE: only 'STRING is supported");
    return string_concat(argc - 1, argv + 1);
}

lisp_value builtin_make_string(int argc, lisp_value *argv)
{
    if (argc < 1)
        lisp_error("MAKE-STRING needs a size");
    int64_t n = require_int(argv[0], "make-string size");
    struct kw kw;
    kw_parse(argc, argv, 1, &kw);
    uint32_t fill = ' ';
    if (kw.has_initial_element)
        fill = require_char(kw.initial_element, "initial element");
    char tmp[4];
    size_t bytes = utf8_encode(fill, tmp);
    char *buffer = heap_alloc((size_t)n * bytes + 1);
    for (int64_t i = 0; i < n; i++)
        memcpy(buffer + (size_t)i * bytes, tmp, bytes);
    buffer[(size_t)n * bytes] = '\0';
    return make_string_raw(buffer, (size_t)n * bytes, (uint32_t)n);
}

lisp_value builtin_search(int argc, lisp_value *argv)
{
    if (argc < 2)
        lisp_error("SEARCH needs two strings");
    if (!stringp(argv[0]) || !stringp(argv[1]))
        lisp_error("SEARCH needs strings");
    struct kw kw;
    kw_parse(argc, argv, 2, &kw);
    lisp_value needle = argv[0], hay = argv[1];
    uint32_t nlen = PTR(needle, lisp_string)->len;
    uint32_t hlen = PTR(hay, lisp_string)->len;
    uint32_t start = (uint32_t)opt_int(&kw, kw.has_start2, kw.start2, 0);
    for (uint32_t i = start; i + nlen <= hlen; i++) {
        bool match = true;
        for (uint32_t k = 0; k < nlen; k++) {
            if (string_char_at(needle, k) != string_char_at(hay, i + k)) {
                match = false;
                break;
            }
        }
        if (match)
            return make_int(i);
    }
    return NIL_VAL;
}

lisp_value builtin_subseq(int argc, lisp_value *argv)
{
    if (argc < 2)
        lisp_error("SUBSEQ needs a sequence and a start");
    int64_t start = require_int(argv[1], "subseq start");
    if (stringp(argv[0])) {
        uint32_t len = PTR(argv[0], lisp_string)->len;
        int64_t end = argc > 2 && !nilp(argv[2]) ? require_int(argv[2], "subseq end") : (int64_t)len;
        if (start < 0 || end > (int64_t)len || start > end)
            lisp_error("SUBSEQ: bad bounds");
        return string_from_range(argv[0], (uint32_t)start, (uint32_t)end);
    }
    lisp_value list = argv[0];
    lisp_value out = NIL_VAL, tail = NIL_VAL;
    int64_t index = 0;
    int64_t end = argc > 2 && !nilp(argv[2]) ? require_int(argv[2], "subseq end") : -1;
    while (consp(list) && (end < 0 || index < end)) {
        if (index >= start) {
            lisp_value cell = cons(CAR(list), NIL_VAL);
            if (nilp(out))
                out = cell;
            else
                CDR(tail) = cell;
            tail = cell;
        }
        list = CDR(list);
        index++;
    }
    return out;
}

lisp_value builtin_map(int argc, lisp_value *argv)
{
    if (argc < 3)
        lisp_error("MAP needs a result type, a function and a sequence");
    if (!symbolp(argv[0]) || strcmp(SYM(argv[0])->name, "STRING") != 0)
        lisp_error("MAP: only 'STRING results are supported");
    lisp_value fn = argv[1];
    lisp_value seq = argv[2];
    uint32_t cap = 64;
    char *buffer = heap_alloc(cap);
    size_t at = 0;
    if (stringp(seq)) {
        uint32_t len = PTR(seq, lisp_string)->len;
        for (uint32_t i = 0; i < len; i++) {
            lisp_value args[1] = { make_char(string_char_at(seq, i)) };
            lisp_value result = apply_function(fn, 1, args);
            if (!charp(result))
                lisp_error("MAP: function did not return a character");
            if (at + 4 >= cap) {
                char *bigger = heap_alloc(cap * 2);
                memcpy(bigger, buffer, at);
                buffer = bigger;
                cap *= 2;
            }
            at += utf8_encode(PTR(result, lisp_char)->code, buffer + at);
        }
    } else {
        while (consp(seq)) {
            lisp_value args[1] = { CAR(seq) };
            seq = CDR(seq);
            lisp_value result = apply_function(fn, 1, args);
            if (!charp(result))
                lisp_error("MAP: function did not return a character");
            if (at + 4 >= cap) {
                char *bigger = heap_alloc(cap * 2);
                memcpy(bigger, buffer, at);
                buffer = bigger;
                cap *= 2;
            }
            at += utf8_encode(PTR(result, lisp_char)->code, buffer + at);
        }
    }
    buffer[at] = '\0';
    return make_string_raw(buffer, at, utf8_strlen(buffer, at));
}

lisp_value builtin_substitute(int argc, lisp_value *argv)
{
    if (argc < 3)
        lisp_error("SUBSTITUTE needs new, old and a sequence");
    lisp_value fresh = argv[0], old = argv[1], seq = argv[2];
    struct kw kw;
    kw_parse(argc, argv, 3, &kw);
    if (stringp(seq)) {
        uint32_t len = PTR(seq, lisp_string)->len;
        char *buffer = heap_alloc(PTR(seq, lisp_string)->byte_len + 16);
        size_t at = 0;
        for (uint32_t i = 0; i < len; i++) {
            uint32_t cp = string_char_at(seq, i);
            lisp_value ch = make_char(cp);
            if (lisp_kw_test(&kw, old, lisp_kw_key(&kw, ch))) {
                if (!charp(fresh))
                    lisp_error("SUBSTITUTE: replacement is not a character");
                at += utf8_encode(PTR(fresh, lisp_char)->code, buffer + at);
            } else {
                at += utf8_encode(cp, buffer + at);
            }
        }
        buffer[at] = '\0';
        return make_string_raw(buffer, at, utf8_strlen(buffer, at));
    }
    lisp_value out = NIL_VAL, tail = NIL_VAL;
    while (consp(seq)) {
        lisp_value value = CAR(seq);
        seq = CDR(seq);
        lisp_value store = lisp_kw_test(&kw, old, lisp_kw_key(&kw, value)) ? fresh : value;
        lisp_value cell = cons(store, NIL_VAL);
        if (nilp(out))
            out = cell;
        else
            CDR(tail) = cell;
        tail = cell;
    }
    return out;
}

static lisp_value value_to_string(lisp_value v, bool readable)
{
    size_t cap = 128;
    for (;;) {
        char *buffer = heap_alloc(cap);
        struct lisp_buf buf;
        buf_init(&buf, buffer, cap);
        print_value(v, readable, &buf);
        if (buf.len + 1 < cap) {
            buffer[buf.len] = '\0';
            return make_string_raw(buffer, buf.len, utf8_strlen(buffer, buf.len));
        }
        cap *= 2;
    }
}

lisp_value builtin_write_to_string(int argc, lisp_value *argv)
{
    if (argc < 1)
        lisp_error("WRITE-TO-STRING needs an object");
    return value_to_string(argv[0], true);
}

lisp_value builtin_prin1_to_string(int argc, lisp_value *argv)
{
    if (argc < 1)
        lisp_error("PRIN1-TO-STRING needs an object");
    return value_to_string(argv[0], true);
}

lisp_value builtin_princ_to_string(int argc, lisp_value *argv)
{
    if (argc < 1)
        lisp_error("PRINC-TO-STRING needs an object");
    return value_to_string(argv[0], false);
}

lisp_value builtin_eq(int argc, lisp_value *argv)
{
    return bool_value(argc >= 2 && argv[0] == argv[1]);
}

lisp_value builtin_eql(int argc, lisp_value *argv)
{
    return bool_value(argc >= 2 && eql_value(argv[0], argv[1]));
}

lisp_value builtin_equal(int argc, lisp_value *argv)
{
    return bool_value(argc >= 2 && equal_value(argv[0], argv[1]));
}

lisp_value builtin_listp(int argc, lisp_value *argv)
{
    if (argc < 1)
        return NIL_VAL;
    lisp_value v = argv[0];
    while (consp(v))
        v = CDR(v);
    return bool_value(nilp(v));
}

lisp_value builtin_null(int argc, lisp_value *argv)
{
    return bool_value(argc < 1 || nilp(argv[0]));
}

lisp_value builtin_not(int argc, lisp_value *argv)
{
    return bool_value(argc < 1 || nilp(argv[0]));
}

lisp_value builtin_consp(int argc, lisp_value *argv)
{
    return bool_value(argc >= 1 && consp(argv[0]));
}

lisp_value builtin_stringp(int argc, lisp_value *argv)
{
    return bool_value(argc >= 1 && stringp(argv[0]));
}

lisp_value builtin_integerp(int argc, lisp_value *argv)
{
    return bool_value(argc >= 1 && intp(argv[0]));
}

lisp_value builtin_realp(int argc, lisp_value *argv)
{
    return bool_value(argc >= 1 && numberp(argv[0]));
}

lisp_value builtin_numberp(int argc, lisp_value *argv)
{
    return bool_value(argc >= 1 && numberp(argv[0]));
}

lisp_value builtin_floatp(int argc, lisp_value *argv)
{
    return bool_value(argc >= 1 && doublep(argv[0]));
}

lisp_value builtin_rationalp(int argc, lisp_value *argv)
{
    return bool_value(argc >= 1 && (intp(argv[0]) || ratiop(argv[0])));
}

lisp_value builtin_characterp(int argc, lisp_value *argv)
{
    return bool_value(argc >= 1 && charp(argv[0]));
}

lisp_value builtin_symbolp(int argc, lisp_value *argv)
{
    return bool_value(argc >= 1 && symbolp(argv[0]));
}

lisp_value builtin_keywordp(int argc, lisp_value *argv)
{
    return bool_value(argc >= 1 && symbol_is_keyword(argv[0]));
}

lisp_value builtin_functionp(int argc, lisp_value *argv)
{
    return bool_value(argc >= 1 && functionp(argv[0]));
}

lisp_value builtin_vectorp(int argc, lisp_value *argv)
{
    (void)argv;
    (void)argc;
    return NIL_VAL;
}

lisp_value builtin_fboundp(int argc, lisp_value *argv)
{
    if (argc < 1 || !symbolp(argv[0]))
        return NIL_VAL;
    return bool_value(!nilp(SYM(argv[0])->function) || SYM(argv[0])->special_id != 0);
}

lisp_value builtin_boundp(int argc, lisp_value *argv)
{
    if (argc < 1 || !symbolp(argv[0]))
        return NIL_VAL;
    return bool_value(SYM(argv[0])->value != VAL(lisp.sym_unbound));
}

lisp_value builtin_symbol_value(int argc, lisp_value *argv)
{
    if (argc < 1 || !symbolp(argv[0]))
        lisp_error("SYMBOL-VALUE needs a symbol");
    if (SYM(argv[0])->value == VAL(lisp.sym_unbound))
        lisp_error("SYMBOL-VALUE: %s is unbound", SYM(argv[0])->name);
    return SYM(argv[0])->value;
}

lisp_value builtin_symbol_function(int argc, lisp_value *argv)
{
    if (argc < 1 || !symbolp(argv[0]))
        lisp_error("SYMBOL-FUNCTION needs a symbol");
    if (nilp(SYM(argv[0])->function))
        lisp_error("SYMBOL-FUNCTION: %s is undefined", SYM(argv[0])->name);
    return SYM(argv[0])->function;
}

static lisp_value arith_variadic(int argc, lisp_value *argv, int op)
{
    if (op == 0) {
        lisp_value acc = make_int(0);
        for (int i = 0; i < argc; i++)
            acc = number_add(acc, require_number(argv[i], "argument"));
        return acc;
    }
    if (op == 2) {
        lisp_value acc = make_int(1);
        for (int i = 0; i < argc; i++)
            acc = number_mul(acc, require_number(argv[i], "argument"));
        return acc;
    }
    if (argc == 0)
        lisp_error("arithmetic needs an argument");
    lisp_value acc = require_number(argv[0], "argument");
    if (op == 1) {
        if (argc == 1)
            return number_sub(make_int(0), acc);
        for (int i = 1; i < argc; i++)
            acc = number_sub(acc, require_number(argv[i], "argument"));
        return acc;
    }
    if (argc == 1)
        return number_div(make_int(1), acc);
    for (int i = 1; i < argc; i++)
        acc = number_div(acc, require_number(argv[i], "argument"));
    return acc;
}

lisp_value builtin_add(int argc, lisp_value *argv) { return arith_variadic(argc, argv, 0); }
lisp_value builtin_sub(int argc, lisp_value *argv) { return arith_variadic(argc, argv, 1); }
lisp_value builtin_mul(int argc, lisp_value *argv) { return arith_variadic(argc, argv, 2); }
lisp_value builtin_div(int argc, lisp_value *argv) { return arith_variadic(argc, argv, 3); }

static bool compare_chain(int argc, lisp_value *argv, int mode)
{
    for (int i = 0; i + 1 < argc; i++) {
        bool ok = true;
        int cmp = number_compare(require_number(argv[i], "argument"),
                                 require_number(argv[i + 1], "argument"), &ok);
        if (!ok)
            return false;
        bool result = false;
        switch (mode) {
        case 0: result = cmp == 0; break;
        case 1: result = cmp != 0; break;
        case 2: result = cmp < 0; break;
        case 3: result = cmp > 0; break;
        case 4: result = cmp <= 0; break;
        case 5: result = cmp >= 0; break;
        }
        if (!result)
            return false;
    }
    return true;
}

lisp_value builtin_num_eq(int argc, lisp_value *argv) { return bool_value(compare_chain(argc, argv, 0)); }
lisp_value builtin_num_ne(int argc, lisp_value *argv) { return bool_value(compare_chain(argc, argv, 1)); }
lisp_value builtin_lt(int argc, lisp_value *argv) { return bool_value(compare_chain(argc, argv, 2)); }
lisp_value builtin_gt(int argc, lisp_value *argv) { return bool_value(compare_chain(argc, argv, 3)); }
lisp_value builtin_le(int argc, lisp_value *argv) { return bool_value(compare_chain(argc, argv, 4)); }
lisp_value builtin_ge(int argc, lisp_value *argv) { return bool_value(compare_chain(argc, argv, 5)); }

lisp_value builtin_min(int argc, lisp_value *argv)
{
    if (argc == 0)
        lisp_error("MIN needs an argument");
    lisp_value best = require_number(argv[0], "argument");
    for (int i = 1; i < argc; i++) {
        bool ok = true;
        lisp_value v = require_number(argv[i], "argument");
        if (number_compare(v, best, &ok) < 0)
            best = v;
    }
    return best;
}

lisp_value builtin_max(int argc, lisp_value *argv)
{
    if (argc == 0)
        lisp_error("MAX needs an argument");
    lisp_value best = require_number(argv[0], "argument");
    for (int i = 1; i < argc; i++) {
        bool ok = true;
        lisp_value v = require_number(argv[i], "argument");
        if (number_compare(v, best, &ok) > 0)
            best = v;
    }
    return best;
}

lisp_value builtin_abs(int argc, lisp_value *argv)
{
    if (argc < 1)
        lisp_error("ABS needs a number");
    lisp_value v = require_number(argv[0], "argument");
    bool ok = true;
    if (number_compare(v, make_int(0), &ok) < 0)
        return number_sub(make_int(0), v);
    return v;
}

lisp_value builtin_mod(int argc, lisp_value *argv)
{
    if (argc < 2)
        lisp_error("MOD needs two numbers");
    lisp_value a = require_number(argv[0], "argument");
    lisp_value b = require_number(argv[1], "argument");
    if (doublep(a) || doublep(b)) {
        double x = number_to_double(a), y = number_to_double(b);
        double r = fmod(x, y);
        if (r != 0 && (r < 0) != (y < 0))
            r += y;
        return make_double(r);
    }
    int64_t an = intp(a) ? INTVAL(a) : PTR(a, lisp_ratio)->num;
    int64_t ad = intp(a) ? 1 : PTR(a, lisp_ratio)->den;
    int64_t bn = intp(b) ? INTVAL(b) : PTR(b, lisp_ratio)->num;
    int64_t bd = intp(b) ? 1 : PTR(b, lisp_ratio)->den;
    if (bn == 0)
        lisp_error("division by zero");
    __int128 num = (__int128)an * bd;
    __int128 den = (__int128)ad * bn;
    __int128 q = num / den;
    __int128 rem = num - q * den;
    if (rem != 0 && ((rem < 0) != (den < 0)))
        rem += den;
    __int128 den2 = (__int128)ad * bd;
    if (den2 == 1)
        return make_int((int64_t)rem);
    return make_ratio((int64_t)rem, (int64_t)den2);
}

lisp_value builtin_one_plus(int argc, lisp_value *argv)
{
    if (argc < 1)
        lisp_error("1+ needs a number");
    return number_add(require_number(argv[0], "argument"), make_int(1));
}

lisp_value builtin_one_minus(int argc, lisp_value *argv)
{
    if (argc < 1)
        lisp_error("1- needs a number");
    return number_sub(require_number(argv[0], "argument"), make_int(1));
}

lisp_value builtin_zerop(int argc, lisp_value *argv)
{
    bool ok = true;
    return bool_value(argc >= 1 && number_compare(require_number(argv[0], "argument"), make_int(0), &ok) == 0);
}

lisp_value builtin_plusp(int argc, lisp_value *argv)
{
    bool ok = true;
    return bool_value(argc >= 1 && number_compare(require_number(argv[0], "argument"), make_int(0), &ok) > 0);
}

lisp_value builtin_minusp(int argc, lisp_value *argv)
{
    bool ok = true;
    return bool_value(argc >= 1 && number_compare(require_number(argv[0], "argument"), make_int(0), &ok) < 0);
}

static int64_t integer_value(lisp_value v, const char *what)
{
    if (!intp(v))
        lisp_error("%s must be an integer", what);
    return INTVAL(v);
}

lisp_value builtin_evenp(int argc, lisp_value *argv)
{
    return bool_value((integer_value(argc >= 1 ? argv[0] : NIL_VAL, "EVENP") % 2) == 0);
}

lisp_value builtin_oddp(int argc, lisp_value *argv)
{
    return bool_value((integer_value(argc >= 1 ? argv[0] : NIL_VAL, "ODDP") % 2) != 0);
}

lisp_value builtin_signum(int argc, lisp_value *argv)
{
    bool ok = true;
    lisp_value v = require_number(argc >= 1 ? argv[0] : NIL_VAL, "argument");
    int cmp = number_compare(v, make_int(0), &ok);
    if (cmp < 0)
        return make_int(-1);
    if (cmp > 0)
        return make_int(1);
    return doublep(v) ? make_double(0.0) : make_int(0);
}

lisp_value builtin_logand(int argc, lisp_value *argv)
{
    int64_t acc = -1;
    for (int i = 0; i < argc; i++)
        acc &= integer_value(argv[i], "LOGAND");
    return make_int(acc);
}

lisp_value builtin_logior(int argc, lisp_value *argv)
{
    int64_t acc = 0;
    for (int i = 0; i < argc; i++)
        acc |= integer_value(argv[i], "LOGIOR");
    return make_int(acc);
}

lisp_value builtin_logandc2(int argc, lisp_value *argv)
{
    if (argc < 2)
        lisp_error("LOGANDC2 needs two integers");
    return make_int(integer_value(argv[0], "LOGANDC2") & ~integer_value(argv[1], "LOGANDC2"));
}

lisp_value builtin_logxor(int argc, lisp_value *argv)
{
    int64_t acc = 0;
    for (int i = 0; i < argc; i++)
        acc ^= integer_value(argv[i], "LOGXOR");
    return make_int(acc);
}

lisp_value builtin_lognot(int argc, lisp_value *argv)
{
    return make_int(~integer_value(argc > 0 ? argv[0] : NIL_VAL, "LOGNOT"));
}

lisp_value builtin_logtest(int argc, lisp_value *argv)
{
    if (argc < 2)
        lisp_error("LOGTEST needs two integers");
    int64_t a = integer_value(argv[0], "LOGTEST");
    int64_t b = integer_value(argv[1], "LOGTEST");
    return bool_value((a & b) != 0);
}

lisp_value builtin_ash(int argc, lisp_value *argv)
{
    if (argc < 2)
        lisp_error("ASH needs two integers");
    int64_t value = integer_value(argv[0], "ASH");
    int64_t count = integer_value(argv[1], "ASH");
    if (count >= 0) {
        if (count > 62)
            lisp_error("arithmetic overflow");
        int64_t result;
        if (__builtin_mul_overflow(value, (int64_t)1 << count, &result))
            lisp_error("arithmetic overflow");
        return make_int(result);
    }
    return make_int(value >> (-count));
}

static lisp_value round_like(int argc, lisp_value *argv, int mode)
{
    if (argc < 1)
        lisp_error("rounding needs a number");
    lisp_value a = require_number(argv[0], "argument");
    lisp_value b = argc > 1 ? require_number(argv[1], "argument") : make_int(1);
    bool ok = true;
    if (doublep(a) || doublep(b)) {
        double x = number_to_double(a), y = number_to_double(b);
        if (y == 0)
            lisp_error("division by zero");
        double q = x / y;
        double rounded;
        switch (mode) {
        case 0: rounded = floor(q); break;
        case 1: rounded = ceil(q); break;
        case 2: rounded = nearbyint(q); break;
        default: rounded = trunc(q); break;
        }
        double r = x - rounded * y;
        if (!(rounded >= -9223372036854775808.0 && rounded < 9223372036854775808.0))
            lisp_error("arithmetic overflow");
        mv_return2(make_int((int64_t)rounded), make_double(r));
        return lisp.mv[0];
    }
    int64_t an = intp(a) ? INTVAL(a) : PTR(a, lisp_ratio)->num;
    int64_t ad = intp(a) ? 1 : PTR(a, lisp_ratio)->den;
    int64_t bn = intp(b) ? INTVAL(b) : PTR(b, lisp_ratio)->num;
    int64_t bd = intp(b) ? 1 : PTR(b, lisp_ratio)->den;
    if (bn == 0)
        lisp_error("division by zero");
    __int128 num = (__int128)an * bd;
    __int128 den = (__int128)ad * bn;
    if (den < 0) {
        num = -num;
        den = -den;
    }
    __int128 q = num / den;
    __int128 rem = num - q * den;
    if (rem != 0) {
        if (mode == 0 && rem < 0)
            q -= 1;
        else if (mode == 1 && rem > 0)
            q += 1;
        else if (mode == 2) {
            __int128 magnitude = rem < 0 ? -rem : rem;
            __int128 twice = magnitude * 2;
            if (twice > den) {
                if (rem > 0)
                    q += 1;
                else
                    q -= 1;
            } else if (twice == den && (q % 2) != 0) {
                if (rem > 0)
                    q += 1;
                else
                    q -= 1;
            }
        }
    }
    __int128 remainder = num - q * den;
    __int128 rden = (__int128)ad * bd;
    lisp_value qv = make_int((int64_t)q);
    lisp_value rv;
    if (rden == 1)
        rv = make_int((int64_t)remainder);
    else
        rv = make_ratio((int64_t)remainder, (int64_t)rden);
    (void)ok;
    mv_return2(qv, rv);
    return qv;
}

lisp_value builtin_ceiling(int argc, lisp_value *argv) { return round_like(argc, argv, 1); }
lisp_value builtin_round(int argc, lisp_value *argv) { return round_like(argc, argv, 2); }
lisp_value builtin_truncate(int argc, lisp_value *argv) { return round_like(argc, argv, 3); }

lisp_value builtin_floor(int argc, lisp_value *argv) { return round_like(argc, argv, 0); }

lisp_value builtin_float(int argc, lisp_value *argv)
{
    if (argc < 1)
        lisp_error("FLOAT needs a number");
    if (doublep(argv[0]))
        return argv[0];
    return make_double(number_to_double(require_number(argv[0], "argument")));
}

lisp_value builtin_coerce(int argc, lisp_value *argv)
{
    if (argc < 2)
        lisp_error("COERCE needs an object and a type");
    lisp_value type = argv[1];
    const char *name = symbolp(type) ? SYM(type)->name : "";
    if (strcmp(name, "DOUBLE-FLOAT") == 0 || strcmp(name, "SINGLE-FLOAT") == 0)
        return make_double(number_to_double(require_number(argv[0], "argument")));
    if (strcmp(name, "STRING") == 0) {
        if (stringp(argv[0]))
            return argv[0];
        if (symbolp(argv[0]))
            return make_string_len(SYM(argv[0])->name, SYM(argv[0])->name_len);
        if (charp(argv[0])) {
            char tmp[4];
            size_t n = utf8_encode(PTR(argv[0], lisp_char)->code, tmp);
            return make_string_len(tmp, n);
        }
    }
    if (strcmp(name, "LIST") == 0) {
        if (stringp(argv[0])) {
            uint32_t len = PTR(argv[0], lisp_string)->len;
            lisp_value out = NIL_VAL;
            for (uint32_t i = len; i > 0; i--)
                out = cons(make_char(string_char_at(argv[0], i - 1)), out);
            return out;
        }
    }
    lisp_error("COERCE: unsupported type");
}

lisp_value builtin_sqrt(int argc, lisp_value *argv)
{
    lisp_value v = require_number(argc >= 1 ? argv[0] : NIL_VAL, "argument");
    if (intp(v) && INTVAL(v) >= 0) {
        int64_t root = (int64_t)llround(sqrt((double)INTVAL(v)));
        if (root * root == INTVAL(v))
            return make_int(root);
    }
    return make_double(sqrt(number_to_double(v)));
}

lisp_value builtin_sin(int argc, lisp_value *argv) { return make_double(sin(number_to_double(require_number(argc >= 1 ? argv[0] : NIL_VAL, "argument")))); }
lisp_value builtin_cos(int argc, lisp_value *argv) { return make_double(cos(number_to_double(require_number(argc >= 1 ? argv[0] : NIL_VAL, "argument")))); }
lisp_value builtin_tanh(int argc, lisp_value *argv) { return make_double(tanh(number_to_double(require_number(argc >= 1 ? argv[0] : NIL_VAL, "argument")))); }
lisp_value builtin_cosh(int argc, lisp_value *argv) { return make_double(cosh(number_to_double(require_number(argc >= 1 ? argv[0] : NIL_VAL, "argument")))); }
lisp_value builtin_acosh(int argc, lisp_value *argv) { return make_double(acosh(number_to_double(require_number(argc >= 1 ? argv[0] : NIL_VAL, "argument")))); }

lisp_value builtin_expt(int argc, lisp_value *argv)
{
    if (argc < 2)
        lisp_error("EXPT needs two numbers");
    lisp_value base = require_number(argv[0], "argument");
    lisp_value power = require_number(argv[1], "argument");
    if (intp(base) && intp(power) && INTVAL(power) >= 0) {
        int64_t result = 1;
        int64_t b = INTVAL(base);
        int64_t e = INTVAL(power);
        while (e > 0) {
            if (e & 1) {
                if (__builtin_mul_overflow(result, b, &result))
                    return make_double(pow(number_to_double(base), number_to_double(power)));
            }
            e >>= 1;
            if (e) {
                if (__builtin_mul_overflow(b, b, &b))
                    return make_double(pow(number_to_double(base), number_to_double(power)));
            }
        }
        return make_int(result);
    }
    return make_double(pow(number_to_double(base), number_to_double(power)));
}

lisp_value builtin_random(int argc, lisp_value *argv)
{
    if (argc < 1)
        lisp_error("RANDOM needs a limit");
    lisp_value n = require_number(argv[0], "argument");
    lisp.rng_state ^= lisp.rng_state << 13;
    lisp.rng_state ^= lisp.rng_state >> 7;
    lisp.rng_state ^= lisp.rng_state << 17;
    if (intp(n)) {
        int64_t limit = INTVAL(n);
        if (limit <= 0)
            lisp_error("RANDOM: bad limit");
        return make_int((int64_t)(lisp.rng_state % (uint64_t)limit));
    }
    double limit = number_to_double(n);
    return make_double(limit * (double)(lisp.rng_state >> 11) / 9007199254740992.0);
}

lisp_value builtin_get_internal_time(int argc, lisp_value *argv)
{
    (void)argc;
    (void)argv;
    return make_int((int64_t)(lisp_now_ns() / 1000ull));
}

lisp_value builtin_finite_p(int argc, lisp_value *argv)
{
    if (argc < 1)
        return NIL_VAL;
    return bool_value(number_finite(require_number(argv[0], "argument")));
}

lisp_value builtin_length(int argc, lisp_value *argv)
{
    if (argc < 1)
        lisp_error("LENGTH needs a sequence");
    if (stringp(argv[0]))
        return make_int(PTR(argv[0], lisp_string)->len);
    lisp_value list = argv[0];
    int64_t n = 0;
    while (consp(list)) {
        n++;
        list = CDR(list);
    }
    return make_int(n);
}

lisp_value builtin_char(int argc, lisp_value *argv)
{
    if (argc < 2)
        lisp_error("CHAR needs a string and an index");
    if (!stringp(argv[0]))
        lisp_error("CHAR needs a string");
    int64_t index = require_int(argv[1], "index");
    if (index < 0 || index >= (int64_t)PTR(argv[0], lisp_string)->len)
        lisp_error("CHAR: index out of range");
    return make_char(string_char_at(argv[0], (uint32_t)index));
}

lisp_value builtin_char_eq(int argc, lisp_value *argv)
{
    if (argc < 2)
        lisp_error("CHAR= needs two characters");
    uint32_t a = require_char(argv[0], "argument");
    for (int i = 1; i < argc; i++)
        if (a != require_char(argv[i], "argument"))
            return NIL_VAL;
    return T_VAL;
}

lisp_value builtin_char_lt(int argc, lisp_value *argv)
{
    if (argc < 2)
        lisp_error("CHAR< needs two characters");
    for (int i = 0; i + 1 < argc; i++)
        if (require_char(argv[i], "argument") >= require_char(argv[i + 1], "argument"))
            return NIL_VAL;
    return T_VAL;
}

lisp_value builtin_char_code(int argc, lisp_value *argv)
{
    if (argc < 1)
        lisp_error("CHAR-CODE needs a character");
    return make_int(require_char(argv[0], "argument"));
}

lisp_value builtin_code_char(int argc, lisp_value *argv)
{
    if (argc < 1)
        lisp_error("CODE-CHAR needs a code");
    return make_char((uint32_t)require_int(argv[0], "code"));
}

lisp_value builtin_string(lisp_value *argv, int argc)
{
    if (argc < 1)
        lisp_error("STRING needs an object");
    if (stringp(argv[0]))
        return argv[0];
    if (symbolp(argv[0]))
        return make_string_len(SYM(argv[0])->name, SYM(argv[0])->name_len);
    if (charp(argv[0])) {
        char tmp[4];
        size_t n = utf8_encode(PTR(argv[0], lisp_char)->code, tmp);
        return make_string_len(tmp, n);
    }
    lisp_error("STRING: unsupported object");
}

lisp_value builtin_char_pred(int argc, lisp_value *argv)
{
    if (argc < 1)
        return NIL_VAL;
    uint32_t cp = require_char(argv[0], "argument");
    return bool_value(cp < 128 && (isalpha((unsigned char)cp) || isdigit((unsigned char)cp) || cp == ' '));
}

lisp_value builtin_alpha_char_p(int argc, lisp_value *argv)
{
    if (argc < 1)
        return NIL_VAL;
    uint32_t cp = require_char(argv[0], "argument");
    return bool_value(cp < 128 && isalpha((unsigned char)cp));
}

lisp_value builtin_alphanumericp(int argc, lisp_value *argv)
{
    if (argc < 1)
        return NIL_VAL;
    uint32_t cp = require_char(argv[0], "argument");
    return bool_value(cp < 128 && isalnum((unsigned char)cp));
}

lisp_value builtin_digit_char_p(int argc, lisp_value *argv)
{
    if (argc < 1)
        return NIL_VAL;
    uint32_t cp = require_char(argv[0], "argument");
    int64_t radix = argc > 1 ? require_int(argv[1], "radix") : 10;
    int digit = -1;
    if (cp >= '0' && cp <= '9')
        digit = (int)(cp - '0');
    else if (cp >= 'a' && cp <= 'z')
        digit = (int)(cp - 'a' + 10);
    else if (cp >= 'A' && cp <= 'Z')
        digit = (int)(cp - 'A' + 10);
    if (digit < 0 || digit >= radix)
        return NIL_VAL;
    return make_int(digit);
}

lisp_value builtin_char_upcase(int argc, lisp_value *argv)
{
    if (argc < 1)
        lisp_error("CHAR-UPCASE needs a character");
    uint32_t cp = require_char(argv[0], "argument");
    if (cp < 128)
        cp = (uint32_t)toupper((unsigned char)cp);
    return make_char(cp);
}

lisp_value builtin_cons(int argc, lisp_value *argv)
{
    return cons(argc > 0 ? argv[0] : NIL_VAL, argc > 1 ? argv[1] : NIL_VAL);
}

struct copy_budget {
    int64_t nodes;
    int64_t limit;
    int64_t chars;
};

_Noreturn void lisp_fail_format(const char *control, int argc, lisp_value *argv)
{
    lisp_value message = lisp_format(control, strlen(control), argc, argv);
    throw_error_value(make_condition_obj(PTR(message, lisp_string)->data));
}

static lisp_value copy_walk(struct copy_budget *budget, lisp_value item, int depth)
{
    if (++budget->nodes > budget->limit || depth > 64)
        lisp_fail_format("Extension data exceeds the node/depth budget.", 0, NULL);
    switch (TYPE(item)) {
    case LT_CONS: {
        lisp_value head = NIL_VAL, last = NIL_VAL, tail = item;
        while (consp(tail)) {
            if (++budget->nodes > budget->limit)
                lisp_fail_format("Extension data exceeds the node budget.", 0, NULL);
            lisp_value cell = cons(copy_walk(budget, CAR(tail), depth + 1), NIL_VAL);
            if (nilp(head))
                head = cell;
            else
                CDR(last) = cell;
            last = cell;
            tail = CDR(tail);
        }
        lisp_value end = copy_walk(budget, tail, depth + 1);
        CDR(last) = end;
        return head;
    }
    case LT_STRING:
        if ((int64_t)PTR(item, lisp_string)->len > budget->chars) {
            lisp_value limit = make_int(budget->chars);
            lisp_fail_format("String exceeds ~D characters.", 1, &limit);
        }
        return item;
    case LT_INT:
    case LT_RATIO:
    case LT_SYMBOL:
        return item;
    case LT_DOUBLE:
        if (!(DBLVAL(item) == DBLVAL(item)) || fabs(DBLVAL(item)) > 1.7976931348623157e308)
            lisp_fail_format("Only finite floats are extension data.", 0, NULL);
        return item;
    default:
        lisp_fail_format("Not extension data: ~S", 1, &item);
    }
}

lisp_value builtin_copy_data(int argc, lisp_value *argv)
{
    struct copy_budget budget = {
        0, require_int(argv[1], "node limit"), require_int(argv[2], "character limit")
    };
    return copy_walk(&budget, argv[0], 0);
}

static struct lisp_symbol *sym_runtime, *sym_pending_writes, *sym_retired_state, *sym_keep, *sym_release;

struct runtime_cache_entry {
    lisp_value state;
    lisp_value key;
    lisp_value *cell;
    uint64_t epoch;
};

static struct runtime_cache_entry runtime_cache[64];

void lisp_runtime_symbols_init(void)
{
    memset(runtime_cache, 0, sizeof(runtime_cache));
    for (int i = 0; i < KW_IDS; i++)
        kw_symbols[i] = VAL(symbol_intern(PKG_KEYWORD, kw_names[i], strlen(kw_names[i]), false));
    sym_runtime = symbol_intern(PKG_TOMOE, "*RUNTIME*", 9, false);
    sym_pending_writes = symbol_intern(PKG_TOMOE, "*PENDING-WRITES*", 16, false);
    sym_retired_state = symbol_intern(PKG_TOMOE, "*RETIRED-STATE*", 15, false);
    sym_keep = symbol_intern(PKG_TOMOE, "%KEEP", 5, false);
    sym_release = symbol_intern(PKG_TOMOE, "%RELEASE", 8, false);
}

static lisp_value *plist_cell(lisp_value plist, lisp_value key)
{
    for (; consp(plist) && consp(CDR(plist)); plist = CDR(CDR(plist)))
        if (CAR(plist) == key)
            return &CAR(CDR(plist));
    return NULL;
}


lisp_value builtin_runtime_read(int argc, lisp_value *argv)
{
    lisp_value key = argv[0];
    lisp_value *cell = plist_cell(sym_pending_writes->value, key);
    if (cell)
        return *cell;
    lisp_value state = sym_runtime->value;
    struct runtime_cache_entry *entry = &runtime_cache[(key >> 4) & 63];
    if (entry->state != state || entry->key != key || entry->epoch != lisp.reclaim_count) {
        entry->state = state;
        entry->key = key;
        entry->cell = plist_cell(state, key);
        entry->epoch = lisp.reclaim_count;
    }
    return entry->cell ? *entry->cell : NIL_VAL;
}

lisp_value builtin_runtime_write(int argc, lisp_value *argv)
{
    struct lisp_symbol *writes = sym_pending_writes;
    lisp_value *cell = plist_cell(writes->value, argv[0]);
    if (cell)
        *cell = argv[1];
    else
        writes->value = cons(argv[0], cons(argv[1], writes->value));
    return argv[1];
}

lisp_value builtin_persist_state(int argc, lisp_value *argv)
{
    struct lisp_symbol *runtime = sym_runtime;
    struct lisp_symbol *writes = sym_pending_writes;
    struct lisp_symbol *retired = sym_retired_state;
    lisp_value state = runtime->value;
    for (lisp_value scan = writes->value; consp(scan) && consp(CDR(scan)); scan = CDR(CDR(scan))) {
        lisp_value *cell = plist_cell(state, CAR(scan));
        if (cell)
            *cell = CAR(CDR(scan));
        else
            state = cons(CAR(scan), cons(CAR(CDR(scan)), state));
    }
    writes->value = NIL_VAL;
    lisp_value old = runtime->value;
    lisp_value kept = apply_function(VAL(sym_keep), 1, &state);
    runtime->value = kept;
    if (truthy(retired->value))
        apply_function(VAL(sym_release), 1, &retired->value);
    retired->value = old;
    return kept;
}

lisp_value builtin_keep(int argc, lisp_value *argv)
{
    if (argc < 1)
        return NIL_VAL;
    return lisp_keep(argv[0]);
}

lisp_value builtin_release(int argc, lisp_value *argv)
{
    if (argc >= 1)
        lisp_release(argv[0]);
    return NIL_VAL;
}

lisp_value builtin_condition_message(int argc, lisp_value *argv)
{
    if (argc < 1)
        return make_string_len("", 0);
    return condition_message(argv[0]);
}

lisp_value builtin_with_limits(int argc, lisp_value *argv)
{
    if (argc < 3)
        lisp_error("%%WITH-LIMITS needs steps, milliseconds and a function");
    uint64_t steps = nilp(argv[0]) ? 0 : (uint64_t)require_int(argv[0], "steps");
    uint64_t ms = (uint64_t)require_int(argv[1], "milliseconds");
    lisp_value fn = argv[2];
    uint64_t saved_steps = lisp.steps;
    uint64_t saved_limit = lisp.steps_limit;
    uint64_t saved_deadline = lisp.deadline_at;
    lisp.steps = 0;
    lisp.steps_limit = steps;
    lisp.deadline_at = ms ? lisp_now_ns() + ms * 1000000ull : 0;
    int ctl = push_ctl(CTL_CATCH);
    if (setjmp(lisp.ctl[ctl].jb) != 0) {
        lisp.ctl_top = ctl;
        lisp.steps = saved_steps;
        lisp.steps_limit = saved_limit;
        lisp.deadline_at = saved_deadline;
        throw_error_value(lisp_escape_condition());
    }
    lisp_value result = apply_function(fn, 0, NULL);
    lisp.ctl_top = ctl;
    lisp.steps = saved_steps;
    lisp.steps_limit = saved_limit;
    lisp.deadline_at = saved_deadline;
    return result;
}

lisp_value builtin_eval(int argc, lisp_value *argv)
{
    if (argc < 1)
        lisp_error("EVAL needs a form");
    return lisp_analyse_and_eval(argv[0], NULL);
}

lisp_value builtin_typep(int argc, lisp_value *argv)
{
    if (argc < 2)
        return NIL_VAL;
    lisp_value v = argv[0];
    lisp_value type = argv[1];
    if (consp(type))
        return NIL_VAL;
    if (!symbolp(type))
        return NIL_VAL;
    const char *name = SYM(type)->name;
    if (strcmp(name, "INTEGER") == 0)
        return bool_value(intp(v));
    if (strcmp(name, "STRING") == 0)
        return bool_value(stringp(v));
    if (strcmp(name, "SYMBOL") == 0)
        return bool_value(symbolp(v));
    if (strcmp(name, "KEYWORD") == 0)
        return bool_value(symbol_is_keyword(v));
    if (strcmp(name, "LIST") == 0)
        return builtin_listp(1, &v);
    if (strcmp(name, "CONS") == 0)
        return bool_value(consp(v));
    if (strcmp(name, "FUNCTION") == 0)
        return bool_value(functionp(v));
    if (strcmp(name, "REAL") == 0 || strcmp(name, "NUMBER") == 0)
        return bool_value(numberp(v));
    if (strcmp(name, "DOUBLE-FLOAT") == 0)
        return bool_value(doublep(v));
    if (strcmp(name, "RATIO") == 0)
        return bool_value(ratiop(v));
    if (strcmp(name, "CHARACTER") == 0)
        return bool_value(charp(v));
    return NIL_VAL;
}

lisp_value builtin_catch(int argc, lisp_value *argv)
{
    if (argc < 2)
        lisp_error("%%CATCH needs a thunk and a handler");
    int ctl = push_ctl(CTL_CATCH);
    if (setjmp(lisp.ctl[ctl].jb) != 0) {
        lisp.ctl_top = ctl;
        lisp_value condition = lisp_escape_condition();
        lisp_value message = condition_message(condition);
        lisp_value args[1] = { message };
        return apply_function(argv[1], 1, args);
    }
    lisp_value result = apply_function(argv[0], 0, NULL);
    lisp.ctl_top = ctl;
    return result;
}

lisp_value builtin_read_source(int argc, lisp_value *argv)
{
    if (argc < 2)
        lisp_error("%%READ-SOURCE needs text and a package name");
    size_t len = 0;
    const char *text = require_string(argv[0], &len, "source text");
    uint32_t package = lisp.cur_package;
    if (symbolp(argv[1]))
        package = package_find_or_create(SYM(argv[1])->name);
    else if (stringp(argv[1]))
        package = package_find_or_create(PTR(argv[1], lisp_string)->data);
    else if (!nilp(argv[1]))
        lisp_error("%%READ-SOURCE needs a package name");
    struct lisp_reader rd;
    reader_init(&rd, "<source>", text, len, package, package == PKG_TOMOE);
    lisp_value head = NIL_VAL, tail = NIL_VAL;
    while (!reader_done(&rd)) {
        lisp_value form = reader_next(&rd, false, NULL);
        lisp_value cell = cons(form, NIL_VAL);
        if (nilp(head))
            head = cell;
        else
            CDR(tail) = cell;
        tail = cell;
    }
    return head;
}

lisp_value builtin_symbol_definition(int argc, lisp_value *argv)
{
    if (argc < 1 || !symbolp(argv[0]))
        lisp_error("%%SYMBOL-DEFINITION needs a symbol");
    return SYM(argv[0])->function;
}

lisp_value builtin_symbol_package_name(int argc, lisp_value *argv)
{
    if (argc < 1 || !symbolp(argv[0]))
        lisp_error("%%SYMBOL-PACKAGE-NAME needs a symbol");
    struct lisp_symbol *sym = SYM(argv[0]);
    if ((sym->h.flags & SYM_UNINTERNED) || sym->package >= LISP_MAX_PACKAGES ||
        !lisp.packages[sym->package].used)
        return NIL_VAL;
    return lisp_cstring(lisp.packages[sym->package].name);
}

lisp_value builtin_set_symbol_definition(int argc, lisp_value *argv)
{
    if (argc < 2 || !symbolp(argv[0]))
        lisp_error("%%SET-SYMBOL-DEFINITION needs a symbol and a definition");
    SYM(argv[0])->function = argv[1];
    if (nilp(argv[1]))
        SYM(argv[0])->h.flags &= (uint8_t)~SYM_MACRO;
    return argv[1];
}

lisp_value builtin_remove_if(int argc, lisp_value *argv)
{
    return filter_list(argc, argv, 1);
}

lisp_value builtin_remove_if_not(int argc, lisp_value *argv)
{
    return filter_list(argc, argv, 2);
}

lisp_value builtin_find_if(int argc, lisp_value *argv)
{
    return find_if_impl(argc, argv, true);
}

lisp_value builtin_position_if(int argc, lisp_value *argv)
{
    return find_if_impl(argc, argv, false);
}

lisp_value builtin_mapc(int argc, lisp_value *argv)
{
    return builtin_map_like(argc, argv, 0);
}

lisp_value builtin_mapcan(int argc, lisp_value *argv)
{
    return builtin_map_like(argc, argv, 1);
}

lisp_value builtin_stable_sort(int argc, lisp_value *argv)
{
    return builtin_sort(argc, argv);
}

lisp_value builtin_string_impl(int argc, lisp_value *argv)
{
    return builtin_string(argv, argc);
}

lisp_value builtin_parse_number(int argc, lisp_value *argv)
{
    if (argc < 1)
        lisp_error("PARSE-NUMBER needs a string");
    size_t len = 0;
    const char *text = require_string(argv[0], &len, "parse-number");
    struct lisp_reader rd;
    reader_init(&rd, "<number>", text, len, lisp.cur_package, false);
    if (reader_done(&rd))
        lisp_error("PARSE-NUMBER: empty string");
    lisp_value value = reader_next(&rd, true, NULL);
    if (!numberp(value))
        lisp_error("PARSE-NUMBER: not a number");
    return value;
}

lisp_value builtin_symbol_value_cell(int argc, lisp_value *argv)
{
    if (argc < 1 || !symbolp(argv[0]))
        lisp_error("%%SYMBOL-VALUE needs a symbol");
    if (SYM(argv[0])->value == VAL(lisp.sym_unbound)) {
        mv_return2(NIL_VAL, NIL_VAL);
        return NIL_VAL;
    }
    mv_return2(SYM(argv[0])->value, T_VAL);
    return SYM(argv[0])->value;
}

lisp_value builtin_set_symbol_value_cell(int argc, lisp_value *argv)
{
    if (argc < 2 || !symbolp(argv[0]))
        lisp_error("%%SET-SYMBOL-VALUE needs a symbol and a value");
    SYM(argv[0])->value = argv[1];
    return argv[1];
}

lisp_value builtin_unbind_symbol(int argc, lisp_value *argv)
{
    if (argc < 1 || !symbolp(argv[0]))
        lisp_error("%%UNBIND-SYMBOL needs a symbol");
    SYM(argv[0])->value = VAL(lisp.sym_unbound);
    return NIL_VAL;
}
