#ifndef TOMOE_LISP_INTERNAL_H
#define TOMOE_LISP_INTERNAL_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>

#include "../lisp.h"

enum lisp_type {
    LT_SYMBOL = 1,
    LT_CONS,
    LT_INT,
    LT_RATIO,
    LT_DOUBLE,
    LT_CHAR,
    LT_STRING,
    LT_FUNCTION,
    LT_FRAME,
    LT_HASHTABLE,
    LT_CONDITION
};

enum lisp_fn_kind {
    FN_BUILTIN = 1,
    FN_CLOSURE
};

enum lisp_region_kind {
    REG_ARENA = 1,
    REG_GENERATION,
    REG_PERSISTENT
};

enum lisp_package_id {
    PKG_SYSTEM = 0,
    PKG_KEYWORD = 1,
    PKG_TOMOE = 2,
    PKG_TOMOE_USER = 3
};

enum lisp_symbol_flags {
    SYM_KEYWORD = 1u << 0,
    SYM_SPECIAL = 1u << 1,
    SYM_MACRO = 1u << 2,
    SYM_VISIBLE = 1u << 3,
    SYM_UNINTERNED = 1u << 4,
    SYM_CONSTANT = 1u << 5
};

enum lisp_binding_kind {
    BIND_VAR = 0,
    BIND_FUNC = 1
};

enum lisp_special_id {
    SF_NONE = 0,
    SF_QUOTE,
    SF_FUNCTION,
    SF_IF,
    SF_PROGN,
    SF_LET,
    SF_LET_STAR,
    SF_SETQ,
    SF_FLET,
    SF_LABELS,
    SF_LAMBDA,
    SF_BLOCK,
    SF_RETURN_FROM,
    SF_RETURN,
    SF_MULTIPLE_VALUE_BIND,
    SF_MULTIPLE_VALUE_SETQ,
    SF_DECLARE,
    SF_DECLAIM,
    SF_DEFUN,
    SF_DEFPARAMETER,
    SF_DEFVAR,
    SF_DEFMACRO,
    SF_IN_PACKAGE,
    SF_AND,
    SF_OR,
    SF_WHEN,
    SF_UNLESS,
    SF_COND,
    SF_CASE,
    SF_ECASE,
    SF_DOLIST,
    SF_DOTIMES,
    SF_DESTRUCTURING_BIND,
    SF_IGNORE_ERRORS,
    SF_HANDLER_CASE,
    SF_UNWIND_PROTECT,
    SF_SETF,
    SF_PUSH,
    SF_PUSHNEW,
    SF_POP,
    SF_INCF,
    SF_DECF,
    SF_ROTATEF,
    SF_LOOP,
    SF_DEFINE_EXTENSION,
    SF_QUASIQUOTE,
    SF_EVAL,
    SF_PROG1,
    SF_CHECK_TYPE,
    SF_MULTIPLE_VALUE_LIST
};

enum lisp_ctl_kind {
    CTL_TOP = 1,
    CTL_CATCH,
    CTL_BLOCK,
    CTL_CLEANUP,
    CTL_DYNBIND
};

#define LISP_MAX_SYMBOLS 32768u
#define LISP_MAX_PACKAGES 64u
#define LISP_NAME_POOL (1u << 20)
#define LISP_MAX_BLOCKS 1024u
#define LISP_BLOCK_SIZE (1u << 20)
#define LISP_MAX_REGIONS 256u
#define LISP_RESERVE_BYTES ((size_t)LISP_MAX_BLOCKS * LISP_BLOCK_SIZE)
#define LISP_MAX_CTL 4096
#define LISP_MAX_MV 16
#define LISP_MAX_DEPTH 512
#define LISP_MAX_DYN 4096

struct lisp_obj {
    uint8_t type;
    uint8_t flags;
    uint16_t aux;
    uint32_t size;
};

struct lisp_symbol {
    struct lisp_obj h;
    uint32_t package;
    uint32_t name_len;
    uint32_t hash;
    uint32_t special_id;
    char *name;
    lisp_value value;
    lisp_value function;
};

struct lisp_cons {
    struct lisp_obj h;
    lisp_value car;
    lisp_value cdr;
};

struct lisp_int {
    struct lisp_obj h;
    int64_t value;
};

struct lisp_ratio {
    struct lisp_obj h;
    int64_t num;
    int64_t den;
};

struct lisp_double {
    struct lisp_obj h;
    double value;
};

