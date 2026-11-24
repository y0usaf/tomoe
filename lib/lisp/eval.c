#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <time.h>
#include <math.h>

#include "lisp-internal.h"

static char g_error[1024];
static const char *g_file;
static int g_line;
static lisp_value escape_condition;
static lisp_value escape_name;
static lisp_value escape_values[LISP_MAX_MV];
static int escape_count;
static bool ctl_ready;

const char *lisp_error_message(void)
{
    return g_error;
}

void lisp_set_error_location(const char *file, int line)
{
    g_file = file;
    g_line = line;
}

void lisp_clear_location(void)
{
    g_file = NULL;
    g_line = 0;
}

const char *lisp_source_file(void)
{
    return g_file ? g_file : "<lisp>";
}

int lisp_source_line(void)
{
    return g_line;
}

bool lisp_loading(void)
{
    return lisp.load_depth > 0;
}

void lisp_enter_call(void)
{
    lisp.call_depth++;
}

void lisp_leave_call(void)
{
    if (lisp.call_depth > 0)
        lisp.call_depth--;
}

lisp_value make_condition(const char *message)
{
    return make_condition_obj(message);
}

lisp_value condition_message(lisp_value condition)
{
    if (TYPE(condition) == LT_CONDITION)
        return PTR(condition, lisp_condition)->message;
    return make_string_len("unknown error", 13);
}

_Noreturn void lisp_oom(void)
{
    if (g_file)
        snprintf(g_error, sizeof(g_error), "%s:%d: lisp heap limit reached", g_file, g_line);
    else
        snprintf(g_error, sizeof(g_error), "lisp heap limit reached");
    uint64_t gen_used = 0;
    for (struct lisp_region *gen = lisp.generations; gen; gen = gen->next)
        gen_used += gen->used;
    fprintf(stderr, "tomoe: %s (hook %s heap %llu generation %llu arena %llu limit %llu)\n",
        g_error, lisp.current_hook ? lisp.current_hook : "-",
        (unsigned long long)lisp.heap_used, (unsigned long long)gen_used,
        (unsigned long long)(lisp.cur_region ? lisp.cur_region->used : 0),
        (unsigned long long)lisp.region_limit);
    lisp.budget_failures++;
    throw_error_value(lisp.oom_condition);
}

_Noreturn void lisp_signal_error(const char *msg)
{
    snprintf(g_error, sizeof(g_error), "%s", msg);
    throw_error_value(make_condition(g_error));
}

_Noreturn void lisp_error(const char *format, ...)
{
    char message[900];
    va_list ap;
    va_start(ap, format);
    vsnprintf(message, sizeof(message), format, ap);
    va_end(ap);
    lisp_value vm_name = lisp_vm_current_name();
    if (symbolp(vm_name) && !nilp(vm_name)) {
        size_t used = strlen(message);
        snprintf(message + used, sizeof(message) - used, " (in %s)", SYM(vm_name)->name);
    } else {
        for (int i = lisp.ctl_top - 1; i >= 0; i--) {
            if (lisp.ctl[i].kind == CTL_BLOCK && symbolp(lisp.ctl[i].name) && !nilp(lisp.ctl[i].name)) {
                size_t used = strlen(message);
                snprintf(message + used, sizeof(message) - used, " (in %s)", SYM(lisp.ctl[i].name)->name);
                break;
            }
        }
    }
    if (g_file)
        snprintf(g_error, sizeof(g_error), "%s:%d: %s", g_file, g_line, message);
    else
        snprintf(g_error, sizeof(g_error), "%s", message);
    throw_error_value(make_condition(g_error));
}

int push_ctl(int kind)
{
    if (lisp.ctl_top >= LISP_MAX_CTL)
        lisp_error("control nesting too deep");
    int index = lisp.ctl_top++;
    if (kind == CTL_TOP)
        ctl_ready = true;
    struct lisp_ctl *entry = &lisp.ctl[index];
    entry->kind = (uint8_t)kind;
    entry->armed = true;
    entry->vm_sp = lisp_vm_sp_get();
    entry->vm_fp = lisp_vm_fp_get();
    entry->name = NIL_VAL;
    entry->sym = NULL;
    return index;
}

void pop_ctl(int index)
{
    lisp.ctl_top = index;
}

static void restore_pending(void)
{
    lisp_value saved_condition = escape_condition;
    lisp_value saved_name = escape_name;
    int saved_count = escape_count;
    lisp_value saved_values[LISP_MAX_MV];
    memcpy(saved_values, escape_values, sizeof(saved_values));
    lisp_value form = lisp.ctl[lisp.ctl_top].form;
    struct lisp_frame *env = lisp.ctl[lisp.ctl_top].env;
    lisp_value result = TYPE(form) == LT_FUNCTION ? apply_fast(form, 0, NULL) : eval_progn(form, env);
    (void)result;
    escape_condition = saved_condition;
    escape_name = saved_name;
    escape_count = saved_count;
    memcpy(escape_values, saved_values, sizeof(saved_values));
}

_Noreturn void unwind_to(int index)
{
    while (lisp.ctl_top > index + 1) {
        int at = lisp.ctl_top - 1;
        struct lisp_ctl *entry = &lisp.ctl[at];
        if (entry->kind == CTL_CLEANUP) {
            lisp.ctl_top = at;
            lisp_vm_sp_set(entry->vm_sp);
            lisp_vm_fp_set(entry->vm_fp);
            restore_pending();
        } else if (entry->kind == CTL_DYNBIND) {
            lisp.ctl_top = at;
            if (entry->sym)
                entry->sym->value = entry->old;
        } else {
            lisp.ctl_top = at;
        }
    }
    lisp.ctl_top = index;
    if (!ctl_ready || index < 0 || index >= LISP_MAX_CTL)
        abort();
    lisp_compile_cleanup();
    lisp_vm_sp_set(lisp.ctl[index].vm_sp);
    lisp_vm_fp_set(lisp.ctl[index].vm_fp);
    longjmp(lisp.ctl[index].jb, 1);
}

_Noreturn void throw_error_value(lisp_value condition)
{
    escape_condition = condition;
    escape_count = 0;
    escape_name = NIL_VAL;
    int target = -1;
    for (int i = lisp.ctl_top - 1; i >= 0; i--) {
        int kind = lisp.ctl[i].kind;
        if (kind == CTL_CATCH || kind == CTL_TOP) {
            target = i;
            break;
        }
    }
    if (target < 0) {
        fprintf(stderr, "tomoe lisp: unhandled error: %s\n", g_error);
        abort();
    }
    unwind_to(target);
}

_Noreturn void throw_return(lisp_value name, int count, lisp_value *values)
{
    escape_name = name;
    escape_count = count > LISP_MAX_MV ? LISP_MAX_MV : count;
    for (int i = 0; i < escape_count; i++)
        escape_values[i] = values[i];
    int target = -1;
    for (int i = lisp.ctl_top - 1; i >= 0; i--) {
        if (lisp.ctl[i].kind == CTL_BLOCK && lisp.ctl[i].armed && eql_value(lisp.ctl[i].name, name)) {
            target = i;
            break;
        }
    }
    if (target < 0)
        lisp_error("RETURN-FROM: no block named %s", symbolp(name) ? SYM(name)->name : "?");
    unwind_to(target);
}

void check_steps_slow(void)
{
    if (lisp.steps_limit && lisp.steps > lisp.steps_limit) {
        lisp.budget_failures++;
        snprintf(g_error, sizeof(g_error), "evaluation budget exceeded");
        throw_error_value(make_condition(g_error));
    }
    if ((lisp.steps & 1023u) == 0 && lisp.deadline_at && lisp_cpu_ns() > lisp.deadline_at) {
        lisp.budget_failures++;
        snprintf(g_error, sizeof(g_error), "evaluation deadline exceeded");
        throw_error_value(make_condition(g_error));
    }
}

void lisp_check_deadline(void)
{
    if (lisp.deadline_at && lisp_cpu_ns() > lisp.deadline_at) {
        lisp.budget_failures++;
        snprintf(g_error, sizeof(g_error), "evaluation deadline exceeded");
        throw_error_value(make_condition(g_error));
    }
}

lisp_value make_builtin(lisp_primitive fn, const char *name, int min_args, int max_args)
{
    struct lisp_function *out = heap_alloc_in(&lisp.system, sizeof(*out));
    memset(out, 0, sizeof(*out));
    out->h.type = LT_FUNCTION;
    out->h.size = sizeof(*out);
    out->kind = FN_BUILTIN;
    out->min_args = (int16_t)min_args;
    out->max_args = (int16_t)max_args;
    out->fn = fn;
    out->name = name;
    out->name_len = name ? (uint32_t)strlen(name) : 0;
    out->self_name = NIL_VAL;
    out->params = NIL_VAL;
    out->body = NIL_VAL;
    out->env = NULL;
    out->gen = NULL;
    return VAL(out);
}

lisp_value make_closure(lisp_value params, lisp_value body, struct lisp_frame *env, lisp_value name)
{
    struct lisp_function *out = heap_alloc(sizeof(*out));
    memset(out, 0, sizeof(*out));
    out->h.type = LT_FUNCTION;
    out->h.size = sizeof(*out);
    out->kind = FN_CLOSURE;
    out->params = params;
    out->body = body;
    out->env = env;
    out->self_name = name;
    out->gen = lisp.cur_region && lisp.cur_region->kind == REG_GENERATION ? lisp.cur_region : NULL;
    return VAL(out);
}

