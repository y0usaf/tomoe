#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <unistd.h>

#include "lisp-internal.h"

static char name_pool[LISP_NAME_POOL];
static uint32_t intern_table[LISP_MAX_SYMBOLS * 2];

static void init_region(struct lisp_region *region, int kind, const char *name)
{
    memset(region, 0, sizeof(*region));
    region->kind = kind;
    region->in_use = true;
    region->first = UINT32_MAX;
    region->last = UINT32_MAX;
    snprintf(region->name, sizeof(region->name), "%s", name);
}

static void define_builtin(const char *name, lisp_primitive fn, int min_args, int max_args)
{
    lisp_register_builtin(name, fn, min_args, max_args);
}

static void define_internal(const char *name, lisp_primitive fn, int min_args, int max_args)
{
    lisp_register_internal(name, fn, min_args, max_args);
}

static void define_constant(const char *name, lisp_value value)
{
    struct lisp_symbol *sym = symbol_intern(PKG_TOMOE, name, strlen(name), false);
    sym->value = value;
    sym->h.flags |= SYM_CONSTANT | SYM_VISIBLE;
}

int init_symbols(void)
{
    lisp.sym_nil = symbol_intern(PKG_SYSTEM, "NIL", 3, false);
    lisp.sym_nil->value = VAL(lisp.sym_nil);
    lisp.sym_nil->h.flags |= SYM_CONSTANT | SYM_SPECIAL | SYM_VISIBLE;
    lisp.sym_t = symbol_intern(PKG_SYSTEM, "T", 1, false);
    lisp.sym_t->value = VAL(lisp.sym_t);
    lisp.sym_t->h.flags |= SYM_CONSTANT | SYM_SPECIAL | SYM_VISIBLE;
    lisp.sym_unbound = symbol_intern(PKG_SYSTEM, "UNBOUND", 7, false);

    lisp.sym_quote = symbol_intern(PKG_TOMOE, "QUOTE", 5, false);
    lisp.sym_function = symbol_intern(PKG_TOMOE, "FUNCTION", 8, false);
    lisp.sym_quasiquote = symbol_intern(PKG_TOMOE, "QUASIQUOTE", 10, false);
    lisp.sym_unquote = symbol_intern(PKG_TOMOE, "UNQUOTE", 7, false);
    lisp.sym_unquote_splicing = symbol_intern(PKG_TOMOE, "UNQUOTE-SPLICING", 16, false);
    lisp.sym_rest = symbol_intern(PKG_TOMOE, "&REST", 5, false);
    lisp.sym_optional = symbol_intern(PKG_TOMOE, "&OPTIONAL", 9, false);
    lisp.sym_key = symbol_intern(PKG_TOMOE, "&KEY", 4, false);
    lisp.sym_allow_other_keys = symbol_intern(PKG_TOMOE, "&ALLOW-OTHER-KEYS", 17, false);
    lisp.sym_aux = symbol_intern(PKG_TOMOE, "&AUX", 4, false);
    lisp.sym_body = symbol_intern(PKG_TOMOE, "&BODY", 5, false);
    lisp.sym_whole = symbol_intern(PKG_TOMOE, "&WHOLE", 6, false);
    lisp.sym_environment = symbol_intern(PKG_TOMOE, "&ENVIRONMENT", 12, false);
    lisp.sym_otherwise = symbol_intern(PKG_TOMOE, "OTHERWISE", 9, false);
    lisp.sym_error = symbol_intern(PKG_TOMOE, "ERROR", 5, false);
    lisp.sym_otherwise->h.flags |= SYM_VISIBLE;
    struct lisp_symbol *list_keywords[] = {
        lisp.sym_rest, lisp.sym_optional, lisp.sym_key, lisp.sym_allow_other_keys,
        lisp.sym_aux, lisp.sym_body, lisp.sym_whole, lisp.sym_environment
    };
    for (size_t i = 0; i < sizeof(list_keywords) / sizeof(list_keywords[0]); i++)
        list_keywords[i]->h.flags |= SYM_VISIBLE;

    lisp.sym_keyword_test = symbol_intern(PKG_KEYWORD, "TEST", 4, false);
    lisp.sym_keyword_key = symbol_intern(PKG_KEYWORD, "KEY", 3, false);
    lisp.sym_keyword_initial_element = symbol_intern(PKG_KEYWORD, "INITIAL-ELEMENT", 15, false);
    lisp.sym_keyword_start = symbol_intern(PKG_KEYWORD, "START", 5, false);
    lisp.sym_keyword_end = symbol_intern(PKG_KEYWORD, "END", 3, false);
    lisp.sym_keyword_from_end = symbol_intern(PKG_KEYWORD, "FROM-END", 8, false);
    lisp.sym_keyword_radix = symbol_intern(PKG_KEYWORD, "RADIX", 5, false);
    lisp.sym_keyword_junk_allowed = symbol_intern(PKG_KEYWORD, "JUNK-ALLOWED", 12, false);
    lisp.sym_keyword_start2 = symbol_intern(PKG_KEYWORD, "START2", 6, false);
    lisp.sym_keyword_end2 = symbol_intern(PKG_KEYWORD, "END2", 4, false);
    lisp.sym_keyword_initial_value = symbol_intern(PKG_KEYWORD, "INITIAL-VALUE", 13, false);
    lisp.sym_keyword_test_not = symbol_intern(PKG_KEYWORD, "TEST-NOT", 8, false);
    struct lisp_symbol *keywords[] = {
        lisp.sym_keyword_test, lisp.sym_keyword_key, lisp.sym_keyword_initial_element,
        lisp.sym_keyword_start, lisp.sym_keyword_end, lisp.sym_keyword_from_end,
        lisp.sym_keyword_radix, lisp.sym_keyword_junk_allowed, lisp.sym_keyword_start2,
        lisp.sym_keyword_end2, lisp.sym_keyword_initial_value, lisp.sym_keyword_test_not
    };
    for (size_t i = 0; i < sizeof(keywords) / sizeof(keywords[0]); i++) {
        keywords[i]->h.flags |= SYM_KEYWORD | SYM_CONSTANT;
        keywords[i]->value = VAL(keywords[i]);
    }
    return 0;
}

