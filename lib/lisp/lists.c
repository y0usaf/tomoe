#include <string.h>

#include "lisp-internal.h"

static lisp_value car_of(lisp_value v)
{
    if (nilp(v))
        return NIL_VAL;
    if (!consp(v))
        lisp_error("CAR of a non-list");
    return CAR(v);
}

static lisp_value cdr_of(lisp_value v)
{
    if (nilp(v))
        return NIL_VAL;
    if (!consp(v))
        lisp_error("CDR of a non-list");
    return CDR(v);
}

lisp_value builtin_car(int argc, lisp_value *argv) { return car_of(argc > 0 ? argv[0] : NIL_VAL); }
lisp_value builtin_cdr(int argc, lisp_value *argv) { return cdr_of(argc > 0 ? argv[0] : NIL_VAL); }
lisp_value builtin_cddr(int argc, lisp_value *argv) { return cdr_of(cdr_of(argc > 0 ? argv[0] : NIL_VAL)); }
lisp_value builtin_caar(int argc, lisp_value *argv) { return car_of(car_of(argc > 0 ? argv[0] : NIL_VAL)); }
lisp_value builtin_cdar(int argc, lisp_value *argv) { return cdr_of(car_of(argc > 0 ? argv[0] : NIL_VAL)); }

lisp_value builtin_first(int argc, lisp_value *argv) { return car_of(argc > 0 ? argv[0] : NIL_VAL); }
lisp_value builtin_second(int argc, lisp_value *argv) { return car_of(cdr_of(argc > 0 ? argv[0] : NIL_VAL)); }
lisp_value builtin_third(int argc, lisp_value *argv) { return car_of(cdr_of(cdr_of(argc > 0 ? argv[0] : NIL_VAL))); }
lisp_value builtin_fourth(int argc, lisp_value *argv) { return car_of(cdr_of(cdr_of(cdr_of(argc > 0 ? argv[0] : NIL_VAL)))); }
lisp_value builtin_fifth(int argc, lisp_value *argv) { return car_of(cdr_of(cdr_of(cdr_of(cdr_of(argc > 0 ? argv[0] : NIL_VAL))))); }
lisp_value builtin_rest(int argc, lisp_value *argv) { return cdr_of(argc > 0 ? argv[0] : NIL_VAL); }

lisp_value builtin_sixth(int argc, lisp_value *argv)
{
    return car_of(cdr_of(cdr_of(cdr_of(cdr_of(cdr_of(argc > 0 ? argv[0] : NIL_VAL))))));
}

lisp_value builtin_nth(int argc, lisp_value *argv)
{
    if (argc < 2)
        lisp_error("NTH needs an index and a list");
    return list_nth(argv[1], require_int(argv[0], "index"));
}

lisp_value builtin_nthcdr(int argc, lisp_value *argv)
{
    if (argc < 2)
        lisp_error("NTHCDR needs an index and a list");
    return list_nthcdr(argv[1], require_int(argv[0], "index"));
}

lisp_value builtin_last(int argc, lisp_value *argv)
{
    if (argc < 1)
        return NIL_VAL;
    int64_t n = argc > 1 ? require_int(argv[1], "count") : 1;
    lisp_value list = argv[0];
    if (n <= 0)
        return NIL_VAL;
    int64_t total = list_count(list);
    if (n >= total)
        return list;
    return list_nthcdr(list, total - n);
}

lisp_value builtin_list(int argc, lisp_value *argv)
{
    return list_from_array(argc, argv);
}

lisp_value builtin_list_star(int argc, lisp_value *argv)
{
    if (argc == 0)
        return NIL_VAL;
    lisp_value out = argv[argc - 1];
    for (int i = argc - 2; i >= 0; i--)
        out = cons(argv[i], out);
    return out;
}

lisp_value builtin_acons(int argc, lisp_value *argv)
{
    if (argc < 3)
        lisp_error("ACONS needs key, value and alist");
    return cons(cons(argv[0], argv[1]), argv[2]);
}

lisp_value builtin_append(int argc, lisp_value *argv)
{
    if (argc == 0)
        return NIL_VAL;
    lisp_value out = argv[argc - 1];
    for (int i = argc - 2; i >= 0; i--)
        out = list_append2(argv[i], out);
    return out;
}

lisp_value builtin_copy_list(int argc, lisp_value *argv)
{
    return argc > 0 ? list_reverse_copy(list_reverse_copy(argv[0])) : NIL_VAL;
}