struct lisp_frame *frame_new(struct lisp_frame *parent, uint32_t count)
{
    lisp_value v = lisp_frame_alloc(parent, count);
    return PTR(v, lisp_frame);
}

void frame_bind(struct lisp_frame *frame, uint32_t index, lisp_value sym, lisp_value value, int kind)
{
    if (index >= frame->count)
        lisp_error("internal: frame overflow");
    frame->entry[index].sym = sym;
    frame->entry[index].value = value;
    frame->entry[index].kind = (uint8_t)kind;
    if (TYPE(sym) == LT_SYMBOL) {
        uint8_t flag = kind == BIND_FUNC ? SYM_LOCAL_FUNC : SYM_LOCAL_VAR;
        if (!(SYM(sym)->h.flags & flag))
            SYM(sym)->h.flags |= flag;
    }
}

lisp_value *frame_slot(struct lisp_frame *frame, lisp_value sym, int kind)
{
    if (TYPE(sym) == LT_SYMBOL &&
            !(SYM(sym)->h.flags & (kind == BIND_FUNC ? SYM_LOCAL_FUNC : SYM_LOCAL_VAR)))
        return NULL;
    for (struct lisp_frame *f = frame; f; f = f->parent) {
        for (uint32_t i = f->count; i-- > 0;) {
            if (f->entry[i].sym == sym && f->entry[i].kind == kind)
                return &f->entry[i].value;
        }
    }
    return NULL;
}

struct lisp_frame *frame_parent(struct lisp_frame *frame)
{
    return frame ? frame->parent : NULL;
}

static bool dynamically_bound(struct lisp_symbol *sym)
{
    for (int i = lisp.ctl_top; i-- > 0;)
        if (lisp.ctl[i].kind == CTL_DYNBIND && lisp.ctl[i].sym == sym)
            return true;
    return false;
}

static void check_global_store(lisp_value sym, lisp_value value)
{
    if (lisp_loading() || lisp.call_depth == 0)
        return;
    if (SYM(sym)->package != PKG_TOMOE)
        lisp_error("cannot assign global %s while handling an event; keep what must outlast it in the extension's state", SYM(sym)->name);
    struct lisp_region *region = heap_region_of((void *)(uintptr_t)value);
    if (region && region->kind == REG_ARENA)
        lisp_error("arena value escapes into %s", SYM(sym)->name);
}

static lisp_value *local_var_slot(lisp_value sym, struct lisp_frame *env, bool *found)
{
    if (symbolp(sym) && (SYM(sym)->h.flags & SYM_SPECIAL)) {
        *found = true;
        return &SYM(sym)->value;
    }
    lisp_value *slot = frame_slot(env, sym, BIND_VAR);
    *found = slot != NULL;
    return slot;
}

lisp_value variable_value(lisp_value sym, struct lisp_frame *env)
{
    if (TYPE(sym) != LT_SYMBOL)
        lisp_error("not a variable name");
    bool found = false;
    lisp_value *slot = local_var_slot(sym, env, &found);
    if (found) {
        if (*slot == VAL(lisp.sym_unbound))
            lisp_error("unbound variable %s", SYM(sym)->name);
        return *slot;
    }
    if (SYM(sym) == lisp.sym_t || SYM(sym) == lisp.sym_nil)
        return sym;
    if (SYM(sym)->h.flags & SYM_KEYWORD)
        return sym;
    if (SYM(sym)->value == VAL(lisp.sym_unbound))
        lisp_error("undefined variable %s", SYM(sym)->name);
    return SYM(sym)->value;
}

void set_variable(lisp_value sym, lisp_value value, struct lisp_frame *env)
{
    if (TYPE(sym) != LT_SYMBOL)
        lisp_error("not a variable name");
    if (SYM(sym)->h.flags & (SYM_CONSTANT | SYM_KEYWORD))
        lisp_error("cannot assign to constant %s", SYM(sym)->name);
    if (SYM(sym)->h.flags & SYM_SPECIAL) {
        if (!dynamically_bound(SYM(sym)))
            check_global_store(sym, value);
        SYM(sym)->value = value;
        return;
    }
    lisp_value *slot = frame_slot(env, sym, BIND_VAR);
    if (slot) {
        *slot = heap_store(slot, value);
        return;
    }
    check_global_store(sym, value);
    SYM(sym)->value = value;
}

lisp_value function_value(lisp_value sym, struct lisp_frame *env, bool *found)
{
    lisp_value local = NIL_VAL;
    lisp_value *slot = frame_slot(env, sym, BIND_FUNC);
    if (slot)
        local = *slot;
    if (!nilp(local)) {
        *found = true;
        return local;
    }
    if (TYPE(sym) == LT_SYMBOL && !nilp(SYM(sym)->function)) {
        *found = true;
        return SYM(sym)->function;
    }
    *found = false;
    return NIL_VAL;
}

static lisp_value parameter_keyword(lisp_value var)
{
    static struct { lisp_value var, keyword; } cache[256];
    if (!symbolp(var))
        lisp_error("bad keyword parameter");
    struct lisp_symbol *sym = SYM(var);
    size_t length = strnlen(sym->name, 255);
    size_t slot = ((uintptr_t)var / sizeof(struct lisp_symbol)) & 255;
    if (cache[slot].var == var) {
        struct lisp_symbol *keyword = SYM(cache[slot].keyword);
        if (keyword->package == PKG_KEYWORD && keyword->name_len == length &&
                memcmp(keyword->name, sym->name, length) == 0)
            return cache[slot].keyword;
    }
    lisp_value keyword = VAL(symbol_intern(PKG_KEYWORD, sym->name, length, false));
    cache[slot].var = var;
    cache[slot].keyword = keyword;
    return keyword;
}

void eval_bind_lambda(struct lisp_frame *frame, lisp_value params, int argc, lisp_value *argv)
{
    uint32_t slot = 0;
    int index = 0;
    bool optional = false, key = false, rest = false;
    lisp_value spec = params;
    int key_start = -1;
    while (consp(spec)) {
        lisp_value item = CAR(spec);
        spec = CDR(spec);
        if (symbolp(item)) {
            if (item == VAL(lisp.sym_optional)) {
                optional = true;
                continue;
            }
            if (item == VAL(lisp.sym_key)) {
                key = true;
                key_start = index;
                continue;
            }
            if (item == VAL(lisp.sym_rest) || item == VAL(lisp.sym_body)) {
                rest = true;
                continue;
            }
            if (item == VAL(lisp.sym_allow_other_keys) || item == VAL(lisp.sym_aux) ||
                item == VAL(lisp.sym_whole) || item == VAL(lisp.sym_environment))
                continue;
        }
        if (rest) {
            lisp_value list = NIL_VAL;
            for (int i = argc - 1; i >= index; i--)
                list = cons(argv[i], list);
            frame_bind(frame, slot++, item, list, BIND_VAR);
            rest = false;
            continue;
        }
        if (key) {
            lisp_value var = item, default_form = NIL_VAL, supplied = VAL(lisp.sym_unbound);
            lisp_value keyword = NIL_VAL;
            if (consp(item)) {
                lisp_value head = CAR(item);
                if (consp(head)) {
                    keyword = CAR(head);
                    var = CAR(CDR(head));
                } else {
                    var = head;
                }
                if (consp(CDR(item)))
                    default_form = CAR(CDR(item));
                if (consp(CDR(CDR(item))))
                    supplied = CAR(CDR(CDR(item)));
            }
            if (nilp(keyword))
                keyword = parameter_keyword(var);
            lisp_value value = VAL(lisp.sym_unbound);
            bool supplied_p = false;
            for (int i = key_start; i + 1 < argc; i += 2) {
                if (argv[i] == keyword) {
                    value = argv[i + 1];
                    supplied_p = true;
                    break;
                }
            }
            if (!supplied_p) {
                if (nilp(default_form))
                    value = NIL_VAL;
                else
                    value = eval(default_form, frame);
            }
            frame_bind(frame, slot++, var, value, BIND_VAR);
            if (supplied != VAL(lisp.sym_unbound) && symbolp(supplied))
                frame_bind(frame, slot++, supplied, supplied_p ? T_VAL : NIL_VAL, BIND_VAR);
            continue;
        }
        if (optional) {
            lisp_value var = item, default_form = NIL_VAL, supplied = VAL(lisp.sym_unbound);
            if (consp(item)) {
                var = CAR(item);
                if (consp(CDR(item)))
                    default_form = CAR(CDR(item));
                if (consp(CDR(CDR(item))))
                    supplied = CAR(CDR(CDR(item)));
            }
            bool supplied_p = index < argc;
            lisp_value value;
            if (supplied_p) {
                value = argv[index++];
            } else if (nilp(default_form)) {
                value = NIL_VAL;
            } else {
                value = eval(default_form, frame);
            }
            frame_bind(frame, slot++, var, value, BIND_VAR);
            if (supplied != VAL(lisp.sym_unbound) && symbolp(supplied))
                frame_bind(frame, slot++, supplied, supplied_p ? T_VAL : NIL_VAL, BIND_VAR);
            continue;
        }
        if (index >= argc)
            lisp_error("missing argument %s", symbolp(item) ? SYM(item)->name : "?");
        frame_bind(frame, slot++, item, argv[index++], BIND_VAR);
    }
}