struct lisp_char {
    struct lisp_obj h;
    uint32_t code;
};

struct lisp_string {
    struct lisp_obj h;
    uint32_t len;
    uint32_t byte_len;
    char data[];
};

struct lisp_frame;

struct lisp_generation;

struct lisp_function {
    struct lisp_obj h;
    uint8_t kind;
    int16_t min_args;
    int16_t max_args;
    lisp_primitive fn;
    uint32_t name_len;
    const char *name;
    lisp_value self_name;
    lisp_value params;
    lisp_value body;
    struct lisp_frame *env;
    struct lisp_region *gen;
};

struct lisp_binding {
    lisp_value sym;
    lisp_value value;
    uint8_t kind;
};

struct lisp_frame {
    struct lisp_obj h;
    struct lisp_frame *parent;
    uint32_t count;
    struct lisp_binding entry[];
};

struct lisp_hashtable {
    struct lisp_obj h;
    uint32_t count;
    lisp_value test;
    lisp_value entries;
};

struct lisp_condition {
    struct lisp_obj h;
    lisp_value class;
    lisp_value message;
};

struct lisp_package {
    bool used;
    char name[48];
    char nickname[48];
    uint32_t export_count;
    lisp_value symbols;
};

struct lisp_region;

struct lisp_block {
    uint32_t used;
    uint32_t next;
    uint8_t in_use;
    uint8_t kind;
    uint8_t mapped;
    uint8_t pad;
    struct lisp_region *owner;
};

struct lisp_region {
    int kind;
    bool in_use;
    bool dropped;
    uint32_t pin_count;
    uint32_t first;
    uint32_t last;
    uint64_t used;
    uint64_t high_water;
    char name[64];
    uint32_t package;
    struct lisp_region *next;
};

struct lisp_ctl {
    uint8_t kind;
    jmp_buf jb;
    lisp_value name;
    lisp_value form;
    struct lisp_frame *env;
    struct lisp_symbol *sym;
    lisp_value old;
};

struct lisp_runtime {
    uint64_t arena_peak;
    uint32_t memo_epochs;
    struct lisp_symbol symbols[LISP_MAX_SYMBOLS];
    uint32_t symbol_count;
    uint32_t *intern_table;
    uint32_t intern_cap;
    uint32_t intern_count;
    char *name_pool;
    size_t name_used;
    struct lisp_package packages[LISP_MAX_PACKAGES];
    struct lisp_block *blocks;
    size_t block_count;
    uint64_t region_limit;
    uint64_t heap_used;
    struct lisp_region regions[LISP_MAX_REGIONS];
    struct lisp_region *free_regions;
    struct lisp_region arena;
    struct lisp_region system;
    struct lisp_region *cur_region;
    struct lisp_region *generations;
    uint32_t next_generation_id;
    uint64_t persistent_used;
    uint64_t persistent_high_water;
    uint64_t reclaim_count;
    uint64_t reclaim_longest_us;
    uint64_t steps;
    uint64_t steps_limit;
    uint64_t steps_total_last;
    uint64_t deadline_at;
    uint64_t pending_steps;
    uint64_t pending_deadline_ns;
    uint64_t budget_failures;
    int call_depth;
    int load_depth;
    int analysis_depth;
    int eval_depth;
    uint32_t cur_package;
    struct lisp_ctl ctl[LISP_MAX_CTL];
    int ctl_top;
    lisp_value mv[LISP_MAX_MV];
    int mv_count;
    struct lisp_region *keep_target;
    uint64_t rng_state;
    bool initialised;
    void *cache_reservation;
    lisp_value oom_condition;
    const char *current_hook;

    struct lisp_symbol *sym_nil;
    struct lisp_symbol *sym_t;
    struct lisp_symbol *sym_unbound;
    struct lisp_symbol *sym_quote;
    struct lisp_symbol *sym_function;
    struct lisp_symbol *sym_quasiquote;
    struct lisp_symbol *sym_unquote;
    struct lisp_symbol *sym_unquote_splicing;
    struct lisp_symbol *sym_rest;
    struct lisp_symbol *sym_optional;
    struct lisp_symbol *sym_key;
    struct lisp_symbol *sym_allow_other_keys;
    struct lisp_symbol *sym_aux;
    struct lisp_symbol *sym_body;
    struct lisp_symbol *sym_whole;
    struct lisp_symbol *sym_environment;
    struct lisp_symbol *sym_otherwise;
    struct lisp_symbol *sym_error;
    struct lisp_symbol *sym_keyword_test;
    struct lisp_symbol *sym_keyword_key;
    struct lisp_symbol *sym_keyword_initial_element;
    struct lisp_symbol *sym_keyword_start;
    struct lisp_symbol *sym_keyword_end;
    struct lisp_symbol *sym_keyword_from_end;
    struct lisp_symbol *sym_keyword_radix;
    struct lisp_symbol *sym_keyword_junk_allowed;
    struct lisp_symbol *sym_keyword_start2;
    struct lisp_symbol *sym_keyword_end2;
    struct lisp_symbol *sym_keyword_initial_value;
    struct lisp_symbol *sym_keyword_test_not;
};