lisp_value builtin_reverse(int argc, lisp_value *argv)
{
    if (argc < 1)
        return NIL_VAL;
    if (stringp(argv[0])) {
        uint32_t len = PTR(argv[0], lisp_string)->len;
        uint32_t cap = PTR(argv[0], lisp_string)->byte_len + 1;
        char *buffer = heap_alloc(cap);
        size_t at = 0;
        for (uint32_t i = len; i > 0; i--)
            at += utf8_encode(string_char_at(argv[0], i - 1), buffer + at);
        buffer[at] = '\0';
        return make_string_raw(buffer, at, len);
    }
    return list_reverse_copy(argv[0]);
}

lisp_value builtin_nreverse(int argc, lisp_value *argv)
{
    if (argc < 1)
        return NIL_VAL;
    lisp_value prev = NIL_VAL;
    lisp_value list = argv[0];
    while (consp(list)) {
        check_steps();
        lisp_value next = CDR(list);
        CDR(list) = prev;
        prev = list;
        list = next;
    }
    return prev;
}

lisp_value builtin_make_list(int argc, lisp_value *argv)
{
    if (argc < 1)
        lisp_error("MAKE-LIST needs a size");
    int64_t n = require_int(argv[0], "size");
    struct kw kw;
    kw_parse(argc, argv, 1, &kw);
    lisp_value fill = kw.has_initial_element ? kw.initial_element : NIL_VAL;
    lisp_value out = NIL_VAL;
    for (int64_t i = 0; i < n; i++)
        out = cons(fill, out);
    return out;
}

lisp_value builtin_getf(int argc, lisp_value *argv)
{
    if (argc < 2)
        lisp_error("GETF needs a plist and a key");
    lisp_value deflt = argc > 2 ? argv[2] : NIL_VAL;
    return keyword_get(argv[0], argv[1], deflt);
}

lisp_value builtin_member(int argc, lisp_value *argv)
{
    if (argc < 2)
        lisp_error("MEMBER needs an item and a list");
    struct kw kw;
    kw_parse(argc, argv, 2, &kw);
    lisp_value list = argv[1];
    while (consp(list)) {
        check_steps();
        lisp_value value = CAR(list);
        lisp_value key = kw.has_key ? lisp_kw_key(&kw, value) : value;
        if (lisp_kw_test(&kw, argv[0], key))
            return list;
        list = CDR(list);
    }
    return NIL_VAL;
}

lisp_value builtin_assoc(int argc, lisp_value *argv)
{
    if (argc < 2)
        lisp_error("ASSOC needs a key and an alist");
    struct kw kw;
    kw_parse(argc, argv, 2, &kw);
    lisp_value list = argv[1];
    while (consp(list)) {
        check_steps();
        lisp_value pair = CAR(list);
        lisp_value key = consp(pair) ? CAR(pair) : pair;
        if (kw.has_key) {
            lisp_value args[1] = { key };
            key = apply_function(kw.key, 1, args);
        }
        bool hit;
        if (kw.has_test) {
            if (!lisp_kw_direct_test(kw.test, argv[0], key, &hit)) {
                lisp_value args[2] = { argv[0], key };
                hit = truthy(apply_function(kw.test, 2, args));
            }
        } else {
            hit = eql_value(argv[0], key);
        }
        if (hit)
            return pair;
        list = CDR(list);
    }
    return NIL_VAL;
}

static lisp_value quantifier(int argc, lisp_value *argv, int mode)
{
    if (argc < 2)
        lisp_error("quantifier needs a predicate and a list");
    lisp_value fn = argv[0];
    int lists_count = argc - 1;
    lisp_value lists[16];
    if (lists_count > 16)
        lisp_error("too many lists");
    for (int i = 0; i < lists_count; i++)
        lists[i] = argv[i + 1];
    while (true) {
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
        lisp_value result = apply_function(fn, lists_count, args);
        if (mode == 1 && truthy(result))
            return result;
        if (mode == 2 && nilp(result))
            return NIL_VAL;
        if (mode == 3 && truthy(result))
            return NIL_VAL;
        for (int i = 0; i < lists_count; i++)
            lists[i] = CDR(lists[i]);
    }
    return (mode == 2 || mode == 3) ? T_VAL : NIL_VAL;
}

lisp_value builtin_some(int argc, lisp_value *argv) { return quantifier(argc, argv, 1); }
lisp_value builtin_every(int argc, lisp_value *argv) { return quantifier(argc, argv, 2); }
lisp_value builtin_notany(int argc, lisp_value *argv) { return quantifier(argc, argv, 3); }