static bool pattern_marker(lisp_value head)
{
    return head == VAL(lisp.sym_rest) || head == VAL(lisp.sym_optional) ||
        head == VAL(lisp.sym_key) || head == VAL(lisp.sym_body) ||
        head == VAL(lisp.sym_allow_other_keys);
}

static uint32_t pattern_slots(lisp_value pattern)
{
    if (nilp(pattern))
        return 0;
    if (symbolp(pattern))
        return 1;
    if (!consp(pattern))
        return 0;
    uint32_t n = 0;
    int mode = 0;
    lisp_value p = pattern;
    while (consp(p)) {
        lisp_value head = CAR(p);
        p = CDR(p);
        if (pattern_marker(head)) {
            if (head == VAL(lisp.sym_optional)) mode = 1;
            else if (head == VAL(lisp.sym_key)) mode = 2;
            continue;
        }
        if (mode && consp(head)) {
            lisp_value var = CAR(head);
            if (mode == 2 && consp(var)) var = CAR(CDR(var));
            n += pattern_slots(var);
            if (consp(CDR(head)) && consp(CDR(CDR(head))))
                n += 1;
        } else {
            n += pattern_slots(head);
        }
    }
    if (!nilp(p))
        n += pattern_slots(p);
    return n;
}

static uint32_t bind_pattern(struct lisp_frame *frame, uint32_t slot, lisp_value pattern, lisp_value value)
{
    if (nilp(pattern))
        return slot;
    if (symbolp(pattern)) {
        frame_bind(frame, slot++, pattern, value, BIND_VAR);
        return slot;
    }
    if (!consp(pattern)) {
        char shown[160], given[160];
        lisp_print(pattern, true, shown, sizeof(shown));
        lisp_print(value, true, given, sizeof(given));
        lisp_error("bad destructuring pattern %s for %s", shown, given);
    }
    int mode = 0;
    lisp_value p = pattern;
    lisp_value item = value;
    lisp_value keys = NIL_VAL;
    while (consp(p)) {
        lisp_value head = CAR(p);
        p = CDR(p);
        if (head == VAL(lisp.sym_rest) || head == VAL(lisp.sym_body)) {
            if (consp(p)) {
                slot = bind_pattern(frame, slot, CAR(p), item);
                p = CDR(p);
            }
            continue;
        }
        if (head == VAL(lisp.sym_optional)) { mode = 1; continue; }
        if (head == VAL(lisp.sym_key)) { mode = 2; keys = item; continue; }
        if (head == VAL(lisp.sym_allow_other_keys)) continue;
        if (mode == 0) {
            if (!consp(item)) {
                slot = bind_pattern(frame, slot, head, NIL_VAL);
                continue;
            }
            slot = bind_pattern(frame, slot, head, CAR(item));
            item = CDR(item);
            continue;
        }
        lisp_value var = head, default_form = NIL_VAL, supplied = NIL_VAL, keyword = NIL_VAL;
        if (consp(head)) {
            var = CAR(head);
            if (consp(CDR(head))) default_form = CAR(CDR(head));
            if (consp(CDR(head)) && consp(CDR(CDR(head)))) supplied = CAR(CDR(CDR(head)));
        }
        bool supplied_p = false;
        lisp_value found = NIL_VAL;
        if (mode == 1) {
            if (consp(item)) {
                found = CAR(item);
                item = CDR(item);
                supplied_p = true;
            }
        } else {
            if (consp(var)) {
                keyword = CAR(var);
                var = CAR(CDR(var));
            } else {
                keyword = VAL(symbol_intern(PKG_KEYWORD, SYM(var)->name, strlen(SYM(var)->name), false));
            }
            for (lisp_value k = keys; consp(k) && consp(CDR(k)); k = CDR(CDR(k)))
                if (CAR(k) == keyword) {
                    found = CAR(CDR(k));
                    supplied_p = true;
                    break;
                }
        }
        if (!supplied_p && !nilp(default_form))
            found = eval(default_form, frame);
        slot = bind_pattern(frame, slot, var, found);
        if (symbolp(supplied) && !nilp(supplied))
            frame_bind(frame, slot++, supplied, supplied_p ? T_VAL : NIL_VAL, BIND_VAR);
    }
    if (!nilp(p))
        slot = bind_pattern(frame, slot, p, item);
    return slot;
}

void destructure_bind(struct lisp_frame *frame, lisp_value pattern, lisp_value value)
{
    bind_pattern(frame, 0, pattern, value);
}

uint32_t pattern_slot_count(lisp_value pattern)
{
    return pattern_slots(pattern);
}

static uint8_t scan_body(lisp_value form, int depth)
{
    uint8_t found = 0;
    while (consp(form) && depth < 256) {
        lisp_value head = CAR(form);
        if (consp(head)) {
            found |= scan_body(head, depth + 1);
        } else if (symbolp(head)) {
            struct lisp_symbol *sym = SYM(head);
            if (sym->h.flags & SYM_MACRO)
                return FNI_ARMED | FNI_CAPTURES;
            switch (sym->special_id) {
            case SF_RETURN:
            case SF_RETURN_FROM:
                found |= FNI_ARMED;
                break;
            case SF_EVAL:
                return FNI_ARMED | FNI_CAPTURES;
            case SF_LAMBDA:
            case SF_FLET:
            case SF_LABELS:
            case SF_DEFUN:
            case SF_DEFMACRO:
            case SF_DEFINE_EXTENSION:
                found |= FNI_CAPTURES;
                break;
            default:
                break;
            }
        }
        form = CDR(form);
    }
    if (depth >= 256)
        return FNI_ARMED | FNI_CAPTURES;
    return found;
}

void lisp_closure_info(struct lisp_function *fn)
{
    uint32_t count = 0;
    uint8_t info = FNI_KNOWN | FNI_SIMPLE;
    lisp_value scan = fn->params;
    while (consp(scan)) {
        lisp_value item = CAR(scan);
        scan = CDR(scan);
        if (item == VAL(lisp.sym_optional) || item == VAL(lisp.sym_key) ||
            item == VAL(lisp.sym_aux) || item == VAL(lisp.sym_allow_other_keys) ||
            item == VAL(lisp.sym_whole) || item == VAL(lisp.sym_environment) ||
            item == VAL(lisp.sym_rest) || item == VAL(lisp.sym_body)) {
            info &= (uint8_t)~FNI_SIMPLE;
            continue;
        }
        count++;
        if (consp(item)) {
            info &= (uint8_t)~FNI_SIMPLE;
            count += (uint32_t)(list_length(CDR(item)));
        } else if (!symbolp(item)) {
            info &= (uint8_t)~FNI_SIMPLE;
        }
    }
    if (!nilp(scan))
        info &= (uint8_t)~FNI_SIMPLE;
    info |= scan_body(fn->params, 0) | scan_body(fn->body, 0);
    fn->slots = count;
    fn->info = info;
}


__attribute__((noinline)) lisp_value apply_closure(struct lisp_function *fn, int argc, lisp_value *argv)
{
    if (!(fn->info & FNI_KNOWN))
        lisp_closure_info(fn);
    if (!(fn->info & FNI_COMPILED) && fn->gen && ++fn->calls >= 2)
        lisp_compile_closure(fn);
    if (fn->code && (!lisp.builtin_redefined || fn->upval_count > 0))
        return lisp_vm_apply(fn, argc, argv);
    uint32_t count = fn->slots;
    uint8_t info = fn->info;
    struct lisp_frame *frame;
    if (info & FNI_CAPTURES) {
        frame = frame_new(fn->env, count);
    } else {
        frame = __builtin_alloca(offsetof(struct lisp_frame, entry) + (size_t)count * sizeof(struct lisp_binding));
        frame->h.type = LT_FRAME;
        frame->h.flags = 0;
        frame->h.aux = 0;
        frame->h.size = 0;
        frame->parent = fn->env;
        frame->count = count;
        if (!(info & FNI_SIMPLE)) {
            for (uint32_t i = 0; i < count; i++) {
                frame->entry[i].sym = NIL_VAL;
                frame->entry[i].value = VAL(lisp.sym_unbound);
                frame->entry[i].kind = BIND_VAR;
            }
        }
    }
    if (info & FNI_SIMPLE) {
        if (argc < (int)count) {
            lisp_value missing = list_nth(fn->params, argc);
            lisp_error("missing argument %s", SYM(missing)->name);
        }
        lisp_value scan = fn->params;
        for (uint32_t i = 0; i < count; i++, scan = CDR(scan)) {
            lisp_value sym = CAR(scan);
            if (!(SYM(sym)->h.flags & SYM_LOCAL_VAR))
                SYM(sym)->h.flags |= SYM_LOCAL_VAR;
            frame->entry[i].sym = sym;
            frame->entry[i].value = argv[i];
            frame->entry[i].kind = BIND_VAR;
        }
    } else {
        eval_bind_lambda(frame, fn->params, argc, argv);
    }
    int ctl = push_ctl(CTL_BLOCK);
    lisp.ctl[ctl].name = fn->self_name;
    if (!(fn->info & FNI_ARMED)) {
        lisp.ctl[ctl].armed = false;
        lisp_value result = eval_progn(fn->body, frame);
        lisp.ctl_top = ctl;
        return result;
    }
    if (setjmp(lisp.ctl[ctl].jb) != 0) {
        lisp.ctl_top = ctl;
        int n = escape_count > 0 ? escape_count : 1;
        lisp.mv_count = n;
        for (int i = 0; i < n; i++)
            lisp.mv[i] = i < escape_count ? escape_values[i] : NIL_VAL;
        return lisp.mv[0];
    }
    lisp_value result = eval_progn(fn->body, frame);
    lisp.ctl_top = ctl;
    return result;
}