extern struct lisp_runtime lisp;

#define OBJ(v) ((struct lisp_obj *)(uintptr_t)(v))
#define TYPE(v) (OBJ(v)->type)
#define PTR(v, t) ((struct t *)(uintptr_t)(v))
#define VAL(p) ((lisp_value)(uintptr_t)(p))

#define NIL_VAL VAL(lisp.sym_nil)
#define T_VAL VAL(lisp.sym_t)

#define CAR(v) (PTR(v, lisp_cons)->car)
#define CDR(v) (PTR(v, lisp_cons)->cdr)

#define LIST1(a) lisp_cons((a), NIL_VAL)
#define LIST2(a, b) lisp_cons((a), lisp_cons((b), NIL_VAL))
#define LIST3(a, b, c) lisp_cons((a), lisp_cons((b), lisp_cons((c), NIL_VAL)))
#define LIST4(a, b, c, d) lisp_cons((a), lisp_cons((b), lisp_cons((c), lisp_cons((d), NIL_VAL))))

static inline bool nilp(lisp_value v) { return v == NIL_VAL; }
static inline bool consp(lisp_value v) { return TYPE(v) == LT_CONS; }
static inline bool symbolp(lisp_value v) { return TYPE(v) == LT_SYMBOL; }
static inline bool intp(lisp_value v) { return TYPE(v) == LT_INT; }
static inline bool doublep(lisp_value v) { return TYPE(v) == LT_DOUBLE; }
static inline bool ratiop(lisp_value v) { return TYPE(v) == LT_RATIO; }
static inline bool numberp(lisp_value v) {
    uint8_t t = TYPE(v);
    return t == LT_INT || t == LT_RATIO || t == LT_DOUBLE;
}
static inline bool stringp(lisp_value v) { return TYPE(v) == LT_STRING; }
static inline bool charp(lisp_value v) { return TYPE(v) == LT_CHAR; }
static inline bool functionp(lisp_value v) { return TYPE(v) == LT_FUNCTION; }
static inline struct lisp_symbol *SYM(lisp_value v) { return PTR(v, lisp_symbol); }
static inline int64_t INTVAL(lisp_value v) { return PTR(v, lisp_int)->value; }
static inline double DBLVAL(lisp_value v) { return PTR(v, lisp_double)->value; }
static inline bool truthy(lisp_value v) { return v != NIL_VAL; }
static inline lisp_value truth(bool b) { return b ? T_VAL : NIL_VAL; }

struct lisp_buf {
    char *data;
    size_t len;
    size_t cap;
    bool owned;
};