lisp_value builtin_count_if(int argc, lisp_value *argv)
{
    if (argc < 2)
        lisp_error("COUNT-IF needs a predicate and a list");
    lisp_value fn = argv[0];
    lisp_value list = argv[1];
    struct kw kw;
    kw_parse(argc, argv, 2, &kw);
    int64_t count = 0;
    while (consp(list)) {
        check_steps();
        lisp_value value = CAR(list);
        list = CDR(list);
        if (kw.has_key) {
            lisp_value args[1] = { value };
            value = apply_function(kw.key, 1, args);
        }
        lisp_value args[1] = { value };
        if (truthy(apply_function(fn, 1, args)))
            count++;
    }
    return make_int(count);
}

static bool contains(lisp_value list, lisp_value item, struct kw *kw)
{
    lisp_value probe = kw->has_key ? lisp_kw_key(kw, item) : item;
    while (consp(list)) {
        check_steps();
        lisp_value value = CAR(list);
        list = CDR(list);
        lisp_value key = kw->has_key ? lisp_kw_key(kw, value) : value;
        if (lisp_kw_test(kw, probe, key))
            return true;
    }
    return false;
}

static void shorter_list(lisp_value list1, lisp_value list2, lisp_value *shorter, lisp_value *longer,
                         size_t *short_length, size_t *long_rest)
{
    size_t length = 0;
    lisp_value a = list1, b = list2;
    for (;;) {
        if (!consp(a)) {
            *shorter = list1;
            *longer = list2;
            break;
        }
        if (!consp(b)) {
            *shorter = list2;
            *longer = list1;
            b = a;
            break;
        }
        check_steps();
        a = CDR(a);
        b = CDR(b);
        length++;
    }
    *short_length = length;
    size_t rest = 0;
    while (consp(b) && rest < 100) {
        rest++;
        b = CDR(b);
    }
    *long_rest = rest;
}

static bool standard_hash_test(struct kw *kw)
{
    if (kw->has_test_not && !nilp(kw->test_not))
        return false;
    if (!kw->has_test || nilp(kw->test))
        return true;
    if (TYPE(kw->test) != LT_FUNCTION)
        return false;
    struct lisp_function *f = PTR(kw->test, lisp_function);
    return f->kind == FN_BUILTIN &&
           (f->fn == builtin_eq || f->fn == builtin_eql || f->fn == builtin_equal);
}

lisp_value builtin_set_difference(int argc, lisp_value *argv)
{
    if (argc < 2)
        lisp_error("SET-DIFFERENCE needs two lists");
    struct kw kw;
    kw_parse(argc, argv, 2, &kw);
    if (nilp(argv[1]))
        return argv[0];
    lisp_value out = NIL_VAL;
    lisp_value list = argv[0];
    while (consp(list)) {
        check_steps();
        lisp_value value = CAR(list);
        list = CDR(list);
        if (!contains(argv[1], value, &kw))
            out = cons(value, out);
    }
    return out;
}

lisp_value builtin_intersection(int argc, lisp_value *argv)
{
    if (argc < 2)
        lisp_error("INTERSECTION needs two lists");
    struct kw kw;
    kw_parse(argc, argv, 2, &kw);
    if (nilp(argv[0]) || nilp(argv[1]))
        return NIL_VAL;
    lisp_value shorter, longer;
    size_t short_length, long_rest;
    shorter_list(argv[0], argv[1], &shorter, &longer, &short_length, &long_rest);
    bool hashing = standard_hash_test(&kw) &&
                   (short_length > 20 || (short_length >= 3 && long_rest + short_length >= 100));
    lisp_value out = NIL_VAL;
    lisp_value scan = hashing ? longer : shorter;
    lisp_value other = hashing ? shorter : longer;
    while (consp(scan)) {
        check_steps();
        lisp_value value = CAR(scan);
        scan = CDR(scan);
        if (contains(other, value, &kw))
            out = cons(value, out);
    }
    return out;
}

lisp_value builtin_subsetp(int argc, lisp_value *argv)
{
    if (argc < 2)
        lisp_error("SUBSETP needs two lists");
    struct kw kw;
    kw_parse(argc, argv, 2, &kw);
    lisp_value list = argv[0];
    while (consp(list)) {
        check_steps();
        lisp_value value = CAR(list);
        list = CDR(list);
        if (!contains(argv[1], value, &kw))
            return NIL_VAL;
    }
    return T_VAL;
}