lisp_value apply_function(lisp_value fn, int argc, lisp_value *argv)
{
    if (symbolp(fn)) {
        bool found = false;
        lisp_value resolved = function_value(fn, NULL, &found);
        if (!found)
            lisp_error("undefined function %s", SYM(fn)->name);
        fn = resolved;
    }
    if (TYPE(fn) != LT_FUNCTION)
        lisp_error("not a function");
    struct lisp_function *f = PTR(fn, lisp_function);
    check_steps();
    if (f->kind == FN_BUILTIN) {
        if (argc < f->min_args || (f->max_args >= 0 && argc > f->max_args))
            lisp_error("%s: wrong number of arguments (%d)", f->name ? f->name : "function", argc);
        lisp.mv_count = 0;
        lisp_value result = f->fn(argc, argv);
        if (lisp.mv_count == 0 || lisp.mv[0] != result)
            mv_return1(result);
        return lisp.mv[0];
    }
    return apply_closure(f, argc, argv);
}

lisp_value eval_progn(lisp_value body, struct lisp_frame *env)
{
    lisp_value result = NIL_VAL;
    bool any = false;
    while (consp(body)) {
        result = eval(CAR(body), env);
        any = true;
        body = CDR(body);
    }
    if (!any)
        return NIL_VAL;
    return result;
}

lisp_value eval_body(lisp_value body, struct lisp_frame *env)
{
    return eval_progn(body, env);
}

enum { CALL_STACK_ARGS = 8 };

static lisp_value eval_call(lisp_value fn, lisp_value args, struct lisp_frame *env)
{
    int n = 0;
    for (lisp_value scan = args; consp(scan); scan = CDR(scan))
        n++;
    lisp_value stack[CALL_STACK_ARGS];
    lisp_value *argv = n <= CALL_STACK_ARGS ? stack : heap_alloc(sizeof(lisp_value) * (size_t)n);
    int i = 0;
    while (consp(args)) {
        argv[i++] = eval(CAR(args), env);
        args = CDR(args);
    }
    return apply_fast(fn, i, argv);
}


static lisp_value eval_let(lisp_value form, struct lisp_frame *env, bool star)
{
    lisp_value bindings = CAR(CDR(form));
    lisp_value body = CDR(CDR(form));
    int count = list_length(bindings);
    int ctl_start = lisp.ctl_top;
    struct lisp_frame *frame = frame_new(env, (uint32_t)count + 1);
    int slot = 0;
    lisp_value b = bindings;
    lisp_value dyn_marks[64];
    int dyn_count = 0;
    while (consp(b)) {
        lisp_value binding = CAR(b);
        b = CDR(b);
        lisp_value name = binding;
        lisp_value init = NIL_VAL;
        if (consp(binding)) {
            name = CAR(binding);
            if (consp(CDR(binding)))
                init = CAR(CDR(binding));
        }
        lisp_value value = eval(init, star ? frame : env);
        if (symbolp(name) && (SYM(name)->h.flags & SYM_SPECIAL)) {
            int ctl = push_ctl(CTL_DYNBIND);
            lisp.ctl[ctl].sym = SYM(name);
            lisp.ctl[ctl].old = SYM(name)->value;
            SYM(name)->value = value;
            (void)dyn_marks;
            (void)dyn_count;
        } else {
            frame_bind(frame, (uint32_t)slot++, name, value, BIND_VAR);
        }
    }
    lisp_value result = eval_progn(body, frame);
    restore_dynamic_to(ctl_start);
    return result;
}

lisp_value eval_unwind_thunk(lisp_value body_fn, lisp_value cleanup_fn)
{
    int ctl = push_ctl(CTL_CLEANUP);
    lisp.ctl[ctl].form = cleanup_fn;
    lisp.ctl[ctl].env = NULL;
    lisp_value result = apply_fast(body_fn, 0, NULL);
    lisp.ctl_top = ctl;
    int count = lisp.mv_count;
    lisp_value saved[LISP_MAX_MV];
    for (int i = 0; i < count && i < LISP_MAX_MV; i++)
        saved[i] = lisp.mv[i];
    apply_fast(cleanup_fn, 0, NULL);
    lisp.mv_count = count;
    for (int i = 0; i < count && i < LISP_MAX_MV; i++)
        lisp.mv[i] = saved[i];
    return result;
}

lisp_value eval_ignore_errors_thunk(lisp_value body_fn)
{
    int ctl = push_ctl(CTL_CATCH);
    if (setjmp(lisp.ctl[ctl].jb) != 0) {
        lisp.ctl_top = ctl;
        lisp_value condition = escape_condition;
        mv_return2(NIL_VAL, condition);
        return NIL_VAL;
    }
    lisp_value result = apply_fast(body_fn, 0, NULL);
    lisp.ctl_top = ctl;
    return result;
}

lisp_value eval_handler_case_thunk(lisp_value body_fn, lisp_value handler_fn)
{
    int ctl = push_ctl(CTL_CATCH);
    if (setjmp(lisp.ctl[ctl].jb) != 0) {
        lisp.ctl_top = ctl;
        lisp_value condition = escape_condition;
        if (!nilp(handler_fn))
            return apply_fast(handler_fn, 1, &condition);
        throw_error_value(condition);
    }
    lisp_value result = apply_fast(body_fn, 0, NULL);
    lisp.ctl_top = ctl;
    return result;
}

lisp_value eval_block_thunk(lisp_value name, lisp_value body_fn)
{
    int ctl = push_ctl(CTL_BLOCK);
    lisp.ctl[ctl].name = name;
    if (setjmp(lisp.ctl[ctl].jb) != 0) {
        lisp.ctl_top = ctl;
        int count = escape_count > 0 ? escape_count : 1;
        lisp.mv_count = count;
        for (int i = 0; i < count; i++)
            lisp.mv[i] = i < escape_count ? escape_values[i] : NIL_VAL;
        return lisp.mv[0];
    }
    lisp_value result = apply_fast(body_fn, 0, NULL);
    lisp.ctl_top = ctl;
    return result;
}

void restore_dynamic_to(int ctl_index)
{
    while (lisp.ctl_top > ctl_index) {
        int at = lisp.ctl_top - 1;
        struct lisp_ctl *entry = &lisp.ctl[at];
        if (entry->kind == CTL_DYNBIND && entry->sym)
            entry->sym->value = entry->old;
        lisp.ctl_top = at;
    }
}

static lisp_value eval_flet(lisp_value form, struct lisp_frame *env, bool recursive)
{
    lisp_value bindings = CAR(CDR(form));
    lisp_value body = CDR(CDR(form));
    int count = list_length(bindings);
    struct lisp_frame *frame = frame_new(env, (uint32_t)count + 1);
    int slot = 0;
    lisp_value b = bindings;
    while (consp(b)) {
        lisp_value binding = CAR(b);
        b = CDR(b);
        lisp_value name = CAR(binding);
        lisp_value params = CAR(CDR(binding));
        lisp_value fn_body = CDR(CDR(binding));
        struct lisp_frame *captured = recursive ? frame : env;
        lisp_value fn = make_closure(params, fn_body, captured, name);
        frame_bind(frame, (uint32_t)slot++, name, fn, BIND_FUNC);
    }
    return eval_progn(body, frame);
}

bool case_match(lisp_value key, lisp_value spec)
{
    if (consp(spec)) {
        while (consp(spec)) {
            if (eql_value(key, CAR(spec)))
                return true;
            spec = CDR(spec);
        }
        return false;
    }
    return eql_value(key, spec);
}

static lisp_value eval_block(lisp_value name, lisp_value body, struct lisp_frame *env)
{
    int ctl = push_ctl(CTL_BLOCK);
    lisp.ctl[ctl].name = name;
    if (setjmp(lisp.ctl[ctl].jb) != 0) {
        lisp.ctl_top = ctl;
        int count = escape_count;
        lisp.mv_count = count > 0 ? count : 1;
        for (int i = 0; i < lisp.mv_count; i++)
            lisp.mv[i] = i < count ? escape_values[i] : NIL_VAL;
        return lisp.mv[0];
    }
    lisp_value result = eval_progn(body, env);
    lisp.ctl_top = ctl;
    return result;
}