void buf_init(struct lisp_buf *buf, char *data, size_t cap);
void buf_putc(struct lisp_buf *buf, char c);
void buf_puts(struct lisp_buf *buf, const char *s);
void buf_putn(struct lisp_buf *buf, const char *s, size_t n);
void buf_printf(struct lisp_buf *buf, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

void *heap_alloc_raw(size_t bytes);
void *heap_alloc_in(struct lisp_region *region, size_t bytes);
void *heap_alloc(size_t bytes);
struct lisp_region *heap_current_region(void);
void heap_set_region(struct lisp_region *region);
void region_reset(struct lisp_region *region);
void region_destroy(struct lisp_region *region);
struct lisp_region *region_new(int kind, const char *name, uint32_t package);
struct lisp_region *persistent_begin(void);
void persistent_end(struct lisp_region *region);
lisp_value heap_deep_copy(lisp_value value, struct lisp_region *dst);
struct lisp_region *heap_region_of(const void *ptr);
uint64_t heap_region_used(struct lisp_region *region);
void heap_arena_reset(void);
void heap_reset_blocks(void);
void heap_set_reservation(void *base);
_Noreturn void lisp_oom(void);
lisp_value lisp_keep(lisp_value value);
void lisp_release(lisp_value value);
lisp_value lisp_frame_alloc(struct lisp_frame *parent, uint32_t count);
void heap_note_reclaim(uint64_t start_ns);
uint64_t lisp_now_ns(void);

struct lisp_symbol *symbol_intern(uint32_t package, const char *name, size_t len, bool uninterned);
struct lisp_symbol *symbol_find(uint32_t package, const char *name, size_t len);
struct lisp_symbol *symbol_intern_c(uint32_t package, const char *name);
lisp_value lisp_intern_token(uint32_t package, const char *name, size_t len, bool keyword, bool uninterned);
bool symbol_is_keyword(lisp_value v);
uint32_t package_find(const char *name);
uint32_t package_find_or_create(const char *name);
void package_drop(uint32_t package);
void symbol_make_visible(struct lisp_symbol *sym);
void symbol_make_special(struct lisp_symbol *sym);
void symbol_defun(struct lisp_symbol *sym, lisp_value fn);
void symbol_set_global(struct lisp_symbol *sym, lisp_value value);
lisp_value symbol_global(struct lisp_symbol *sym);
void lisp_register_builtin(const char *name, lisp_primitive fn, int min_args, int max_args);
void lisp_register_internal(const char *name, lisp_primitive fn, int min_args, int max_args);
void lisp_register_special(const char *name, int id);
uint32_t lisp_current_package(void);
void lisp_set_current_package(uint32_t pkg);

struct lisp_frame *frame_new(struct lisp_frame *parent, uint32_t count);
lisp_value *frame_slot(struct lisp_frame *frame, lisp_value sym, int kind);
struct lisp_frame *frame_parent(struct lisp_frame *frame);
lisp_value make_closure(lisp_value params, lisp_value body, struct lisp_frame *env, lisp_value name);
lisp_value make_builtin(lisp_primitive fn, const char *name, int min_args, int max_args);
lisp_value apply_function(lisp_value fn, int argc, lisp_value *argv);
lisp_value apply_closure(struct lisp_function *fn, int argc, lisp_value *argv);
lisp_value eval(lisp_value form, struct lisp_frame *env);
lisp_value eval_progn(lisp_value body, struct lisp_frame *env);
lisp_value eval_body(lisp_value body, struct lisp_frame *env);
void eval_bind_lambda(struct lisp_frame *frame, lisp_value params, int argc, lisp_value *argv);
void destructure_bind(struct lisp_frame *frame, lisp_value pattern, lisp_value value);
uint32_t pattern_slot_count(lisp_value pattern);
_Noreturn void throw_error_value(lisp_value condition);
_Noreturn void throw_return(lisp_value name, int count, lisp_value *values);
lisp_value make_condition(const char *message);
lisp_value condition_message(lisp_value condition);
void frame_bind(struct lisp_frame *frame, uint32_t index, lisp_value sym, lisp_value value, int kind);
int push_ctl(int kind);
void pop_ctl(int index);
_Noreturn void unwind_to(int index);
void check_steps(void);
const char *lisp_error_message(void);
void lisp_set_limits(uint64_t steps, uint64_t deadline_ns);
void lisp_end_turn(void);
int lisp_init(size_t region_bytes);
int lisp_escape_count(void);
lisp_value lisp_escape_condition(void);
const lisp_value *lisp_escape_values_ptr(void);
void lisp_signal_error(const char *msg);
void lisp_set_error_location(const char *file, int line);
const char *lisp_source_file(void);
int lisp_source_line(void);
void lisp_clear_location(void);
bool lisp_loading(void);
void lisp_enter_call(void);
void lisp_leave_call(void);
lisp_value lisp_analyse_and_eval(lisp_value form, struct lisp_frame *env);
void lisp_analyse_form(lisp_value form, lisp_value file_functions, lisp_value file_variables);

struct kw {
    bool has_test, has_test_not, has_key, has_start, has_end, has_from_end, has_radix, has_junk;
    bool has_start2, has_end2, has_initial_value, has_initial_element, has_count;
    lisp_value test, key, start, end, start2, end2, initial_value, initial_element, count;
    lisp_value from_end, radix, test_not;
    bool junk;
};

void kw_parse(int argc, lisp_value *argv, int base, struct kw *kw);
lisp_value lisp_kw_key(struct kw *kw, lisp_value v);
bool lisp_kw_test(struct kw *kw, lisp_value a, lisp_value b);

struct lisp_actx {
    lisp_value vars[512];
    int var_top;
    lisp_value funcs[512];
    int func_top;
    lisp_value file_funcs;
    lisp_value file_vars;
};

void actx_init(struct lisp_actx *ctx, lisp_value file_funcs, lisp_value file_vars);
void analyse_expr(struct lisp_actx *ctx, lisp_value form);
void analyse_push_var(struct lisp_actx *ctx, lisp_value sym);
void analyse_push_pattern(struct lisp_actx *ctx, lisp_value pattern);
void analyse_push_func(struct lisp_actx *ctx, lisp_value sym);
void loop_analyse(struct lisp_actx *ctx, lisp_value clauses);
lisp_value read_token_symbol(const char *name, size_t len, bool keyword, bool uninterned);

struct lisp_reader {
    const char *text;
    size_t len;
    size_t pos;
    int line;
    const char *file;
    uint32_t package;
    bool allow_backquote;
};

void reader_init(struct lisp_reader *rd, const char *file, const char *text, size_t len, uint32_t package, bool allow_backquote);
bool reader_done(struct lisp_reader *rd);
lisp_value reader_next(struct lisp_reader *rd, bool data_mode, int *line_out);
void reader_error(struct lisp_reader *rd, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

size_t print_value(lisp_value value, bool readably, struct lisp_buf *buf);
lisp_value lisp_format(const char *control, size_t len, int argc, lisp_value *argv);
void print_double(struct lisp_buf *buf, double value);
void print_integer(struct lisp_buf *buf, int64_t value);

lisp_value cons(lisp_value car, lisp_value cdr);
void lisp_init_small_ints(void);
lisp_value make_int(int64_t v);
lisp_value make_ratio(int64_t num, int64_t den);
lisp_value make_double(double v);
lisp_value make_char(uint32_t cp);
lisp_value make_string_len(const char *data, size_t len);
lisp_value make_string_raw(const char *data, size_t bytes, uint32_t cp_len);
lisp_value make_hashtable(lisp_value test);
lisp_value make_condition_obj(const char *message);
bool equal_value(lisp_value a, lisp_value b);
bool hash_table_match(lisp_value table, lisp_value a, lisp_value b);
bool eql_value(lisp_value a, lisp_value b);
bool lisp_is_true(lisp_value v);
int64_t require_int(lisp_value v, const char *what);
lisp_value require_number(lisp_value v, const char *what);
const char *require_string(lisp_value v, size_t *len, const char *what);
uint32_t require_char(lisp_value v, const char *what);
lisp_value bool_value(bool b);
lisp_value list_from_array(int n, lisp_value *items);
int list_length(lisp_value list);
lisp_value list_reverse_copy(lisp_value list);
lisp_value list_append2(lisp_value a, lisp_value b);
lisp_value list_nth(lisp_value list, int64_t n);
lisp_value list_nthcdr(lisp_value list, int64_t n);
lisp_value list_last(lisp_value list);
int64_t list_count(lisp_value list);
void mv_reset(lisp_value primary);
lisp_value mv_first(void);
void mv_return1(lisp_value v);
void mv_return2(lisp_value a, lisp_value b);
lisp_value number_add(lisp_value a, lisp_value b);
lisp_value number_sub(lisp_value a, lisp_value b);
lisp_value number_mul(lisp_value a, lisp_value b);
lisp_value number_div(lisp_value a, lisp_value b);
int number_compare(lisp_value a, lisp_value b, bool *ok);
double number_to_double(lisp_value v);
bool number_finite(lisp_value v);
void number_print(struct lisp_buf *buf, lisp_value v, bool readably);
uint32_t utf8_decode(const char *s, size_t len, size_t *advance);
size_t utf8_encode(uint32_t cp, char *out);
uint32_t utf8_strlen(const char *s, size_t len);
uint32_t string_char_at(lisp_value str, uint32_t index);
lisp_value string_subseq(lisp_value str, uint32_t start, uint32_t end);
lisp_value string_from_range(lisp_value str, uint32_t start, uint32_t end);
bool string_equal_to(lisp_value a, lisp_value b);
lisp_value string_concat(int count, lisp_value *items);
lisp_value builtin_values(int argc, lisp_value *argv);
lisp_value builtin_error(int argc, lisp_value *argv);
lisp_value builtin_funcall(int argc, lisp_value *argv);
lisp_value list_to_array(lisp_value list, int *count_out);
lisp_value keyword_get(lisp_value plist, lisp_value key, lisp_value deflt);
bool plist_lookup(lisp_value plist, const char *name, lisp_value *out);
lisp_value builtin_format(int argc, lisp_value *argv);
lisp_value builtin_apply(int argc, lisp_value *argv);
lisp_value builtin_mapcar(int argc, lisp_value *argv);
lisp_value builtin_remove(int argc, lisp_value *argv);
lisp_value builtin_find(int argc, lisp_value *argv);
lisp_value builtin_position(int argc, lisp_value *argv);
lisp_value builtin_sort(int argc, lisp_value *argv);
lisp_value builtin_make_hash_table(int argc, lisp_value *argv);
lisp_value builtin_gethash(int argc, lisp_value *argv);
lisp_value builtin_parse_integer(int argc, lisp_value *argv);
lisp_value builtin_read_from_string(int argc, lisp_value *argv);
lisp_value builtin_intern(int argc, lisp_value *argv);
lisp_value builtin_coerce(int argc, lisp_value *argv);
lisp_value builtin_typep(int argc, lisp_value *argv);
lisp_value builtin_floor(int argc, lisp_value *argv);
lisp_value builtin_ceiling(int argc, lisp_value *argv);
lisp_value builtin_round(int argc, lisp_value *argv);
lisp_value builtin_truncate(int argc, lisp_value *argv);
lisp_value builtin_values_call(int argc, lisp_value *argv);
lisp_value builtin_char(int argc, lisp_value *argv);
lisp_value builtin_char_eq(int argc, lisp_value *argv);
lisp_value builtin_char_lt(int argc, lisp_value *argv);
lisp_value builtin_char_code(int argc, lisp_value *argv);
lisp_value builtin_code_char(int argc, lisp_value *argv);
lisp_value builtin_string(lisp_value *argv, int argc);
lisp_value builtin_string_eq(int argc, lisp_value *argv);
lisp_value builtin_string_equal(int argc, lisp_value *argv);
lisp_value builtin_string_lt(int argc, lisp_value *argv);
lisp_value builtin_string_gt(int argc, lisp_value *argv);
lisp_value builtin_string_le(int argc, lisp_value *argv);
lisp_value builtin_string_upcase(int argc, lisp_value *argv);
lisp_value builtin_string_downcase(int argc, lisp_value *argv);
lisp_value builtin_string_trim(int argc, lisp_value *argv);
lisp_value builtin_concatenate(int argc, lisp_value *argv);
lisp_value builtin_make_string(int argc, lisp_value *argv);
lisp_value builtin_search(int argc, lisp_value *argv);
lisp_value builtin_subseq(int argc, lisp_value *argv);
lisp_value builtin_map(int argc, lisp_value *argv);
lisp_value builtin_substitute(int argc, lisp_value *argv);
lisp_value builtin_write_to_string(int argc, lisp_value *argv);
lisp_value builtin_prin1_to_string(int argc, lisp_value *argv);
lisp_value builtin_princ_to_string(int argc, lisp_value *argv);
lisp_value builtin_symbol_name(int argc, lisp_value *argv);
lisp_value builtin_gensym(int argc, lisp_value *argv);
lisp_value builtin_eq(int argc, lisp_value *argv);
lisp_value builtin_eql(int argc, lisp_value *argv);
lisp_value builtin_equal(int argc, lisp_value *argv);
lisp_value builtin_listp(int argc, lisp_value *argv);
lisp_value builtin_null(int argc, lisp_value *argv);
lisp_value builtin_not(int argc, lisp_value *argv);
lisp_value builtin_consp(int argc, lisp_value *argv);
lisp_value builtin_stringp(int argc, lisp_value *argv);
lisp_value builtin_integerp(int argc, lisp_value *argv);
lisp_value builtin_realp(int argc, lisp_value *argv);
lisp_value builtin_numberp(int argc, lisp_value *argv);
lisp_value builtin_floatp(int argc, lisp_value *argv);
lisp_value builtin_rationalp(int argc, lisp_value *argv);
lisp_value builtin_characterp(int argc, lisp_value *argv);
lisp_value builtin_symbolp(int argc, lisp_value *argv);
lisp_value builtin_keywordp(int argc, lisp_value *argv);
lisp_value builtin_functionp(int argc, lisp_value *argv);
lisp_value builtin_vectorp(int argc, lisp_value *argv);
lisp_value builtin_fboundp(int argc, lisp_value *argv);
lisp_value builtin_boundp(int argc, lisp_value *argv);
lisp_value builtin_symbol_value(int argc, lisp_value *argv);
lisp_value builtin_symbol_function(int argc, lisp_value *argv);
lisp_value builtin_add(int argc, lisp_value *argv);
lisp_value builtin_sub(int argc, lisp_value *argv);
lisp_value builtin_mul(int argc, lisp_value *argv);
lisp_value builtin_div(int argc, lisp_value *argv);
lisp_value builtin_num_eq(int argc, lisp_value *argv);
lisp_value builtin_num_ne(int argc, lisp_value *argv);
lisp_value builtin_lt(int argc, lisp_value *argv);
lisp_value builtin_gt(int argc, lisp_value *argv);
lisp_value builtin_le(int argc, lisp_value *argv);
lisp_value builtin_ge(int argc, lisp_value *argv);
lisp_value builtin_min(int argc, lisp_value *argv);
lisp_value builtin_max(int argc, lisp_value *argv);
lisp_value builtin_abs(int argc, lisp_value *argv);
lisp_value builtin_mod(int argc, lisp_value *argv);
lisp_value builtin_one_plus(int argc, lisp_value *argv);
lisp_value builtin_one_minus(int argc, lisp_value *argv);
lisp_value builtin_zerop(int argc, lisp_value *argv);
lisp_value builtin_plusp(int argc, lisp_value *argv);
lisp_value builtin_minusp(int argc, lisp_value *argv);
lisp_value builtin_evenp(int argc, lisp_value *argv);
lisp_value builtin_oddp(int argc, lisp_value *argv);
lisp_value builtin_signum(int argc, lisp_value *argv);
lisp_value builtin_logand(int argc, lisp_value *argv);
lisp_value builtin_logior(int argc, lisp_value *argv);
lisp_value builtin_logandc2(int argc, lisp_value *argv);
lisp_value builtin_logxor(int argc, lisp_value *argv);
lisp_value builtin_lognot(int argc, lisp_value *argv);
lisp_value builtin_logtest(int argc, lisp_value *argv);
lisp_value builtin_ash(int argc, lisp_value *argv);
lisp_value builtin_float(int argc, lisp_value *argv);
lisp_value builtin_sqrt(int argc, lisp_value *argv);
lisp_value builtin_sin(int argc, lisp_value *argv);
lisp_value builtin_cos(int argc, lisp_value *argv);
lisp_value builtin_tanh(int argc, lisp_value *argv);
lisp_value builtin_cosh(int argc, lisp_value *argv);
lisp_value builtin_acosh(int argc, lisp_value *argv);
lisp_value builtin_expt(int argc, lisp_value *argv);
lisp_value builtin_random(int argc, lisp_value *argv);
lisp_value builtin_get_internal_time(int argc, lisp_value *argv);
lisp_value builtin_finite_p(int argc, lisp_value *argv);
lisp_value builtin_length(int argc, lisp_value *argv);
lisp_value builtin_char_pred(int argc, lisp_value *argv);
lisp_value builtin_alpha_char_p(int argc, lisp_value *argv);
lisp_value builtin_alphanumericp(int argc, lisp_value *argv);
lisp_value builtin_digit_char_p(int argc, lisp_value *argv);
lisp_value builtin_char_upcase(int argc, lisp_value *argv);
lisp_value builtin_string_prefix_p(int argc, lisp_value *argv);
lisp_value builtin_string_suffix_p(int argc, lisp_value *argv);
lisp_value builtin_keep(int argc, lisp_value *argv);
lisp_value builtin_release(int argc, lisp_value *argv);
lisp_value builtin_condition_message(int argc, lisp_value *argv);
lisp_value builtin_with_limits(int argc, lisp_value *argv);
lisp_value builtin_load_source(int argc, lisp_value *argv);
lisp_value builtin_drop_package(int argc, lisp_value *argv);
lisp_value builtin_export(int argc, lisp_value *argv);
lisp_value builtin_eval(int argc, lisp_value *argv);
lisp_value builtin_apply_fn(int argc, lisp_value *argv);
lisp_value builtin_maphash(int argc, lisp_value *argv);
lisp_value builtin_remhash(int argc, lisp_value *argv);
lisp_value builtin_clrhash(int argc, lisp_value *argv);
lisp_value builtin_hash_count(int argc, lisp_value *argv);
lisp_value builtin_reduce(int argc, lisp_value *argv);
lisp_value builtin_catch(int argc, lisp_value *argv);
lisp_value builtin_read_source(int argc, lisp_value *argv);
lisp_value builtin_symbol_definition(int argc, lisp_value *argv);
lisp_value builtin_set_symbol_definition(int argc, lisp_value *argv);
lisp_value builtin_symbol_value_cell(int argc, lisp_value *argv);
lisp_value builtin_set_symbol_value_cell(int argc, lisp_value *argv);
lisp_value builtin_unbind_symbol(int argc, lisp_value *argv);
lisp_value builtin_identity(int argc, lisp_value *argv);
lisp_value builtin_cons(int argc, lisp_value *argv);
lisp_value builtin_car(int argc, lisp_value *argv);
lisp_value builtin_cdr(int argc, lisp_value *argv);
lisp_value builtin_cddr(int argc, lisp_value *argv);
lisp_value builtin_first(int argc, lisp_value *argv);
lisp_value builtin_second(int argc, lisp_value *argv);
lisp_value builtin_third(int argc, lisp_value *argv);
lisp_value builtin_fourth(int argc, lisp_value *argv);
lisp_value builtin_fifth(int argc, lisp_value *argv);
lisp_value builtin_rest(int argc, lisp_value *argv);
lisp_value builtin_nth(int argc, lisp_value *argv);
lisp_value builtin_nthcdr(int argc, lisp_value *argv);
lisp_value builtin_last(int argc, lisp_value *argv);
lisp_value builtin_list(int argc, lisp_value *argv);
lisp_value builtin_list_star(int argc, lisp_value *argv);
lisp_value builtin_acons(int argc, lisp_value *argv);
lisp_value builtin_append(int argc, lisp_value *argv);
lisp_value builtin_copy_list(int argc, lisp_value *argv);
lisp_value builtin_reverse(int argc, lisp_value *argv);
lisp_value builtin_nreverse(int argc, lisp_value *argv);
lisp_value builtin_make_list(int argc, lisp_value *argv);
lisp_value builtin_getf(int argc, lisp_value *argv);
lisp_value builtin_member(int argc, lisp_value *argv);
lisp_value builtin_assoc(int argc, lisp_value *argv);
lisp_value builtin_some(int argc, lisp_value *argv);
lisp_value builtin_every(int argc, lisp_value *argv);
lisp_value builtin_notany(int argc, lisp_value *argv);
lisp_value builtin_count_if(int argc, lisp_value *argv);
lisp_value builtin_set_difference(int argc, lisp_value *argv);
lisp_value builtin_intersection(int argc, lisp_value *argv);
lisp_value builtin_subsetp(int argc, lisp_value *argv);
lisp_value builtin_adjoin(int argc, lisp_value *argv);
lisp_value builtin_nconc(int argc, lisp_value *argv);
lisp_value builtin_delete(int argc, lisp_value *argv);
lisp_value builtin_delete_if(int argc, lisp_value *argv);
lisp_value builtin_remove_duplicates(int argc, lisp_value *argv);
lisp_value builtin_copy_tree(int argc, lisp_value *argv);
lisp_value builtin_mapc(int argc, lisp_value *argv);
lisp_value builtin_mapcan(int argc, lisp_value *argv);
lisp_value builtin_find_if(int argc, lisp_value *argv);
lisp_value builtin_position_if(int argc, lisp_value *argv);
lisp_value builtin_parse_number(int argc, lisp_value *argv);
lisp_value builtin_string_impl(int argc, lisp_value *argv);
lisp_value builtin_remove_if(int argc, lisp_value *argv);
lisp_value builtin_remove_if_not(int argc, lisp_value *argv);
lisp_value builtin_stable_sort(int argc, lisp_value *argv);
lisp_value builtin_caar(int argc, lisp_value *argv);
lisp_value builtin_cdar(int argc, lisp_value *argv);
lisp_value builtin_sixth(int argc, lisp_value *argv);
lisp_value builtin_copy_seq(int argc, lisp_value *argv);
lisp_value builtin_ldb(int argc, lisp_value *argv);
lisp_value builtin_byte(int argc, lisp_value *argv);
lisp_value builtin_union(int argc, lisp_value *argv);

int init_symbols(void);
int init_primitives(void);
lisp_value loop_eval(lisp_value clauses, struct lisp_frame *env, const char *file);

#endif