int init_primitives(void)
{
    define_builtin("CAR", builtin_car, 1, 1);
    define_builtin("CDR", builtin_cdr, 1, 1);
    define_builtin("CDDR", builtin_cddr, 1, 1);
    define_builtin("CAAR", builtin_caar, 1, 1);
    define_builtin("CDAR", builtin_cdar, 1, 1);
    define_builtin("SIXTH", builtin_sixth, 1, 1);
    define_builtin("FIRST", builtin_first, 1, 1);
    define_builtin("SECOND", builtin_second, 1, 1);
    define_builtin("THIRD", builtin_third, 1, 1);
    define_builtin("FOURTH", builtin_fourth, 1, 1);
    define_builtin("FIFTH", builtin_fifth, 1, 1);
    define_builtin("REST", builtin_rest, 1, 1);
    define_builtin("NTH", builtin_nth, 2, 2);
    define_builtin("NTHCDR", builtin_nthcdr, 2, 2);
    define_builtin("LAST", builtin_last, 1, 2);
    define_builtin("LIST", builtin_list, 0, -1);
    define_builtin("LIST*", builtin_list_star, 0, -1);
    define_builtin("CONS", builtin_cons, 2, 2);
    define_builtin("ACONS", builtin_acons, 3, 3);
    define_builtin("APPEND", builtin_append, 0, -1);
    define_builtin("COPY-LIST", builtin_copy_list, 1, 1);
    define_builtin("COPY-SEQ", builtin_copy_seq, 1, 1);
    define_builtin("REVERSE", builtin_reverse, 1, 1);
    define_builtin("NREVERSE", builtin_nreverse, 1, 1);
    define_builtin("LENGTH", builtin_length, 1, 1);
    define_builtin("MAKE-LIST", builtin_make_list, 1, -1);
    define_builtin("GETF", builtin_getf, 2, 3);
    define_builtin("MEMBER", builtin_member, 2, -1);
    define_builtin("ASSOC", builtin_assoc, 2, -1);
    define_builtin("MAPCAR", builtin_mapcar, 2, -1);
    define_builtin("MAPC", builtin_mapc, 2, -1);
    define_builtin("MAPCAN", builtin_mapcan, 2, -1);
    define_builtin("APPLY", builtin_apply, 2, -1);
    define_builtin("FUNCALL", builtin_funcall, 1, -1);
    define_builtin("REDUCE", builtin_reduce, 2, -1);
    define_builtin("REMOVE", builtin_remove, 2, -1);
    define_builtin("REMOVE-IF", builtin_remove_if, 2, -1);
    define_builtin("REMOVE-IF-NOT", builtin_remove_if_not, 2, -1);
    define_builtin("DELETE", builtin_delete, 2, -1);
    define_builtin("DELETE-IF", builtin_delete_if, 2, -1);
    define_builtin("REMOVE-DUPLICATES", builtin_remove_duplicates, 1, -1);
    define_builtin("FIND", builtin_find, 2, -1);
    define_builtin("FIND-IF", builtin_find_if, 2, -1);
    define_builtin("POSITION", builtin_position, 2, -1);
    define_builtin("POSITION-IF", builtin_position_if, 2, -1);
    define_builtin("SOME", builtin_some, 2, -1);
    define_builtin("EVERY", builtin_every, 2, -1);
    define_builtin("NOTANY", builtin_notany, 2, -1);
    define_builtin("COUNT-IF", builtin_count_if, 2, -1);
    define_builtin("SET-DIFFERENCE", builtin_set_difference, 2, -1);
    define_builtin("INTERSECTION", builtin_intersection, 2, -1);
    define_builtin("UNION", builtin_union, 2, -1);
    define_builtin("SUBSETP", builtin_subsetp, 2, -1);
    define_builtin("ADJOIN", builtin_adjoin, 2, -1);
    define_builtin("NCONC", builtin_nconc, 0, -1);
    define_builtin("COPY-TREE", builtin_copy_tree, 1, 1);
    define_builtin("SORT", builtin_sort, 2, -1);
    define_builtin("STABLE-SORT", builtin_stable_sort, 2, -1);
    define_builtin("SUBSEQ", builtin_subseq, 2, 3);
    define_builtin("SUBSTITUTE", builtin_substitute, 3, -1);
    define_builtin("SEARCH", builtin_search, 2, -1);
    define_builtin("MAP", builtin_map, 3, 3);

    define_builtin("NULL", builtin_null, 1, 1);
    define_builtin("NOT", builtin_not, 1, 1);
    define_builtin("LISTP", builtin_listp, 1, 1);
    define_builtin("CONSP", builtin_consp, 1, 1);
    define_builtin("STRINGP", builtin_stringp, 1, 1);
    define_builtin("INTEGERP", builtin_integerp, 1, 1);
    define_builtin("REALP", builtin_realp, 1, 1);
    define_builtin("ZEROP", builtin_zerop, 1, 1);
    define_builtin("PLUSP", builtin_plusp, 1, 1);
    define_builtin("MINUSP", builtin_minusp, 1, 1);
    define_builtin("EVENP", builtin_evenp, 1, 1);
    define_builtin("ODDP", builtin_oddp, 1, 1);
    define_builtin("DIGIT-CHAR-P", builtin_digit_char_p, 1, 2);
    define_builtin("KEYWORDP", builtin_keywordp, 1, 1);
    define_builtin("SYMBOLP", builtin_symbolp, 1, 1);
    define_builtin("FUNCTIONP", builtin_functionp, 1, 1);
    define_builtin("NUMBERP", builtin_numberp, 1, 1);
    define_builtin("FLOATP", builtin_floatp, 1, 1);
    define_builtin("RATIONALP", builtin_rationalp, 1, 1);
    define_builtin("CHARACTERP", builtin_characterp, 1, 1);
    define_builtin("VECTORP", builtin_vectorp, 1, 1);
    define_builtin("FBOUNDP", builtin_fboundp, 1, 1);
    define_builtin("BOUNDP", builtin_boundp, 1, 1);
    define_builtin("SYMBOL-VALUE", builtin_symbol_value, 1, 1);
    define_builtin("SYMBOL-FUNCTION", builtin_symbol_function, 1, 1);
    define_builtin("TYPEP", builtin_typep, 2, 2);

    define_builtin("+", builtin_add, 0, -1);
    define_builtin("-", builtin_sub, 1, -1);
    define_builtin("*", builtin_mul, 0, -1);
    define_builtin("/", builtin_div, 1, -1);
    define_builtin("1+", builtin_one_plus, 1, 1);
    define_builtin("1-", builtin_one_minus, 1, 1);
    define_builtin("=", builtin_num_eq, 1, -1);
    define_builtin("/=", builtin_num_ne, 1, -1);
    define_builtin("<", builtin_lt, 1, -1);
    define_builtin(">", builtin_gt, 1, -1);
    define_builtin("<=", builtin_le, 1, -1);
    define_builtin(">=", builtin_ge, 1, -1);
    define_builtin("MIN", builtin_min, 1, -1);
    define_builtin("MAX", builtin_max, 1, -1);
    define_builtin("ABS", builtin_abs, 1, 1);
    define_builtin("MOD", builtin_mod, 2, 2);
    define_builtin("FLOOR", builtin_floor, 1, 2);
    define_builtin("CEILING", builtin_ceiling, 1, 2);
    define_builtin("ROUND", builtin_round, 1, 2);
    define_builtin("TRUNCATE", builtin_truncate, 1, 2);
    define_builtin("FLOAT", builtin_float, 1, 2);
    define_builtin("COERCE", builtin_coerce, 2, 2);
    define_builtin("SQRT", builtin_sqrt, 1, 1);
    define_builtin("SIN", builtin_sin, 1, 1);
    define_builtin("COS", builtin_cos, 1, 1);
    define_builtin("TANH", builtin_tanh, 1, 1);
    define_builtin("COSH", builtin_cosh, 1, 1);
    define_builtin("ACOSH", builtin_acosh, 1, 1);
    define_builtin("EXPT", builtin_expt, 2, 2);
    define_builtin("LOGTEST", builtin_logtest, 2, 2);
    define_builtin("LDB", builtin_ldb, 2, 2);
    define_builtin("BYTE", builtin_byte, 2, 2);
    define_builtin("LOGAND", builtin_logand, 0, -1);
    define_builtin("LOGANDC2", builtin_logandc2, 2, 2);
    define_builtin("LOGXOR", builtin_logxor, 0, -1);
    define_builtin("LOGNOT", builtin_lognot, 1, 1);
    define_builtin("LOGIOR", builtin_logior, 0, -1);
    define_builtin("ASH", builtin_ash, 2, 2);
    define_builtin("SIGNUM", builtin_signum, 1, 1);
    define_builtin("RANDOM", builtin_random, 1, 1);
    define_builtin("GET-INTERNAL-REAL-TIME", builtin_get_internal_time, 0, 0);
    define_builtin("%FINITE-P", builtin_finite_p, 1, 1);

    define_builtin("CHAR", builtin_char, 2, 2);
    define_builtin("CHAR=", builtin_char_eq, 2, -1);
    define_builtin("CHAR<", builtin_char_lt, 2, -1);
    define_builtin("CHAR-CODE", builtin_char_code, 1, 1);
    define_builtin("CODE-CHAR", builtin_code_char, 1, 1);
    define_builtin("ALPHA-CHAR-P", builtin_alpha_char_p, 1, 1);
    define_builtin("ALPHANUMERICP", builtin_alphanumericp, 1, 1);
    define_builtin("CHAR-UPCASE", builtin_char_upcase, 1, 1);
    define_builtin("STRING", builtin_string_impl, 1, 1);
    define_builtin("STRING=", builtin_string_eq, 2, -1);
    define_builtin("STRING-EQUAL", builtin_string_equal, 2, -1);
    define_builtin("STRING<", builtin_string_lt, 2, 2);
    define_builtin("STRING>", builtin_string_gt, 2, 2);
    define_builtin("STRING<=", builtin_string_le, 2, 2);
    define_builtin("STRING-UPCASE", builtin_string_upcase, 1, 1);
    define_builtin("STRING-DOWNCASE", builtin_string_downcase, 1, 1);
    define_builtin("STRING-TRIM", builtin_string_trim, 2, 2);
    define_builtin("STRING-PREFIX-P", builtin_string_prefix_p, 2, 2);
    define_builtin("STRING-SUFFIX-P", builtin_string_suffix_p, 2, 2);
    define_builtin("CONCATENATE", builtin_concatenate, 1, -1);
    define_builtin("MAKE-STRING", builtin_make_string, 1, -1);
    define_builtin("PARSE-INTEGER", builtin_parse_integer, 1, -1);
    define_builtin("PARSE-NUMBER", builtin_parse_number, 1, 1);
    define_builtin("READ-FROM-STRING", builtin_read_from_string, 1, -1);
    define_builtin("INTERN", builtin_intern, 1, 2);
    define_builtin("SYMBOL-NAME", builtin_symbol_name, 1, 1);
    define_builtin("GENSYM", builtin_gensym, 0, 1);
    define_builtin("FORMAT", builtin_format, 2, -1);
    define_builtin("PRINC-TO-STRING", builtin_princ_to_string, 1, 1);
    define_builtin("PRIN1-TO-STRING", builtin_prin1_to_string, 1, 1);
    define_builtin("WRITE-TO-STRING", builtin_write_to_string, 1, 1);

    define_builtin("EQ", builtin_eq, 2, 2);
    define_builtin("EQL", builtin_eql, 2, 2);
    define_builtin("EQUAL", builtin_equal, 2, 2);

    define_builtin("VALUES", builtin_values, 0, -1);
    define_builtin("ERROR", builtin_error, 1, -1);

    define_builtin("MAKE-HASH-TABLE", builtin_make_hash_table, 0, -1);
    define_builtin("GETHASH", builtin_gethash, 2, 3);
    define_builtin("REMHASH", builtin_remhash, 2, 2);
    define_builtin("CLRHASH", builtin_clrhash, 1, 1);
    define_builtin("HASH-TABLE-COUNT", builtin_hash_count, 1, 1);
    define_builtin("MAPHASH", builtin_maphash, 2, 2);

    define_internal("%KEEP", builtin_keep, 1, 1);
    define_internal("%RELEASE", builtin_release, 1, 1);
    define_internal("%CONDITION-MESSAGE", builtin_condition_message, 1, 1);
    define_internal("%WITH-LIMITS", builtin_with_limits, 3, 3);
    define_internal("%LOAD-SOURCE", builtin_load_source, 3, 3);
    define_internal("%DROP-PACKAGE", builtin_drop_package, 1, 1);
    define_internal("%EXPORT", builtin_export, 1, 1);
    define_internal("%READ-SOURCE", builtin_read_source, 2, 2);
    define_internal("%SYMBOL-DEFINITION", builtin_symbol_definition, 1, 1);
    define_internal("%SET-SYMBOL-DEFINITION", builtin_set_symbol_definition, 2, 2);
    define_internal("%CATCH", builtin_catch, 2, 2);
    define_internal("%SYMBOL-VALUE", builtin_symbol_value_cell, 1, 1);
    define_internal("%SET-SYMBOL-VALUE", builtin_set_symbol_value_cell, 2, 2);
    define_internal("%UNBIND-SYMBOL", builtin_unbind_symbol, 1, 1);

    lisp_register_special("QUOTE", SF_QUOTE);
    lisp_register_special("FUNCTION", SF_FUNCTION);
    lisp_register_special("IF", SF_IF);
    lisp_register_special("PROGN", SF_PROGN);
    lisp_register_special("LET", SF_LET);
    lisp_register_special("LET*", SF_LET_STAR);
    lisp_register_special("SETQ", SF_SETQ);
    lisp_register_special("FLET", SF_FLET);
    lisp_register_special("LABELS", SF_LABELS);
    lisp_register_special("LAMBDA", SF_LAMBDA);
    lisp_register_special("BLOCK", SF_BLOCK);
    lisp_register_special("RETURN-FROM", SF_RETURN_FROM);
    lisp_register_special("RETURN", SF_RETURN);
    lisp_register_special("MULTIPLE-VALUE-BIND", SF_MULTIPLE_VALUE_BIND);
    lisp_register_special("MULTIPLE-VALUE-SETQ", SF_MULTIPLE_VALUE_SETQ);
    lisp_register_special("DECLARE", SF_DECLARE);
    lisp_register_special("DECLAIM", SF_DECLAIM);
    lisp_register_special("DEFUN", SF_DEFUN);
    lisp_register_special("DEFPARAMETER", SF_DEFPARAMETER);
    lisp_register_special("DEFVAR", SF_DEFVAR);
    lisp_register_special("DEFMACRO", SF_DEFMACRO);
    lisp_register_special("IN-PACKAGE", SF_IN_PACKAGE);
    lisp_register_special("AND", SF_AND);
    lisp_register_special("OR", SF_OR);
    lisp_register_special("WHEN", SF_WHEN);
    lisp_register_special("UNLESS", SF_UNLESS);
    lisp_register_special("COND", SF_COND);
    lisp_register_special("CASE", SF_CASE);
    lisp_register_special("ECASE", SF_ECASE);
    lisp_register_special("DOLIST", SF_DOLIST);
    lisp_register_special("DOTIMES", SF_DOTIMES);
    lisp_register_special("DESTRUCTURING-BIND", SF_DESTRUCTURING_BIND);
    lisp_register_special("IGNORE-ERRORS", SF_IGNORE_ERRORS);
    lisp_register_special("HANDLER-CASE", SF_HANDLER_CASE);
    lisp_register_special("UNWIND-PROTECT", SF_UNWIND_PROTECT);
    lisp_register_special("SETF", SF_SETF);
    lisp_register_special("PUSH", SF_PUSH);
    lisp_register_special("PUSHNEW", SF_PUSHNEW);
    lisp_register_special("POP", SF_POP);
    lisp_register_special("INCF", SF_INCF);
    lisp_register_special("DECF", SF_DECF);
    lisp_register_special("ROTATEF", SF_ROTATEF);
    lisp_register_special("LOOP", SF_LOOP);
    lisp_register_special("DEFINE-EXTENSION", SF_DEFINE_EXTENSION);
    lisp_register_special("EVAL", SF_EVAL);
    lisp_register_special("QUASIQUOTE", SF_QUASIQUOTE);
    lisp_register_special("MULTIPLE-VALUE-LIST", SF_MULTIPLE_VALUE_LIST);
    lisp_register_special("PROG1", SF_PROG1);
    lisp_register_special("CHECK-TYPE", SF_CHECK_TYPE);
    return 0;
}