static lisp_value quasiquote(lisp_value x, struct lisp_frame *env, int depth)
{
    if (consp(x)) {
        lisp_value head = CAR(x);
        if (head == VAL(lisp.sym_unquote)) {
            if (depth == 1)
                return eval(CAR(CDR(x)), env);
            lisp_value inner = quasiquote(CAR(CDR(x)), env, depth - 1);
            return LIST2(VAL(lisp.sym_unquote), inner);
        }
        if (head == VAL(lisp.sym_quasiquote)) {
            lisp_value inner = quasiquote(CAR(CDR(x)), env, depth + 1);
            return LIST2(VAL(lisp.sym_quasiquote), inner);
        }
        lisp_value out = NIL_VAL, tail = NIL_VAL;
        lisp_value item = x;
        while (consp(item)) {
            lisp_value element = CAR(item);
            if (consp(element) && CAR(element) == VAL(lisp.sym_unquote_splicing) && depth == 1) {
                lisp_value spliced = eval(CAR(CDR(element)), env);
                while (consp(spliced)) {
                    lisp_value cell = cons(CAR(spliced), NIL_VAL);
                    if (nilp(out))
                        out = cell;
                    else
                        CDR(tail) = cell;
                    tail = cell;
                    spliced = CDR(spliced);
                }
            } else {
                lisp_value cell = cons(quasiquote(element, env, depth), NIL_VAL);
                if (nilp(out))
                    out = cell;
                else
                    CDR(tail) = cell;
                tail = cell;
            }
            item = CDR(item);
        }
        if (!nilp(item))
            CDR(tail) = quasiquote(item, env, depth);
        return out;
    }
    return x;
}

static lisp_value place_getf, place_cdr, place_car, place_nth, place_gethash, sym_lambda, sym_define_extension;

static lisp_value tomoe_symbol(const char *name)
{
    return VAL(symbol_intern(PKG_TOMOE, name, strlen(name), false));
}

void lisp_eval_symbols_init(void)
{
    place_getf = tomoe_symbol("GETF");
    place_cdr = tomoe_symbol("CDR");
    place_car = tomoe_symbol("CAR");
    place_nth = tomoe_symbol("NTH");
    place_gethash = tomoe_symbol("GETHASH");
    sym_lambda = tomoe_symbol("LAMBDA");
    sym_define_extension = tomoe_symbol("%DEFINE-EXTENSION");
}


bool lisp_place_head(lisp_value head)
{
    return head == place_getf || head == place_cdr || head == place_nth || head == place_gethash;
}

void eval_place(lisp_value form, struct lisp_frame *env, struct lisp_place *place)
{
    place->kind = PLACE_VAR;
    place->sym = NIL_VAL;
    place->container = NIL_VAL;
    place->key = NIL_VAL;
    place->index = 0;
    place->env = env;
    place->parent_form = NIL_VAL;
    place->value = NIL_VAL;
    if (symbolp(form)) {
        place->kind = PLACE_VAR;
        place->sym = form;
        return;
    }
    if (!consp(form))
        lisp_error("bad setf place");
    lisp_value head = CAR(form);
    if (head != place_getf && head != place_cdr &&
            head != place_car && head != place_nth &&
            head != place_gethash) {
        place->kind = PLACE_VALUE;
        place->value = eval(form, env);
        return;
    }
    if (head == place_car) {
        struct lisp_place parent;
        eval_place(CAR(CDR(form)), env, &parent);
        place->kind = PLACE_NTH;
        place->sym = parent.sym;
        place->env = parent.env;
        place->index = 0;
        place->container = place_get(&parent);
        return;
    }
    if (head == place_getf) {
        struct lisp_place parent;
        eval_place(CAR(CDR(form)), env, &parent);
        place->kind = PLACE_GETF;
        place->sym = parent.sym;
        place->env = env;
        place->parent_form = CAR(CDR(form));
        place->container = place_get(&parent);
        place->key = eval(CAR(CDR(CDR(form))), env);
        return;
    }
    if (head == place_cdr) {
        struct lisp_place parent;
        eval_place(CAR(CDR(form)), env, &parent);
        place->kind = PLACE_CDR;
        place->sym = parent.sym;
        place->env = parent.env;
        place->container = place_get(&parent);
        return;
    }
    if (head == place_nth) {
        struct lisp_place parent;
        eval_place(CAR(CDR(CDR(form))), env, &parent);
        place->kind = PLACE_NTH;
        place->sym = parent.sym;
        place->env = parent.env;
        place->index = require_int(eval(CAR(CDR(form)), env), "nth index");
        place->container = place_get(&parent);
        return;
    }
    if (head == place_gethash) {
        struct lisp_place parent;
        eval_place(CAR(CDR(CDR(form))), env, &parent);
        place->kind = PLACE_GETHASH;
        place->sym = parent.sym;
        place->env = parent.env;
        place->container = place_get(&parent);
        place->key = eval(CAR(CDR(form)), env);
        return;
    }
    lisp_error("unsupported setf place");
}

lisp_value place_get(struct lisp_place *place)
{
    switch (place->kind) {
    case PLACE_VAR:
        return variable_value(place->sym, place->env);
    case PLACE_VALUE:
        return place->value;
    case PLACE_GETF:
        return keyword_get(place->container, place->key, NIL_VAL);
    case PLACE_CDR:
        if (!consp(place->container))
            lisp_error("cdr place is not a cons");
        return CDR(place->container);
    case PLACE_NTH: {
        lisp_value list = list_nthcdr(place->container, place->index);
        return consp(list) ? CAR(list) : NIL_VAL;
    }
    case PLACE_GETHASH: {
        lisp_value entries = PTR(place->container, lisp_hashtable)->entries;
        while (consp(entries)) {
            check_steps();
            lisp_value pair = CAR(entries);
            if (hash_table_match(place->container, CAR(pair), place->key))
                return CDR(pair);
            entries = CDR(entries);
        }
        return NIL_VAL;
    }
    }
    return NIL_VAL;
}

void place_set(struct lisp_place *place, lisp_value value)
{
    switch (place->kind) {
    case PLACE_VAR:
        set_variable(place->sym, value, place->env);
        return;
    case PLACE_VALUE:
        lisp_error("setf of a value that is not a place");
    case PLACE_GETF: {
        lisp_value plist = place->container;
        lisp_value scan = plist;
        while (consp(scan) && consp(CDR(scan))) {
            check_steps();
            if (eql_value(CAR(scan), place->key)) {
                CAR(CDR(scan)) = heap_store(OBJ(CDR(scan)), value);
                return;
            }
            scan = CDR(CDR(scan));
        }
        lisp_value fresh = cons(place->key, cons(value, plist));
        struct lisp_place parent;
        eval_place(place->parent_form, place->env, &parent);
        place_set(&parent, fresh);
        return;
    }
    case PLACE_CDR:
        if (!consp(place->container))
            lisp_error("cdr place is not a cons");
        CDR(place->container) = heap_store(OBJ(place->container), value);
        return;
    case PLACE_NTH: {
        lisp_value list = list_nthcdr(place->container, place->index);
        if (!consp(list))
            lisp_error("nth place out of range");
        CAR(list) = heap_store(OBJ(list), value);
        return;
    }
    case PLACE_GETHASH: {
        struct lisp_hashtable *table = PTR(place->container, lisp_hashtable);
        lisp_value entries = table->entries;
        for (lisp_value scan = entries; consp(scan); scan = CDR(scan)) {
            check_steps();
            lisp_value pair = CAR(scan);
            if (hash_table_match(place->container, CAR(pair), place->key)) {
                CDR(pair) = heap_store(OBJ(pair), value);
                return;
            }
        }
        table->entries = heap_store(table, cons(cons(place->key, value), entries));
        table->count++;
        return;
    }
    }
}

static bool named(const struct lisp_symbol *sym, const char *name, uint32_t len)
{
    return sym->name_len == len && memcmp(sym->name, name, len) == 0;
}

#define NAMED(sym, literal) named((sym), literal, sizeof(literal) - 1)

static bool type_bound(lisp_value spec, lisp_value value, bool lower)
{
    if (symbolp(spec) && NAMED(SYM(spec), "*"))
        return true;
    bool ok = true;
    int cmp = number_compare(value, spec, &ok);
    if (!ok)
        return false;
    return lower ? cmp >= 0 : cmp <= 0;
}

bool lisp_type_check(lisp_value value, lisp_value type, struct lisp_frame *env)
{
    if (symbolp(type)) {
        const struct lisp_symbol *name = SYM(type);
        if (NAMED(name, "INTEGER"))
            return intp(value);
        if (NAMED(name, "FIXNUM"))
            return intp(value);
        if (NAMED(name, "BOOLEAN"))
            return value == T_VAL || nilp(value);
        if (NAMED(name, "STRING"))
            return stringp(value);
        if (NAMED(name, "NUMBER"))
            return numberp(value);
        if (NAMED(name, "REAL"))
            return numberp(value);
        if (NAMED(name, "RATIO"))
            return ratiop(value);
        if (NAMED(name, "DOUBLE-FLOAT"))
            return doublep(value);
        if (NAMED(name, "FUNCTION"))
            return functionp(value);
        if (NAMED(name, "NULL"))
            return nilp(value);
        if (NAMED(name, "LIST"))
            return nilp(value) || consp(value);
        if (NAMED(name, "CONS"))
            return consp(value);
        if (NAMED(name, "SYMBOL"))
            return symbolp(value);
        if (NAMED(name, "KEYWORD"))
            return symbol_is_keyword(value);
        if (NAMED(name, "CHARACTER"))
            return charp(value);
        if (NAMED(name, "VECTOR"))
            return false;
        return true;
    }
    if (!consp(type))
        return true;
    lisp_value head = CAR(type);
    if (!symbolp(head))
        return true;
    const struct lisp_symbol *op = SYM(head);
    if (NAMED(op, "INTEGER") || NAMED(op, "REAL")) {
        if (!numberp(value))
            return false;
        if (NAMED(op, "INTEGER") && !intp(value))
            return false;
        if (consp(CDR(type)) && !type_bound(CAR(CDR(type)), value, true))
            return false;
        if (consp(CDR(CDR(type))) && !type_bound(CAR(CDR(CDR(type))), value, false))
            return false;
        return true;
    }
    if (NAMED(op, "AND")) {
        lisp_value p = CDR(type);
        while (consp(p)) {
            if (!lisp_type_check(value, CAR(p), env))
                return false;
            p = CDR(p);
        }
        return true;
    }
    if (NAMED(op, "OR")) {
        lisp_value p = CDR(type);
        while (consp(p)) {
            if (lisp_type_check(value, CAR(p), env))
                return true;
            p = CDR(p);
        }
        return false;
    }
    if (NAMED(op, "NOT"))
        return !lisp_type_check(value, CAR(CDR(type)), env);
    if (NAMED(op, "SATISFIES")) {
        lisp_value fn = CAR(CDR(type));
        lisp_value fn_value = symbolp(fn) ? fn : (nilp(fn) ? NIL_VAL : eval(fn, env));
        lisp_value args[1] = { value };
        return truthy(apply_function(fn_value, 1, args));
    }
    if (NAMED(op, "MEMBER")) {
        lisp_value p = CDR(type);
        while (consp(p)) {
            if (eql_value(value, CAR(p)))
                return true;
            p = CDR(p);
        }
        return false;
    }
    if (NAMED(op, "EQL"))
        return eql_value(value, CAR(CDR(type)));
    return true;
}