lisp_value builtin_adjoin(int argc, lisp_value *argv)
{
    if (argc < 2)
        lisp_error("ADJOIN needs an item and a list");
    struct kw kw;
    kw_parse(argc, argv, 2, &kw);
    if (contains(argv[1], argv[0], &kw))
        return argv[1];
    return cons(argv[0], argv[1]);
}

lisp_value builtin_nconc(int argc, lisp_value *argv)
{
    if (argc == 0)
        return NIL_VAL;
    lisp_value out = NIL_VAL;
    lisp_value tail = NIL_VAL;
    for (int i = 0; i < argc; i++) {
        lisp_value list = argv[i];
        if (nilp(list))
            continue;
        if (!consp(list))
            lisp_error("NCONC of a non-list");
        if (nilp(out))
            out = list;
        else
            CDR(tail) = heap_store(OBJ(tail), list);
        tail = list_last(list);
    }
    return out;
}

lisp_value builtin_delete(int argc, lisp_value *argv)
{
    return builtin_remove(argc, argv);
}

lisp_value builtin_delete_if(int argc, lisp_value *argv)
{
    return builtin_remove_if(argc, argv);
}

lisp_value builtin_remove_duplicates(int argc, lisp_value *argv)
{
    if (argc < 1)
        return NIL_VAL;
    struct kw kw;
    kw_parse(argc, argv, 1, &kw);
    lisp_value out = NIL_VAL, tail = NIL_VAL;
    lisp_value list = argv[0];
    while (consp(list)) {
        check_steps();
        lisp_value value = CAR(list);
        list = CDR(list);
        if (contains(out, value, &kw))
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

static lisp_value copy_tree(lisp_value value, int depth)
{
    if (!consp(value))
        return value;
    if (depth > LISP_MAX_DEPTH)
        lisp_error("COPY-TREE: the tree nests more than %d deep", LISP_MAX_DEPTH);
    lisp_value out = NIL_VAL, tail = NIL_VAL;
    while (consp(value)) {
        check_steps();
        lisp_value cell = cons(copy_tree(CAR(value), depth + 1), NIL_VAL);
        if (nilp(out))
            out = cell;
        else
            CDR(tail) = cell;
        tail = cell;
        value = CDR(value);
    }
    CDR(tail) = value;
    return out;
}

lisp_value builtin_copy_tree(int argc, lisp_value *argv)
{
    return argc < 1 ? NIL_VAL : copy_tree(argv[0], 0);
}

lisp_value builtin_copy_seq(int argc, lisp_value *argv)
{
    if (argc < 1)
        return NIL_VAL;
    if (stringp(argv[0]))
        return argv[0];
    if (nilp(argv[0]) || consp(argv[0]))
        return list_reverse_copy(list_reverse_copy(argv[0]));
    lisp_error("COPY-SEQ needs a list or a string");
}

lisp_value builtin_byte(int argc, lisp_value *argv)
{
    if (argc < 2)
        lisp_error("BYTE needs a size and a position");
    return cons(make_int(require_int(argv[0], "byte size")),
                make_int(require_int(argv[1], "byte position")));
}

lisp_value builtin_ldb(int argc, lisp_value *argv)
{
    if (argc < 2)
        lisp_error("LDB needs a byte spec and an integer");
    if (!consp(argv[0]))
        lisp_error("LDB needs a byte spec");
    int64_t size = require_int(CAR(argv[0]), "byte size");
    int64_t position = require_int(CDR(argv[0]), "byte position");
    int64_t value = require_int(argv[1], "integer");
    if (size < 0 || size > 63 || position < 0 || position + size > 64)
        lisp_error("LDB: bad byte spec");
    uint64_t mask = size == 0 ? 0 : (UINT64_C(1) << size) - 1;
    return make_int((int64_t)(((uint64_t)value >> position) & mask));
}

lisp_value builtin_union(int argc, lisp_value *argv)
{
    if (argc < 2)
        lisp_error("UNION needs two lists");
    struct kw kw;
    kw_parse(argc, argv, 2, &kw);
    if (nilp(argv[0]))
        return argv[1];
    if (nilp(argv[1]))
        return argv[0];
    lisp_value shorter, longer;
    size_t short_length, long_rest;
    shorter_list(argv[0], argv[1], &shorter, &longer, &short_length, &long_rest);
    lisp_value out = shorter;
    while (consp(longer)) {
        check_steps();
        lisp_value value = CAR(longer);
        longer = CDR(longer);
        if (!contains(shorter, value, &kw))
            out = cons(value, out);
    }
    return out;
}