int lisp_init(size_t region_bytes)
{
    if (lisp.initialised)
        return 0;
    memset(&lisp, 0, sizeof(lisp));
    void *reservation = mmap(NULL, LISP_RESERVE_BYTES, PROT_NONE,
                             MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (reservation == MAP_FAILED)
        return -1;
    lisp.blocks = calloc(LISP_MAX_BLOCKS, sizeof(struct lisp_block));
    lisp.intern_table = intern_table;
    lisp.intern_cap = LISP_MAX_SYMBOLS * 2;
    lisp.name_pool = name_pool;
    lisp.region_limit = region_bytes ? region_bytes : (size_t)LISP_BLOCK_SIZE * 16;
    if (lisp.region_limit > LISP_RESERVE_BYTES)
        lisp.region_limit = LISP_RESERVE_BYTES;
    memset(intern_table, 0, sizeof(intern_table));
    memset(name_pool, 0, sizeof(name_pool));
    for (uint32_t i = 0; i < LISP_MAX_BLOCKS; i++) {
        memset(&lisp.blocks[i], 0, sizeof(lisp.blocks[i]));
        lisp.blocks[i].next = i + 1 < LISP_MAX_BLOCKS ? i + 1 : UINT32_MAX;
    }
    heap_set_reservation(reservation);
    heap_reset_blocks();
    init_region(&lisp.arena, REG_ARENA, "<arena>");
    init_region(&lisp.system, REG_GENERATION, "<system>");
    lisp.cur_region = &lisp.arena;
    for (uint32_t i = 0; i < LISP_MAX_REGIONS; i++)
        memset(&lisp.regions[i], 0, sizeof(lisp.regions[i]));
    struct lisp_package *packages = lisp.packages;
    memset(packages, 0, sizeof(lisp.packages));
    const char *names[4] = { "SYSTEM", "KEYWORD", "TOMOE", "TOMOE-USER" };
    for (uint32_t i = 0; i < 4; i++) {
        packages[i].used = true;
        snprintf(packages[i].name, sizeof(packages[i].name), "%s", names[i]);
        packages[i].nickname[0] = '\0';
        packages[i].symbols = NIL_VAL;
        packages[i].export_count = 0;
    }
    lisp.cur_package = PKG_TOMOE;
    lisp.rng_state = (uint64_t)time(NULL) ^ ((uint64_t)getpid() << 32) ^ 0x9e3779b97f4a7c15ull;
    if (lisp.rng_state == 0)
        lisp.rng_state = 1;
    lisp.cur_region = &lisp.system;
    lisp_init_small_ints();
    init_symbols();
    init_primitives();
    define_constant("PI", make_double(3.14159265358979323846));
    define_constant("INTERNAL-TIME-UNITS-PER-SECOND", make_int(1000000));
    define_constant("MOST-POSITIVE-FIXNUM", make_int(INT64_C(4611686018427387903)));
    lisp.oom_condition = make_condition_obj("lisp heap limit reached");
    lisp.cur_region = &lisp.arena;
    lisp.initialised = true;
    lisp.mv_count = 0;
    lisp.ctl_top = 0;
    lisp.cache_reservation = reservation;
    return 0;
}

void lisp_shutdown(void)
{
    if (!lisp.initialised)
        return;
    if (lisp.cache_reservation) {
        munmap(lisp.cache_reservation, LISP_RESERVE_BYTES);
        lisp.cache_reservation = NULL;
    }
    free(lisp.blocks);
    lisp.blocks = NULL;
    lisp.initialised = false;
}

int lisp_set_region_limit(size_t region_bytes)
{
    if (!lisp.initialised)
        return -1;
    if (region_bytes < lisp.heap_used || region_bytes > LISP_RESERVE_BYTES)
        return -1;
    lisp.region_limit = region_bytes;
    return 0;
}

void lisp_heap_stats(struct lisp_heap_stats *out)
{
    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    out->region_bytes = lisp.region_limit;
    out->persistent_used = lisp.persistent_used;
    out->persistent_high_water = lisp.persistent_high_water;
    out->arena_used = lisp.arena.used;
    out->arena_high_water = lisp.arena.high_water;
    uint64_t generations = 0;
    for (struct lisp_region *gen = lisp.generations; gen; gen = gen->next)
        generations++;
    out->generations = generations;
    out->reclaim_count = lisp.reclaim_count;
    out->reclaim_longest_us = lisp.reclaim_longest_us;
    out->steps_last = lisp.steps_total_last;
    out->budget_failures = lisp.budget_failures;
}

lisp_value lisp_nil(void) { return NIL_VAL; }
lisp_value lisp_t(void) { return T_VAL; }
lisp_value lisp_bool(bool value) { return value ? T_VAL : NIL_VAL; }
lisp_value lisp_int(int64_t value) { return make_int(value); }

lisp_value lisp_uint(uint64_t value)
{
    if (value <= (uint64_t)INT64_MAX)
        return make_int((int64_t)value);
    return make_double((double)value);
}

lisp_value lisp_double(double value) { return make_double(value); }
lisp_value lisp_string(const char *text, size_t length) { return make_string_len(text, length); }
lisp_value lisp_cstring(const char *text) { return make_string_len(text, strlen(text)); }

lisp_value lisp_keyword(const char *name)
{
    if (name[0] == ':')
        name++;
    size_t len = strlen(name);
    char buffer[256];
    if (len >= sizeof(buffer))
        len = sizeof(buffer) - 1;
    for (size_t i = 0; i < len; i++)
        buffer[i] = (char)toupper((unsigned char)name[i]);
    buffer[len] = '\0';
    struct lisp_symbol *sym = symbol_intern(PKG_KEYWORD, buffer, len, false);
    sym->h.flags |= SYM_KEYWORD | SYM_CONSTANT;
    sym->value = VAL(sym);
    return VAL(sym);
}

lisp_value lisp_cons(lisp_value car, lisp_value cdr) { return cons(car, cdr); }

lisp_value lisp_list(int count, ...)
{
    lisp_value items[64];
    int n = count > 64 ? 64 : count;
    va_list ap;
    va_start(ap, count);
    for (int i = 0; i < n; i++)
        items[i] = va_arg(ap, lisp_value);
    va_end(ap);
    return list_from_array(n, items);
}

lisp_value lisp_nreverse(lisp_value list) { return builtin_nreverse(1, &list); }

bool lisp_nilp(lisp_value value) { return nilp(value); }
bool lisp_consp(lisp_value value) { return consp(value); }
bool lisp_intp(lisp_value value) { return intp(value); }
bool lisp_numberp(lisp_value value) { return numberp(value); }
bool lisp_stringp(lisp_value value) { return stringp(value); }
bool lisp_keywordp(lisp_value value) { return symbol_is_keyword(value); }
bool lisp_symbolp(lisp_value value) { return symbolp(value); }
bool lisp_functionp(lisp_value value) { return functionp(value); }

lisp_value lisp_car(lisp_value value) { return nilp(value) ? NIL_VAL : consp(value) ? CAR(value) : NIL_VAL; }
lisp_value lisp_cdr(lisp_value value) { return nilp(value) ? NIL_VAL : consp(value) ? CDR(value) : NIL_VAL; }

int64_t lisp_get_int(lisp_value value)
{
    if (intp(value))
        return INTVAL(value);
    if (doublep(value))
        return (int64_t)DBLVAL(value);
    if (ratiop(value))
        return PTR(value, lisp_ratio)->num / PTR(value, lisp_ratio)->den;
    return 0;
}

uint64_t lisp_get_uint(lisp_value value)
{
    if (intp(value))
        return (uint64_t)INTVAL(value);
    if (doublep(value))
        return (uint64_t)DBLVAL(value);
    return 0;
}

double lisp_get_double(lisp_value value) { return numberp(value) ? number_to_double(value) : 0.0; }

const char *lisp_get_string(lisp_value value, size_t *length)
{
    if (!stringp(value))
        return NULL;
    if (length)
        *length = PTR(value, lisp_string)->byte_len;
    return PTR(value, lisp_string)->data;
}

const char *lisp_symbol_name(lisp_value value)
{
    if (!symbolp(value))
        return "";
    return SYM(value)->name;
}

lisp_value lisp_getf(lisp_value plist, const char *keyword)
{
    return keyword_get(plist, lisp_keyword(keyword), NIL_VAL);
}

bool lisp_eq_keyword(lisp_value value, const char *keyword)
{
    if (!symbolp(value) || !(SYM(value)->h.flags & SYM_KEYWORD))
        return false;
    if (keyword[0] == ':')
        keyword++;
    size_t len = strlen(keyword);
    if (len != SYM(value)->name_len)
        return false;
    for (size_t i = 0; i < len; i++)
        if (toupper((unsigned char)keyword[i]) != (unsigned char)SYM(value)->name[i])
            return false;
    return true;
}

size_t lisp_print(lisp_value value, bool readably, char *buffer, size_t size)
{
    struct lisp_buf buf;
    buf_init(&buf, buffer, size);
    print_value(value, readably, &buf);
    return buf.len;
}

void lisp_define_primitive(const char *name, lisp_primitive fn, int min_args, int max_args)
{
    lisp_register_builtin(name, fn, min_args, max_args);
}