static lisp_value eval_special(lisp_value form, struct lisp_frame *env, int id);

lisp_value eval(lisp_value form, struct lisp_frame *env)
{
    if (TYPE(form) != LT_CONS) {
        if (TYPE(form) == LT_SYMBOL) {
            lisp_value value = variable_value(form, env);
            mv_return1(value);
            return value;
        }
        mv_return1(form);
        return form;
    }
    lisp_value head = CAR(form);
    if (TYPE(head) == LT_SYMBOL) {
        lisp_value *local_slot = frame_slot(env, head, BIND_FUNC);
        if (local_slot && !nilp(*local_slot))
            return eval_call(*local_slot, CDR(form), env);
        if (SYM(head)->special_id)
            return eval_special(form, env, (int)SYM(head)->special_id);
        if (SYM(head)->h.flags & SYM_MACRO) {
            int argc = 0;
            for (lisp_value scan = CDR(form); consp(scan); scan = CDR(scan))
                argc++;
            lisp_value *argv = heap_alloc(sizeof(lisp_value) * (size_t)(argc + 1));
            lisp_value macro_args = CDR(form);
            int i = 0;
            while (consp(macro_args)) {
                argv[i++] = CAR(macro_args);
                macro_args = CDR(macro_args);
            }
            lisp_value expansion = apply_function(SYM(head)->function, argc, argv);
            return eval(expansion, env);
        }
        if (!nilp(SYM(head)->function))
            return eval_call(SYM(head)->function, CDR(form), env);
        lisp_error("undefined function %s", SYM(head)->name);
    }
    return eval_call(eval(head, env), CDR(form), env);
}

static lisp_value eval_special(lisp_value form, struct lisp_frame *env, int id)
{
    lisp_value args = CDR(form);
    switch (id) {
    case SF_QUOTE: {
        lisp_value v = CAR(args);
        mv_return1(v);
        return v;
    }
    case SF_FUNCTION: {
        lisp_value target = CAR(args);
        if (TYPE(target) == LT_CONS && CAR(target) == VAL(lisp.sym_quote))
            target = CAR(CDR(target));
        if (symbolp(target)) {
            bool found = false;
            lisp_value fn = function_value(target, env, &found);
            if (!found || nilp(fn))
                lisp_error("undefined function %s", SYM(target)->name);
            mv_return1(fn);
            return fn;
        }
        if (TYPE(target) == LT_CONS && CAR(target) == sym_lambda) {
            lisp_value fn = eval(target, env);
            mv_return1(fn);
            return fn;
        }
        mv_return1(target);
        return target;
    }
    case SF_IF: {
        lisp_value test = eval(CAR(args), env);
        if (truthy(test))
            return eval(CAR(CDR(args)), env);
        if (consp(CDR(CDR(args))))
            return eval(CAR(CDR(CDR(args))), env);
        mv_return1(NIL_VAL);
        return NIL_VAL;
    }
    case SF_PROGN:
        return eval_progn(args, env);
    case SF_LET:
        return eval_let(form, env, false);
    case SF_LET_STAR:
        return eval_let(form, env, true);
    case SF_SETQ: {
        lisp_value result = NIL_VAL;
        lisp_value p = args;
        while (consp(p) && consp(CDR(p))) {
            lisp_value sym = CAR(p);
            lisp_value value = eval(CAR(CDR(p)), env);
            set_variable(sym, value, env);
            result = value;
            p = CDR(CDR(p));
        }
        mv_return1(result);
        return result;
    }
    case SF_FLET:
        return eval_flet(form, env, false);
    case SF_LABELS:
        return eval_flet(form, env, true);
    case SF_LAMBDA: {
        lisp_value fn = make_closure(CAR(args), CDR(args), env, NIL_VAL);
        mv_return1(fn);
        return fn;
    }
    case SF_BLOCK:
        return eval_block(CAR(args), CDR(args), env);
    case SF_RETURN_FROM: {
        lisp_value name = CAR(args);
        lisp_value value_form = consp(CDR(args)) ? CAR(CDR(args)) : NIL_VAL;
        lisp_value buffer[LISP_MAX_MV];
        int count = 1;
        buffer[0] = NIL_VAL;
        if (!nilp(value_form)) {
            eval(value_form, env);
            count = lisp.mv_count > 0 ? lisp.mv_count : 1;
            if (count > LISP_MAX_MV)
                count = LISP_MAX_MV;
            for (int i = 0; i < count; i++)
                buffer[i] = lisp.mv[i];
        }
        throw_return(name, count, buffer);
        buffer[0] = NIL_VAL;
        throw_return(name, 1, buffer);
    }
    case SF_RETURN: {
        lisp_value value_form = consp(args) ? CAR(args) : NIL_VAL;
        eval(value_form, env);
        int count = lisp.mv_count > 0 ? lisp.mv_count : 1;
        lisp_value buffer[LISP_MAX_MV];
        for (int i = 0; i < count; i++)
            buffer[i] = lisp.mv[i];
        throw_return(NIL_VAL, count, buffer);
        buffer[0] = NIL_VAL;
        throw_return(NIL_VAL, 1, buffer);
    }
    case SF_MULTIPLE_VALUE_LIST: {
        eval(CAR(args), env);
        int count = lisp.mv_count;
        lisp_value out = NIL_VAL;
        for (int i = count - 1; i >= 0; i--)
            out = cons(lisp.mv[i], out);
        mv_return1(out);
        return out;
    }
    case SF_MULTIPLE_VALUE_BIND: {
        lisp_value vars = CAR(args);
        lisp_value form2 = CAR(CDR(args));
        lisp_value body = CDR(CDR(args));
        lisp_value value = eval(form2, env);
        int count = lisp.mv_count;
        lisp_value captured[LISP_MAX_MV];
        for (int i = 0; i < count; i++)
            captured[i] = lisp.mv[i];
        (void)value;
        int n = list_length(vars);
        struct lisp_frame *frame = frame_new(env, (uint32_t)n + 1);
        int slot = 0;
        lisp_value v = vars;
        while (consp(v)) {
            lisp_value sym = CAR(v);
            v = CDR(v);
            lisp_value bound = slot < count ? captured[slot] : NIL_VAL;
            frame_bind(frame, (uint32_t)slot, sym, bound, BIND_VAR);
            slot++;
        }
        return eval_progn(body, frame);
    }
    case SF_MULTIPLE_VALUE_SETQ: {
        lisp_value vars = CAR(args);
        lisp_value value = eval(CAR(CDR(args)), env);
        int count = lisp.mv_count;
        lisp_value captured[LISP_MAX_MV];
        for (int i = 0; i < count; i++)
            captured[i] = lisp.mv[i];
        (void)value;
        int slot = 0;
        lisp_value v = vars;
        while (consp(v)) {
            lisp_value sym = CAR(v);
            set_variable(sym, slot < count ? captured[slot] : NIL_VAL, env);
            slot++;
            v = CDR(v);
        }
        mv_return1(count > 0 ? captured[0] : NIL_VAL);
        return lisp.mv[0];
    }
    case SF_DECLARE:
    case SF_DECLAIM:
        mv_return1(NIL_VAL);
        return NIL_VAL;
    case SF_DEFUN: {
        lisp_value name = CAR(args);
        lisp_value params = CAR(CDR(args));
        lisp_value body = CDR(CDR(args));
        lisp_value fn = make_closure(params, body, env, name);
        symbol_defun(SYM(name), fn);
        mv_return1(name);
        return name;
    }
    case SF_DEFPARAMETER: {
        lisp_value name = CAR(args);
        lisp_value init = eval(CAR(CDR(args)), env);
        if (!dynamically_bound(SYM(name)))
            check_global_store(name, init);
        symbol_make_special(SYM(name));
        SYM(name)->value = init;
        mv_return1(name);
        return name;
    }
    case SF_DEFVAR: {
        lisp_value name = CAR(args);
        if (SYM(name)->value == VAL(lisp.sym_unbound) && consp(CDR(args))) {
            lisp_value init = eval(CAR(CDR(args)), env);
            if (!dynamically_bound(SYM(name)))
                check_global_store(name, init);
            SYM(name)->value = init;
        }
        symbol_make_special(SYM(name));
        mv_return1(name);
        return name;
    }
    case SF_DEFMACRO: {
        lisp_value name = CAR(args);
        lisp_value params = CAR(CDR(args));
        lisp_value body = CDR(CDR(args));
        lisp_value fn = make_closure(params, body, env, name);
        lisp_mark_builtin_redefined(SYM(name));
        SYM(name)->function = fn;
        SYM(name)->h.flags |= SYM_MACRO;
        if (SYM(name)->package == PKG_TOMOE)
            SYM(name)->h.flags |= SYM_VISIBLE;
        mv_return1(name);
        return name;
    }
    case SF_IN_PACKAGE: {
        lisp_value target = CAR(args);
        const char *name = NULL;
        if (symbolp(target))
            name = SYM(target)->name;
        else if (stringp(target))
            name = PTR(target, lisp_string)->data;
        if (!name)
            lisp_error("in-package needs a package name");
        if (lisp.cur_package < LISP_MAX_PACKAGES && lisp.packages[lisp.cur_package].used) {
            const char *current = lisp.packages[lisp.cur_package].name;
            const char *nickname = lisp.packages[lisp.cur_package].nickname;
            if (strcmp(name, current) == 0 || (nickname[0] && strcmp(name, nickname) == 0))
                return NIL_VAL;
            if (strcmp(name, "TOMOE-USER") == 0 &&
                    lisp.cur_package != PKG_TOMOE && lisp.cur_package != PKG_KEYWORD)
                return NIL_VAL;
            if (lisp.cur_package == PKG_TOMOE && strcmp(name, "TOMOE") == 0)
                return NIL_VAL;
        }
        lisp_error("in-package %s is not supported", name);
    }
    case SF_AND: {
        lisp_value result = T_VAL;
        while (consp(args)) {
            result = eval(CAR(args), env);
            if (nilp(result)) {
                mv_return1(NIL_VAL);
                return NIL_VAL;
            }
            args = CDR(args);
        }
        return result;
    }
    case SF_OR: {
        lisp_value result = NIL_VAL;
        while (consp(args)) {
            result = eval(CAR(args), env);
            if (truthy(result))
                return result;
            args = CDR(args);
        }
        mv_return1(NIL_VAL);
        return NIL_VAL;
    }
    case SF_WHEN: {
        lisp_value test = eval(CAR(args), env);
        if (truthy(test))
            return eval_progn(CDR(args), env);
        mv_return1(NIL_VAL);
        return NIL_VAL;
    }
    case SF_UNLESS: {
        lisp_value test = eval(CAR(args), env);
        if (nilp(test))
            return eval_progn(CDR(args), env);
        mv_return1(NIL_VAL);
        return NIL_VAL;
    }
    case SF_COND: {
        lisp_value clauses = args;
        while (consp(clauses)) {
            lisp_value clause = CAR(clauses);
            lisp_value test = eval(CAR(clause), env);
            if (truthy(test)) {
                if (consp(CDR(clause)))
                    return eval_progn(CDR(clause), env);
                return test;
            }
            clauses = CDR(clauses);
        }
        mv_return1(NIL_VAL);
        return NIL_VAL;
    }
    case SF_CASE:
    case SF_ECASE: {
        lisp_value key = eval(CAR(args), env);
        lisp_value clauses = CDR(args);
        bool matched = false;
        while (consp(clauses)) {
            lisp_value clause = CAR(clauses);
            lisp_value spec = CAR(clause);
            bool is_default = spec == T_VAL || eql_value(spec, VAL(lisp.sym_otherwise));
            if (is_default || case_match(key, spec)) {
                matched = true;
                lisp_value result = eval_progn(CDR(clause), env);
                return result;
            }
            clauses = CDR(clauses);
        }
        if (id == SF_ECASE) {
            char shown[160];
            char whole[400];
            lisp_print(key, true, shown, sizeof(shown));
            lisp_print(form, true, whole, sizeof(whole));
            lisp_error("ECASE %s has no clause for %s", whole, shown);
        }
        (void)matched;
        mv_return1(NIL_VAL);
        return NIL_VAL;
    }
    case SF_DOLIST: {
        lisp_value spec = CAR(args);
        lisp_value var = CAR(spec);
        lisp_value list = eval(CAR(CDR(spec)), env);
        lisp_value body = CDR(args);
        struct lisp_frame *frame = frame_new(env, 1);
        int ctl = push_ctl(CTL_BLOCK);
        lisp.ctl[ctl].name = NIL_VAL;
        if (setjmp(lisp.ctl[ctl].jb) != 0) {
            lisp.ctl_top = ctl;
            lisp.mv_count = escape_count > 0 ? escape_count : 1;
            for (int i = 0; i < lisp.mv_count; i++)
                lisp.mv[i] = i < escape_count ? escape_values[i] : NIL_VAL;
            return lisp.mv[0];
        }
        lisp_value result_form = consp(CDR(CDR(spec))) ? CAR(CDR(CDR(spec))) : NIL_VAL;
        while (consp(list)) {
            check_steps();
            frame_bind(frame, 0, var, CAR(list), BIND_VAR);
            eval_progn(body, frame);
            list = CDR(list);
        }
        lisp.ctl_top = ctl;
        if (!nilp(result_form))
            return eval(result_form, frame);
        mv_return1(NIL_VAL);
        return NIL_VAL;
    }
    case SF_DOTIMES: {
        lisp_value spec = CAR(args);
        lisp_value var = CAR(spec);
        int64_t count = require_int(eval(CAR(CDR(spec)), env), "dotimes count");
        lisp_value body = CDR(args);
        struct lisp_frame *frame = frame_new(env, 1);
        int ctl = push_ctl(CTL_BLOCK);
        lisp.ctl[ctl].name = NIL_VAL;
        if (setjmp(lisp.ctl[ctl].jb) != 0) {
            lisp.ctl_top = ctl;
            lisp.mv_count = escape_count > 0 ? escape_count : 1;
            for (int i = 0; i < lisp.mv_count; i++)
                lisp.mv[i] = i < escape_count ? escape_values[i] : NIL_VAL;
            return lisp.mv[0];
        }
        lisp_value result_form = consp(CDR(CDR(spec))) ? CAR(CDR(CDR(spec))) : NIL_VAL;
        for (int64_t i = 0; i < count; i++) {
            check_steps();
            frame_bind(frame, 0, var, make_int(i), BIND_VAR);
            eval_progn(body, frame);
        }
        lisp.ctl_top = ctl;
        if (!nilp(result_form))
            return eval(result_form, frame);
        mv_return1(NIL_VAL);
        return NIL_VAL;
    }
    case SF_DESTRUCTURING_BIND: {
        lisp_value pattern = CAR(args);
        lisp_value value = eval(CAR(CDR(args)), env);
        lisp_value body = CDR(CDR(args));
        uint32_t slots = pattern_slots(pattern);
        struct lisp_frame *frame = frame_new(env, slots + 1);
        bind_pattern(frame, 0, pattern, value);
        return eval_progn(body, frame);
    }
    case SF_IGNORE_ERRORS: {
        int ctl = push_ctl(CTL_CATCH);
        if (setjmp(lisp.ctl[ctl].jb) != 0) {
            lisp.ctl_top = ctl;
            lisp_value condition = escape_condition;
            mv_return2(NIL_VAL, condition);
            return NIL_VAL;
        }
        lisp_value result = eval_progn(args, env);
        lisp.ctl_top = ctl;
        return result;
    }
    case SF_HANDLER_CASE: {
        lisp_value body_form = CAR(args);
        lisp_value clauses = CDR(args);
        int ctl = push_ctl(CTL_CATCH);
        if (setjmp(lisp.ctl[ctl].jb) != 0) {
            lisp.ctl_top = ctl;
            lisp_value condition = escape_condition;
            while (consp(clauses)) {
                lisp_value clause = CAR(clauses);
                clauses = CDR(clauses);
                lisp_value var_spec = CAR(CDR(clause));
                lisp_value var = consp(var_spec) ? CAR(var_spec) : var_spec;
                struct lisp_frame *frame = frame_new(env, 1);
                frame_bind(frame, 0, var, condition, BIND_VAR);
                return eval_progn(CDR(CDR(clause)), frame);
            }
            throw_error_value(condition);
        }
        lisp_value result = eval(body_form, env);
        lisp.ctl_top = ctl;
        return result;
    }
    case SF_UNWIND_PROTECT: {
        lisp_value protected = CAR(args);
        lisp_value cleanup = CDR(args);
        int ctl = push_ctl(CTL_CLEANUP);
        lisp.ctl[ctl].form = cleanup;
        lisp.ctl[ctl].env = env;
        lisp_value result = eval(protected, env);
        lisp.ctl_top = ctl;
        eval_progn(cleanup, env);
        return result;
    }
    case SF_SETF: {
        lisp_value p = args;
        lisp_value result = NIL_VAL;
        while (consp(p) && consp(CDR(p))) {
            struct lisp_place place;
            eval_place(CAR(p), env, &place);
            lisp_value value = eval(CAR(CDR(p)), env);
            int count = lisp.mv_count;
            lisp_value captured[LISP_MAX_MV];
            for (int i = 0; i < count && i < LISP_MAX_MV; i++)
                captured[i] = lisp.mv[i];
            place_set(&place, value);
            result = value;
            if (count > 0) {
                lisp.mv_count = count;
                for (int i = 0; i < count; i++)
                    lisp.mv[i] = captured[i];
            }
            p = CDR(CDR(p));
        }
        return result;
    }
    case SF_PUSH:
    case SF_PUSHNEW: {
        lisp_value item = eval(CAR(args), env);
        struct lisp_place place;
        eval_place(CAR(CDR(args)), env, &place);
        lisp_value list = place_get(&place);
        if (id == SF_PUSHNEW) {
            bool present = false;
            for (lisp_value scan = list; consp(scan); scan = CDR(scan)) {
                check_steps();
                if (equal_value(item, CAR(scan))) {
                    present = true;
                    break;
                }
            }
            if (!present) {
                lisp_value fresh = cons(item, list);
                place_set(&place, fresh);
                mv_return1(fresh);
                return fresh;
            }
            return list;
        }
        lisp_value fresh = cons(item, list);
        place_set(&place, fresh);
        mv_return1(fresh);
        return fresh;
    }
    case SF_POP: {
        struct lisp_place place;
        eval_place(CAR(args), env, &place);
        lisp_value list = place_get(&place);
        if (!consp(list))
            lisp_error("POP of an empty list");
        place_set(&place, CDR(list));
        mv_return1(CAR(list));
        return CAR(list);
    }
    case SF_INCF:
    case SF_DECF: {
        struct lisp_place place;
        eval_place(CAR(args), env, &place);
        lisp_value delta = consp(CDR(args)) ? eval(CAR(CDR(args)), env) : make_int(1);
        lisp_value current = place_get(&place);
        lisp_value value = id == SF_INCF ? number_add(current, delta) : number_sub(current, delta);
        place_set(&place, value);
        mv_return1(value);
        return value;
    }
    case SF_ROTATEF: {
        struct lisp_place resolved[8];
        lisp_value values[8];
        int n = 0;
        lisp_value p = args;
        while (consp(p) && n < 8) {
            eval_place(CAR(p), env, &resolved[n]);
            n++;
            p = CDR(p);
        }
        if (n >= 2) {
            for (int i = 0; i < n; i++)
                values[i] = place_get(&resolved[i]);
            for (int i = 0; i < n; i++)
                place_set(&resolved[i], values[(i + 1) % n]);
        }
        mv_return1(NIL_VAL);
        return NIL_VAL;
    }
    case SF_LOOP:
        return loop_eval(args, env, lisp_source_file());
    case SF_QUASIQUOTE: {
        lisp_value result = quasiquote(CAR(args), env, 1);
        mv_return1(result);
        return result;
    }
    case SF_DEFINE_EXTENSION: {
        lisp_value name = eval(CAR(args), env);
        lisp_value options = CAR(CDR(args));
        lisp_value lambda_list = CAR(CDR(CDR(args)));
        lisp_value body = CDR(CDR(CDR(args)));
        lisp_value reads = NIL_VAL, state = NIL_VAL, admission = NIL_VAL;
        lisp_value opt = options;
        while (consp(opt) && consp(CDR(opt))) {
            lisp_value key = CAR(opt);
            lisp_value value = CAR(CDR(opt));
            if (lisp_eq_keyword(key, "READS"))
                reads = value;
            else if (lisp_eq_keyword(key, "STATE"))
                state = eval(value, env);
            else if (lisp_eq_keyword(key, "ADMISSION"))
                admission = eval(value, env);
            opt = CDR(CDR(opt));
        }
        if (nilp(reads))
            reads = NIL_VAL;
        lisp_value fn = make_closure(lambda_list, body, env, NIL_VAL);
        struct lisp_symbol *register_sym = SYM(sym_define_extension);
        if (nilp(register_sym->function))
            lisp_error("%s", "%DEFINE-EXTENSION is not defined");
        lisp_value argv[5] = { name, reads, state, fn, admission };
        mv_return1(NIL_VAL);
        return apply_function(register_sym->function, 5, argv);
    }
    case SF_EVAL: {
        lisp_value target = eval(CAR(args), env);
        return lisp_analyse_and_eval(target, env);
    }
    case SF_PROG1: {
        lisp_value first = eval(CAR(args), env);
        int count = lisp.mv_count;
        lisp_value captured[LISP_MAX_MV];
        for (int i = 0; i < count; i++)
            captured[i] = lisp.mv[i];
        eval_progn(CDR(args), env);
        for (int i = 0; i < count; i++)
            lisp.mv[i] = captured[i];
        lisp.mv_count = count;
        return first;
    }
    case SF_CHECK_TYPE: {
        lisp_value form = CAR(args);
        bool is_place = symbolp(form);
        if (consp(form)) {
            lisp_value head = CAR(form);
            is_place = head == place_getf || head == place_cdr ||
                       head == place_nth || head == place_gethash;
        }
        lisp_value value;
        if (is_place) {
            struct lisp_place place;
            eval_place(form, env, &place);
            value = place_get(&place);
        } else {
            value = eval(form, env);
        }
        lisp_value type = CAR(CDR(args));
        if (!lisp_type_check(value, type, env))
            lisp_error("The value %s is not of type %s", "value", "type");
        mv_return1(NIL_VAL);
        return NIL_VAL;
    }
    default:
        lisp_error("unsupported special form");
    }
}

lisp_value lisp_analyse_and_eval(lisp_value form, struct lisp_frame *env)
{
    lisp_analyse_form(form, NIL_VAL, NIL_VAL);
    return eval(form, env);
}

lisp_value lisp_escape_condition(void)
{
    return escape_condition;
}

int lisp_escape_count(void)
{
    return escape_count;
}

const lisp_value *lisp_escape_values_ptr(void)
{
    return escape_values;
}

void lisp_set_limits(uint64_t steps, uint64_t deadline_ns)
{
    lisp.pending_steps = steps;
    lisp.pending_deadline_ns = deadline_ns;
}

void lisp_end_turn(void)
{
    if (lisp.call_depth == 0) {
        heap_arena_reset();
        heap_reclaim_dropped();
    }
    lisp.steps_total_last = lisp.steps;
}

const char *lisp_active_hook(void)
{
    return lisp.current_hook;
}

static lisp_value lookup_hook(const char *function)
{
    const char *sep = strstr(function, "::");
    uint32_t package = PKG_TOMOE;
    const char *name = function;
    if (sep) {
        size_t plen = (size_t)(sep - function);
        if (plen == 9 && strncmp(function, "KEYWORD", 7) == 0)
            package = PKG_KEYWORD;
        else if (plen == 10 && strncmp(function, "TOMOE-USER", 10) == 0)
            package = PKG_TOMOE_USER;
        name = sep + 2;
    }
    struct lisp_symbol *sym = symbol_find(package, name, strlen(name));
    if (!sym)
        return NIL_VAL;
    return sym->function;
}

int lisp_call(const char *function, int argc, const lisp_value *argv, lisp_value *result,
    char *error, size_t error_size)
{
    if (result)
        *result = NIL_VAL;
    if (!lisp.initialised) {
        if (error && error_size)
            snprintf(error, error_size, "lisp runtime not initialised");
        return -1;
    }
    if (lisp.call_depth > 0) {
        if (error && error_size)
            snprintf(error, error_size, "nested lisp call");
        return -1;
    }
    lisp_value fn = lookup_hook(function);
    lisp.current_hook = function;
    if (nilp(fn)) {
        if (error && error_size)
            snprintf(error, error_size, "undefined lisp hook %s", function);
        return -1;
    }
    lisp.steps = 0;
    lisp.limited_depth = 0;
    lisp.steps_limit = lisp.pending_steps;
    lisp.deadline_at = lisp.pending_deadline_ns ? lisp_cpu_ns() + lisp.pending_deadline_ns : 0;
    lisp.call_depth++;
    struct lisp_region *saved_region = lisp.cur_region;
    lisp.cur_region = &lisp.arena;
    int index = push_ctl(CTL_TOP);
    bool ok = true;
    if (setjmp(lisp.ctl[index].jb) != 0) {
        lisp.ctl_top = index;
        ok = false;
    } else {
        lisp.mv_count = 0;
        lisp_value value = apply_function(fn, argc, (lisp_value *)argv);
        if (result)
            *result = value;
        lisp.ctl_top = index;
    }
    pop_ctl(index);
    lisp.call_depth--;
    lisp.cur_region = saved_region;
    lisp.steps_total_last = lisp.steps;
    lisp.deadline_at = 0;
    lisp.steps_limit = 0;
    lisp.current_hook = NULL;
    if (!ok) {
        if (error && error_size) {
            lisp_value message = condition_message(lisp_escape_condition());
            size_t length = 0;
            const char *text = lisp_get_string(message, &length);
            if (text && length)
                snprintf(error, error_size, "%.*s", (int)length, text);
            else
                snprintf(error, error_size, "%s", lisp_error_message());
        }
        lisp_clear_location();
        return -1;
    }
    lisp_clear_location();
    return 0;
}

