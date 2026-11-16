#include <string.h>
#include <stdio.h>
#include <setjmp.h>

#include "lisp-internal.h"

struct cnode;
typedef lisp_value (*cnode_run)(const struct cnode *node, struct lisp_frame *env);

enum { WRAPPER_ARGS = 8 };

struct wrapper {
    lisp_value closure;
    struct lisp_symbol *callee;
    lisp_value builtin;
    uint32_t params;
    uint32_t count;
    int32_t source[WRAPPER_ARGS];
    lisp_value constant[WRAPPER_ARGS];
};

struct cnode {
    cnode_run run;
    lisp_value form;
    lisp_value sym;
    uint32_t depth;
    uint32_t index;
    uint32_t count;
    bool stack;
    const struct cnode *a;
    union {
        const struct cnode *b;
        uint8_t *dynamic;
    };
    union {
        const struct cnode *c;
        lisp_value *syms;
        const struct lisp_code *code;
        const struct wrapper *wrapper;
    };
    const struct cnode **kids;
};

struct lisp_code {
    const struct cnode *body;
    const struct wrapper *wrapper;
    uint8_t info;
    uint32_t slots;
};

struct cscope {
    struct cscope *parent;
    bool frame;
    bool barrier;
    uint32_t count;
    const lisp_value *syms;
};

struct compiler {
    struct lisp_region *region;
    bool stack;
    int depth;
};

enum lookup { LOOKUP_LEXICAL, LOOKUP_RUNTIME };

static const struct cnode *compile_form(struct compiler *cc, lisp_value form, struct cscope *scope);
static const struct cnode *compile_body(struct compiler *cc, lisp_value body, struct cscope *scope);
static const struct lisp_code *compile_lambda(struct compiler *cc, lisp_value params, lisp_value body,
    uint8_t info, uint32_t slots, struct cscope *outer);

static struct cnode *node_new(struct compiler *cc, cnode_run run, lisp_value form)
{
    struct cnode *node = heap_alloc_in(cc->region, sizeof(*node));
    memset(node, 0, sizeof(*node));
    node->run = run;
    node->form = form;
    node->sym = NIL_VAL;
    return node;
}

static const struct cnode **kids_new(struct compiler *cc, uint32_t count)
{
    if (!count)
        return NULL;
    return heap_alloc_in(cc->region, sizeof(struct cnode *) * count);
}

static enum lookup scope_lookup(struct cscope *scope, lisp_value sym, uint32_t *depth, uint32_t *index)
{
    uint32_t d = 0;
    for (struct cscope *s = scope; s; s = s->parent) {
        if (s->barrier)
            return LOOKUP_RUNTIME;
        for (uint32_t i = s->count; i-- > 0;) {
            if (s->syms[i] == sym) {
                *depth = d;
                *index = i;
                return LOOKUP_LEXICAL;
            }
        }
        if (s->frame)
            d++;
    }
    return LOOKUP_RUNTIME;
}

static inline struct lisp_frame *frame_at(struct lisp_frame *env, uint32_t depth)
{
    while (depth--)
        env = env->parent;
    return env;
}

static struct lisp_frame *stack_frame(void *memory, struct lisp_frame *parent, uint32_t count)
{
    struct lisp_frame *frame = memory;
    frame->h.type = LT_FRAME;
    frame->h.flags = 0;
    frame->h.aux = 0;
    frame->h.size = 0;
    frame->parent = parent;
    frame->count = count;
    for (uint32_t i = 0; i < count; i++) {
        frame->entry[i].sym = NIL_VAL;
        frame->entry[i].value = VAL(lisp.sym_unbound);
        frame->entry[i].kind = BIND_VAR;
    }
    return frame;
}

#define FRAME_BYTES(count) (offsetof(struct lisp_frame, entry) + (size_t)(count) * sizeof(struct lisp_binding))
#define NODE_FRAME(node, env, count) ((node)->stack ? \
    stack_frame(__builtin_alloca(FRAME_BYTES(count)), (env), (count)) : frame_new((env), (count)))

static lisp_value run_eval(const struct cnode *node, struct lisp_frame *env)
{
    return eval(node->form, env);
}

static lisp_value run_const(const struct cnode *node, struct lisp_frame *env)
{
    mv_return1(node->sym);
    return node->sym;
}

static lisp_value run_lexical(const struct cnode *node, struct lisp_frame *env)
{
    lisp_value value = frame_at(env, node->depth)->entry[node->index].value;
    if (value == VAL(lisp.sym_unbound))
        lisp_error("unbound variable %s", SYM(node->sym)->name);
    mv_return1(value);
    return value;
}

static lisp_value run_special(const struct cnode *node, struct lisp_frame *env)
{
    lisp_value value = SYM(node->sym)->value;
    if (value == VAL(lisp.sym_unbound))
        lisp_error("unbound variable %s", SYM(node->sym)->name);
    mv_return1(value);
    return value;
}

static lisp_value run_variable(const struct cnode *node, struct lisp_frame *env)
{
    lisp_value value = variable_value(node->sym, env);
    mv_return1(value);
    return value;
}

enum { CALL_STACK_ARGS = 8 };

static lisp_value call_with(const struct cnode *node, lisp_value fn, struct lisp_frame *env)
{
    lisp_value stack[CALL_STACK_ARGS];
    uint32_t count = node->count;
    lisp_value *argv = count <= CALL_STACK_ARGS ? stack : heap_alloc(sizeof(lisp_value) * count);
    for (uint32_t i = 0; i < count; i++)
        argv[i] = node->kids[i]->run(node->kids[i], env);
    return apply_fast(fn, (int)count, argv);
}

static lisp_value run_call(const struct cnode *node, struct lisp_frame *env)
{
    struct lisp_symbol *sym = SYM(node->sym);
    if (sym->h.flags & SYM_LOCAL_FUNC) {
        lisp_value *slot = frame_slot(env, node->sym, BIND_FUNC);
        if (slot && !nilp(*slot))
            return call_with(node, *slot, env);
    }
    if (nilp(sym->function))
        lisp_error("undefined function %s", sym->name);
    return call_with(node, sym->function, env);
}

static lisp_value run_wrapper(const struct cnode *node, struct lisp_frame *env)
{
    const struct wrapper *w = node->wrapper;
    struct lisp_symbol *sym = SYM(node->sym);
    if (sym->function != w->closure || w->callee->function != w->builtin ||
            (sym->h.flags & SYM_LOCAL_FUNC) || node->count != w->params)
        return run_call(node, env);
    lisp_value args[WRAPPER_ARGS];
    for (uint32_t i = 0; i < node->count; i++)
        args[i] = node->kids[i]->run(node->kids[i], env);
    lisp_value argv[WRAPPER_ARGS];
    for (uint32_t i = 0; i < w->count; i++)
        argv[i] = w->source[i] < 0 ? w->constant[i] : args[w->source[i]];
    check_steps();
    return apply_fast(w->builtin, (int)w->count, argv);
}

static const struct wrapper *wrapper_for(struct lisp_region *region, lisp_value fn)
{
    if (TYPE(fn) != LT_FUNCTION)
        return NULL;
    struct lisp_function *f = PTR(fn, lisp_function);
    if (f->kind != FN_CLOSURE || f->env || !f->gen ||
            (f->gen != region && f->gen->package != PKG_TOMOE))
        return NULL;
    if (!(f->info & FNI_KNOWN))
        lisp_closure_info(f);
    if (!(f->info & FNI_SIMPLE) || f->slots > WRAPPER_ARGS)
        return NULL;
    lisp_value body = f->body;
    if (consp(body) && stringp(CAR(body)) && consp(CDR(body)))
        body = CDR(body);
    if (!consp(body) || !nilp(CDR(body)) || !consp(CAR(body)))
        return NULL;
    lisp_value call = CAR(body);
    if (!symbolp(CAR(call)))
        return NULL;
    struct lisp_symbol *callee = SYM(CAR(call));
    if (callee->special_id || (callee->h.flags & (SYM_MACRO | SYM_LOCAL_FUNC)) ||
            TYPE(callee->function) != LT_FUNCTION ||
            PTR(callee->function, lisp_function)->kind != FN_BUILTIN)
        return NULL;
    struct wrapper *w = heap_alloc_in(region, sizeof(*w));
    memset(w, 0, sizeof(*w));
    w->closure = fn;
    w->callee = callee;
    w->builtin = callee->function;
    w->params = f->slots;
    for (lisp_value arg = CDR(call); consp(arg); arg = CDR(arg)) {
        if (w->count == WRAPPER_ARGS)
            return NULL;
        lisp_value item = CAR(arg);
        int32_t source = -1;
        if (symbolp(item) && !(SYM(item)->h.flags & SYM_KEYWORD) && item != NIL_VAL && item != T_VAL) {
            uint32_t index = 0;
            for (lisp_value p = f->params; consp(p); p = CDR(p), index++)
                if (CAR(p) == item)
                    source = (int32_t)index;
            if (source < 0 || (SYM(item)->h.flags & SYM_SPECIAL))
                return NULL;
        } else if (consp(item)) {
            if (CAR(item) != VAL(lisp.sym_quote) || !consp(CDR(item)) || !nilp(CDR(CDR(item))))
                return NULL;
            item = CAR(CDR(item));
        } else if (stringp(item)) {
            return NULL;
        }
        w->source[w->count] = source;
        w->constant[w->count] = source < 0 ? item : NIL_VAL;
        w->count++;
    }
    return w;
}

static lisp_value run_progn(const struct cnode *node, struct lisp_frame *env)
{
    if (!node->count)
        return NIL_VAL;
    uint32_t last = node->count - 1;
    for (uint32_t i = 0; i < last; i++)
        node->kids[i]->run(node->kids[i], env);
    return node->kids[last]->run(node->kids[last], env);
}

static lisp_value run_if(const struct cnode *node, struct lisp_frame *env)
{
    if (truthy(node->a->run(node->a, env)))
        return node->b->run(node->b, env);
    if (node->c)
        return node->c->run(node->c, env);
    mv_return1(NIL_VAL);
    return NIL_VAL;
}

static lisp_value run_when(const struct cnode *node, struct lisp_frame *env)
{
    lisp_value test = node->a->run(node->a, env);
    if (node->index ? nilp(test) : truthy(test))
        return node->b->run(node->b, env);
    mv_return1(NIL_VAL);
    return NIL_VAL;
}

static lisp_value run_and(const struct cnode *node, struct lisp_frame *env)
{
    lisp_value result = T_VAL;
    for (uint32_t i = 0; i < node->count; i++) {
        result = node->kids[i]->run(node->kids[i], env);
        if (nilp(result)) {
            mv_return1(NIL_VAL);
            return NIL_VAL;
        }
    }
    return result;
}

static lisp_value run_or(const struct cnode *node, struct lisp_frame *env)
{
    for (uint32_t i = 0; i < node->count; i++) {
        lisp_value result = node->kids[i]->run(node->kids[i], env);
        if (truthy(result))
            return result;
    }
    mv_return1(NIL_VAL);
    return NIL_VAL;
}

static lisp_value run_cond(const struct cnode *node, struct lisp_frame *env)
{
    for (uint32_t i = 0; i < node->count; i += 2) {
        lisp_value test = node->kids[i]->run(node->kids[i], env);
        if (truthy(test)) {
            if (node->kids[i + 1])
                return node->kids[i + 1]->run(node->kids[i + 1], env);
            return test;
        }
    }
    mv_return1(NIL_VAL);
    return NIL_VAL;
}

static lisp_value run_let(const struct cnode *node, struct lisp_frame *env)
{
    int ctl_start = lisp.ctl_top;
    struct lisp_frame *frame = NODE_FRAME(node, env, node->index);
    uint32_t slot = 0;
    for (uint32_t i = 0; i < node->count; i++) {
        lisp_value value = node->kids[i]->run(node->kids[i], node->depth ? frame : env);
        lisp_value name = node->syms[i];
        if (node->dynamic[i]) {
            int ctl = push_ctl(CTL_DYNBIND);
            lisp.ctl[ctl].sym = SYM(name);
            lisp.ctl[ctl].old = SYM(name)->value;
            SYM(name)->value = value;
        } else {
            frame_bind(frame, slot++, name, value, BIND_VAR);
        }
    }
    lisp_value result = node->a->run(node->a, frame);
    restore_dynamic_to(ctl_start);
    return result;
}

static lisp_value place_read(const struct cnode *target, struct lisp_frame *env)
{
    if (target->run == run_lexical) {
        lisp_value value = frame_at(env, target->depth)->entry[target->index].value;
        if (value == VAL(lisp.sym_unbound))
            lisp_error("unbound variable %s", SYM(target->sym)->name);
        return value;
    }
    return variable_value(target->sym, env);
}

static void place_write(const struct cnode *target, lisp_value value, struct lisp_frame *env)
{
    if (target->run == run_lexical) {
        if (SYM(target->sym)->h.flags & (SYM_CONSTANT | SYM_KEYWORD))
            lisp_error("cannot assign to constant %s", SYM(target->sym)->name);
        struct lisp_frame *frame = frame_at(env, target->depth);
        frame->entry[target->index].value = heap_store(frame, value);
        return;
    }
    set_variable(target->sym, value, env);
}

static lisp_value run_setq(const struct cnode *node, struct lisp_frame *env)
{
    lisp_value result = NIL_VAL;
    for (uint32_t i = 0; i < node->count; i++) {
        const struct cnode *target = node->kids[2 * i];
        const struct cnode *value_node = node->kids[2 * i + 1];
        lisp_value value = value_node->run(value_node, env);
        place_write(target, value, env);
        result = value;
    }
    mv_return1(result);
    return result;
}

static lisp_value run_setf_vars(const struct cnode *node, struct lisp_frame *env)
{
    lisp_value result = NIL_VAL;
    for (uint32_t i = 0; i < node->count; i++) {
        const struct cnode *target = node->kids[2 * i];
        const struct cnode *value_node = node->kids[2 * i + 1];
        lisp_value value = value_node->run(value_node, env);
        int count = lisp.mv_count;
        lisp_value captured[LISP_MAX_MV];
        for (int k = 0; k < count && k < LISP_MAX_MV; k++)
            captured[k] = lisp.mv[k];
        place_write(target, value, env);
        result = value;
        if (count > 0) {
            lisp.mv_count = count;
            for (int k = 0; k < count; k++)
                lisp.mv[k] = captured[k];
        }
    }
    return result;
}

static lisp_value run_push(const struct cnode *node, struct lisp_frame *env)
{
    lisp_value item = node->b->run(node->b, env);
    lisp_value fresh = cons(item, place_read(node->a, env));
    place_write(node->a, fresh, env);
    mv_return1(fresh);
    return fresh;
}

static lisp_value run_incf(const struct cnode *node, struct lisp_frame *env)
{
    lisp_value delta = node->b ? node->b->run(node->b, env) : make_int(1);
    lisp_value current = place_read(node->a, env);
    lisp_value value = node->index ? number_sub(current, delta) : number_add(current, delta);
    place_write(node->a, value, env);
    mv_return1(value);
    return value;
}

static lisp_value run_lambda(const struct cnode *node, struct lisp_frame *env)
{
    lisp_value fn = make_closure(CAR(node->sym), CDR(node->sym), env, NIL_VAL);
    if (node->code && lisp.cur_region && lisp.cur_region->kind == REG_ARENA) {
        struct lisp_function *f = PTR(fn, lisp_function);
        f->code = node->code;
        f->info = node->code->info;
        f->slots = node->code->slots;
    }
    mv_return1(fn);
    return fn;
}

static lisp_value run_function(const struct cnode *node, struct lisp_frame *env)
{
    bool found = false;
    lisp_value fn = function_value(node->sym, env, &found);
    if (!found || nilp(fn))
        lisp_error("undefined function %s", SYM(node->sym)->name);
    mv_return1(fn);
    return fn;
}

static lisp_value loop_escape(int ctl)
{
    lisp.ctl_top = ctl;
    int escaped = lisp_escape_count();
    const lisp_value *values = lisp_escape_values_ptr();
    lisp.mv_count = escaped > 0 ? escaped : 1;
    for (int i = 0; i < lisp.mv_count; i++)
        lisp.mv[i] = i < escaped ? values[i] : NIL_VAL;
    return lisp.mv[0];
}

static lisp_value dolist_body(const struct cnode *node, struct lisp_frame *frame, lisp_value list)
{
    while (consp(list)) {
        check_steps();
        frame_bind(frame, 0, node->sym, CAR(list), BIND_VAR);
        node->b->run(node->b, frame);
        list = CDR(list);
    }
    if (node->c)
        return node->c->run(node->c, frame);
    mv_return1(NIL_VAL);
    return NIL_VAL;
}

static lisp_value run_dolist(const struct cnode *node, struct lisp_frame *env)
{
    lisp_value list = node->a->run(node->a, env);
    struct lisp_frame *frame = NODE_FRAME(node, env, 1);
    if (!node->index)
        return dolist_body(node, frame, list);
    int ctl = push_ctl(CTL_BLOCK);
    lisp.ctl[ctl].name = NIL_VAL;
    if (setjmp(lisp.ctl[ctl].jb) != 0)
        return loop_escape(ctl);
    while (consp(list)) {
        check_steps();
        frame_bind(frame, 0, node->sym, CAR(list), BIND_VAR);
        node->b->run(node->b, frame);
        list = CDR(list);
    }
    lisp.ctl_top = ctl;
    if (node->c)
        return node->c->run(node->c, frame);
    mv_return1(NIL_VAL);
    return NIL_VAL;
}

static lisp_value run_dotimes(const struct cnode *node, struct lisp_frame *env)
{
    int64_t count = require_int(node->a->run(node->a, env), "dotimes count");
    struct lisp_frame *frame = NODE_FRAME(node, env, 1);
    int ctl = -1;
    if (node->index) {
        ctl = push_ctl(CTL_BLOCK);
        lisp.ctl[ctl].name = NIL_VAL;
        if (setjmp(lisp.ctl[ctl].jb) != 0)
            return loop_escape(ctl);
    }
    for (int64_t i = 0; i < count; i++) {
        check_steps();
        frame_bind(frame, 0, node->sym, make_int(i), BIND_VAR);
        node->b->run(node->b, frame);
    }
    if (ctl >= 0)
        lisp.ctl_top = ctl;
    if (node->c)
        return node->c->run(node->c, frame);
    mv_return1(NIL_VAL);
    return NIL_VAL;
}

static lisp_value run_block(const struct cnode *node, struct lisp_frame *env)
{
    int ctl = push_ctl(CTL_BLOCK);
    lisp.ctl[ctl].name = node->sym;
    if (setjmp(lisp.ctl[ctl].jb) != 0)
        return loop_escape(ctl);
    lisp_value result = node->a->run(node->a, env);
    lisp.ctl_top = ctl;
    return result;
}

static lisp_value run_return_from(const struct cnode *node, struct lisp_frame *env)
{
    lisp_value buffer[LISP_MAX_MV];
    int count = 1;
    buffer[0] = NIL_VAL;
    if (node->a) {
        node->a->run(node->a, env);
        count = lisp.mv_count > 0 ? lisp.mv_count : 1;
        if (count > LISP_MAX_MV)
            count = LISP_MAX_MV;
        for (int i = 0; i < count; i++)
            buffer[i] = lisp.mv[i];
    }
    throw_return(node->sym, count, buffer);
}

static lisp_value run_return(const struct cnode *node, struct lisp_frame *env)
{
    node->a->run(node->a, env);
    int count = lisp.mv_count > 0 ? lisp.mv_count : 1;
    lisp_value buffer[LISP_MAX_MV];
    for (int i = 0; i < count; i++)
        buffer[i] = lisp.mv[i];
    throw_return(NIL_VAL, count, buffer);
}

static lisp_value run_case(const struct cnode *node, struct lisp_frame *env)
{
    lisp_value key = node->a->run(node->a, env);
    for (uint32_t i = 0; i < node->count; i++) {
        lisp_value spec = node->syms[i];
        if (spec == T_VAL || eql_value(spec, VAL(lisp.sym_otherwise)) || case_match(key, spec))
            return node->kids[i]->run(node->kids[i], env);
    }
    if (node->index) {
        char shown[160];
        char whole[400];
        lisp_print(key, true, shown, sizeof(shown));
        lisp_print(node->form, true, whole, sizeof(whole));
        lisp_error("ECASE %s has no clause for %s", whole, shown);
    }
    mv_return1(NIL_VAL);
    return NIL_VAL;
}

static lisp_value run_mvbind(const struct cnode *node, struct lisp_frame *env)
{
    node->a->run(node->a, env);
    int count = lisp.mv_count;
    lisp_value captured[LISP_MAX_MV];
    for (int i = 0; i < count; i++)
        captured[i] = lisp.mv[i];
    struct lisp_frame *frame = NODE_FRAME(node, env, node->count);
    for (uint32_t slot = 0; slot < node->count; slot++)
        frame_bind(frame, slot, node->syms[slot], (int)slot < count ? captured[slot] : NIL_VAL, BIND_VAR);
    return node->b->run(node->b, frame);
}

static lisp_value run_destructure(const struct cnode *node, struct lisp_frame *env)
{
    lisp_value value = node->a->run(node->a, env);
    struct lisp_frame *frame = frame_new(env, node->index + 1);
    destructure_bind(frame, node->sym, value);
    return node->b->run(node->b, frame);
}

static lisp_value run_prog1(const struct cnode *node, struct lisp_frame *env)
{
    lisp_value first = node->a->run(node->a, env);
    int count = lisp.mv_count;
    lisp_value captured[LISP_MAX_MV];
    for (int i = 0; i < count; i++)
        captured[i] = lisp.mv[i];
    node->b->run(node->b, env);
    for (int i = 0; i < count; i++)
        lisp.mv[i] = captured[i];
    lisp.mv_count = count;
    return first;
}

static lisp_value run_mvlist(const struct cnode *node, struct lisp_frame *env)
{
    node->a->run(node->a, env);
    int count = lisp.mv_count;
    lisp_value out = NIL_VAL;
    for (int i = count - 1; i >= 0; i--)
        out = cons(lisp.mv[i], out);
    mv_return1(out);
    return out;
}

static lisp_value run_check_type(const struct cnode *node, struct lisp_frame *env)
{
    lisp_value value = node->a->run == run_lexical ? place_read(node->a, env) : node->a->run(node->a, env);
    if (!lisp_type_check(value, node->sym, env))
        lisp_error("The value %s is not of type %s", "value", "type");
    mv_return1(NIL_VAL);
    return NIL_VAL;
}

static lisp_value run_setf(const struct cnode *node, struct lisp_frame *env)
{
    lisp_value result = NIL_VAL;
    for (uint32_t i = 0; i < node->count; i++) {
        struct lisp_place place;
        eval_place(node->syms[i], env, &place);
        lisp_value value = node->kids[i]->run(node->kids[i], env);
        int count = lisp.mv_count;
        lisp_value captured[LISP_MAX_MV];
        for (int k = 0; k < count && k < LISP_MAX_MV; k++)
            captured[k] = lisp.mv[k];
        place_set(&place, value);
        result = value;
        if (count > 0) {
            lisp.mv_count = count;
            for (int k = 0; k < count; k++)
                lisp.mv[k] = captured[k];
        }
    }
    return result;
}

static lisp_value run_flet(const struct cnode *node, struct lisp_frame *env)
{
    struct lisp_frame *frame = frame_new(env, node->count + 1);
    bool arena = lisp.cur_region && lisp.cur_region->kind == REG_ARENA;
    for (uint32_t i = 0; i < node->count; i++) {
        const struct cnode *def = node->kids[i];
        lisp_value fn = make_closure(CAR(def->sym), CDR(def->sym), node->index ? frame : env, node->syms[i]);
        if (def->code && arena) {
            struct lisp_function *f = PTR(fn, lisp_function);
            f->code = def->code;
            f->info = def->code->info;
            f->slots = def->code->slots;
        }
        frame_bind(frame, i, node->syms[i], fn, BIND_FUNC);
    }
    return node->a->run(node->a, frame);
}

static bool proper_list(lisp_value list, uint32_t *count)
{
    uint32_t n = 0;
    while (consp(list)) {
        n++;
        list = CDR(list);
    }
    *count = n;
    return nilp(list);
}

static bool tree_has_return(lisp_value form, int depth)
{
    while (consp(form) && depth < 256) {
        lisp_value head = CAR(form);
        if (consp(head) && tree_has_return(head, depth + 1))
            return true;
        if (symbolp(head)) {
            uint32_t id = SYM(head)->special_id;
            if (id == SF_RETURN || id == SF_RETURN_FROM || id == SF_EVAL || (SYM(head)->h.flags & SYM_MACRO))
                return true;
        }
        form = CDR(form);
    }
    return depth >= 256;
}

static const struct cnode *fallback(struct compiler *cc, lisp_value form)
{
    return node_new(cc, run_eval, form);
}

static const struct cnode *compile_variable(struct compiler *cc, lisp_value sym, struct cscope *scope)
{
    struct lisp_symbol *s = SYM(sym);
    if (s->h.flags & SYM_SPECIAL) {
        struct cnode *node = node_new(cc, run_special, sym);
        node->sym = sym;
        return node;
    }
    uint32_t depth = 0, index = 0;
    if (scope_lookup(scope, sym, &depth, &index) == LOOKUP_LEXICAL) {
        struct cnode *node = node_new(cc, run_lexical, sym);
        node->sym = sym;
        node->depth = depth;
        node->index = index;
        return node;
    }
    struct cnode *node = node_new(cc, run_variable, sym);
    node->sym = sym;
    return node;
}

static const struct cnode *compile_body(struct compiler *cc, lisp_value body, struct cscope *scope)
{
    uint32_t count = 0;
    if (!proper_list(body, &count))
        return NULL;
    if (count == 1)
        return compile_form(cc, CAR(body), scope);
    struct cnode *node = node_new(cc, run_progn, body);
    node->count = count;
    node->kids = kids_new(cc, count);
    uint32_t i = 0;
    for (lisp_value scan = body; consp(scan); scan = CDR(scan)) {
        const struct cnode *kid = compile_form(cc, CAR(scan), scope);
        if (!kid)
            return NULL;
        node->kids[i++] = kid;
    }
    return node;
}

static struct cnode *compile_list(struct compiler *cc, cnode_run run, lisp_value form,
    lisp_value items, struct cscope *scope)
{
    uint32_t count = 0;
    if (!proper_list(items, &count))
        return NULL;
    struct cnode *node = node_new(cc, run, form);
    node->count = count;
    node->kids = kids_new(cc, count);
    uint32_t i = 0;
    for (lisp_value scan = items; consp(scan); scan = CDR(scan)) {
        const struct cnode *kid = compile_form(cc, CAR(scan), scope);
        if (!kid)
            return NULL;
        node->kids[i++] = kid;
    }
    return node;
}

static const struct cnode *compile_let(struct compiler *cc, lisp_value form, struct cscope *scope, bool star)
{
    lisp_value bindings = CAR(CDR(form));
    uint32_t count = 0;
    if (!proper_list(bindings, &count) || count > 256)
        return NULL;
    struct cnode *node = node_new(cc, run_let, form);
    node->count = count;
    node->depth = star;
    node->stack = cc->stack;
    node->kids = kids_new(cc, count);
    node->syms = count ? heap_alloc_in(cc->region, sizeof(lisp_value) * count) : NULL;
    node->dynamic = count ? heap_alloc_in(cc->region, count) : NULL;
    lisp_value *slots = count ? heap_alloc_in(cc->region, sizeof(lisp_value) * count) : NULL;
    struct cscope inner = { scope, true, false, 0, slots };
    uint32_t slot_count = 0;
    uint32_t i = 0;
    for (lisp_value b = bindings; consp(b); b = CDR(b), i++) {
        lisp_value binding = CAR(b);
        lisp_value name = binding;
        lisp_value init = NIL_VAL;
        if (consp(binding)) {
            name = CAR(binding);
            if (consp(CDR(binding)))
                init = CAR(CDR(binding));
        }
        if (!symbolp(name))
            return NULL;
        const struct cnode *kid = compile_form(cc, init, star ? &inner : scope);
        if (!kid)
            return NULL;
        node->kids[i] = kid;
        node->syms[i] = name;
        node->dynamic[i] = (SYM(name)->h.flags & SYM_SPECIAL) != 0;
        if (!node->dynamic[i]) {
            slots[slot_count++] = name;
            if (star)
                inner.count = slot_count;
        }
    }
    inner.count = slot_count;
    node->index = slot_count;
    node->a = compile_body(cc, CDR(CDR(form)), &inner);
    return node->a ? node : NULL;
}

static const struct cnode *compile_var_target(struct compiler *cc, lisp_value sym, struct cscope *scope)
{
    if (!symbolp(sym))
        return NULL;
    struct cnode *node = node_new(cc, run_variable, sym);
    node->sym = sym;
    uint32_t depth = 0, index = 0;
    if (!(SYM(sym)->h.flags & SYM_SPECIAL) && scope_lookup(scope, sym, &depth, &index) == LOOKUP_LEXICAL) {
        node->run = run_lexical;
        node->depth = depth;
        node->index = index;
    }
    return node;
}

static const struct cnode *compile_assign(struct compiler *cc, cnode_run run, lisp_value form,
    struct cscope *scope)
{
    lisp_value args = CDR(form);
    uint32_t count = 0;
    if (!proper_list(args, &count) || count % 2)
        return NULL;
    struct cnode *node = node_new(cc, run, form);
    node->count = count / 2;
    node->kids = kids_new(cc, count);
    uint32_t i = 0;
    for (lisp_value p = args; consp(p); p = CDR(CDR(p))) {
        const struct cnode *target = compile_var_target(cc, CAR(p), scope);
        const struct cnode *value = target ? compile_form(cc, CAR(CDR(p)), scope) : NULL;
        if (!value)
            return NULL;
        node->kids[i++] = target;
        node->kids[i++] = value;
    }
    return node;
}

static const struct cnode *compile_lambda_node(struct compiler *cc, lisp_value form, lisp_value lambda,
    struct cscope *outer)
{
    if (!consp(lambda) || !consp(CDR(lambda)))
        return NULL;
    struct cnode *node = node_new(cc, run_lambda, form);
    node->sym = CDR(lambda);
    struct lisp_function probe;
    memset(&probe, 0, sizeof(probe));
    probe.params = CAR(CDR(lambda));
    probe.body = CDR(CDR(lambda));
    lisp_closure_info(&probe);
    node->code = compile_lambda(cc, probe.params, probe.body, probe.info, probe.slots, outer);
    return node;
}

static const struct cnode *compile_loop_var(struct compiler *cc, cnode_run run, lisp_value form,
    struct cscope *scope)
{
    lisp_value args = CDR(form);
    lisp_value spec = CAR(args);
    if (!consp(spec) || !symbolp(CAR(spec)) || !consp(CDR(spec)))
        return NULL;
    lisp_value var = CAR(spec);
    if (SYM(var)->h.flags & SYM_SPECIAL)
        return NULL;
    struct cnode *node = node_new(cc, run, form);
    node->sym = var;
    node->stack = cc->stack;
    node->index = tree_has_return(CDR(args), 0);
    node->a = compile_form(cc, CAR(CDR(spec)), scope);
    lisp_value *slot = heap_alloc_in(cc->region, sizeof(lisp_value));
    slot[0] = var;
    struct cscope inner = { scope, true, false, 1, slot };
    node->b = compile_body(cc, CDR(args), &inner);
    if (!node->a || !node->b)
        return NULL;
    if (consp(CDR(CDR(spec)))) {
        struct cscope opaque = { scope, true, true, 0, NULL };
        node->c = compile_form(cc, CAR(CDR(CDR(spec))), &opaque);
        if (!node->c)
            return NULL;
    }
    return node;
}

static const struct cnode *compile_special(struct compiler *cc, lisp_value form, struct cscope *scope,
    uint32_t id)
{
    lisp_value args = CDR(form);
    switch (id) {
    case SF_QUOTE: {
        struct cnode *node = node_new(cc, run_const, form);
        node->sym = CAR(args);
        return node;
    }
    case SF_FUNCTION: {
        lisp_value target = CAR(args);
        if (consp(target) && CAR(target) == VAL(lisp.sym_quote))
            return NULL;
        if (symbolp(target)) {
            struct cnode *node = node_new(cc, run_function, form);
            node->sym = target;
            return node;
        }
        if (consp(target) && symbolp(CAR(target)) && SYM(CAR(target))->special_id == SF_LAMBDA)
            return compile_lambda_node(cc, form, target, scope);
        return NULL;
    }
    case SF_LAMBDA:
        return compile_lambda_node(cc, form, form, scope);
    case SF_IF: {
        struct cnode *node = node_new(cc, run_if, form);
        node->a = compile_form(cc, CAR(args), scope);
        node->b = compile_form(cc, CAR(CDR(args)), scope);
        if (consp(CDR(CDR(args))))
            node->c = compile_form(cc, CAR(CDR(CDR(args))), scope);
        if (!node->a || !node->b || (consp(CDR(CDR(args))) && !node->c))
            return NULL;
        return node;
    }
    case SF_WHEN:
    case SF_UNLESS: {
        struct cnode *node = node_new(cc, run_when, form);
        node->index = id == SF_UNLESS;
        node->a = compile_form(cc, CAR(args), scope);
        node->b = compile_body(cc, CDR(args), scope);
        return node->a && node->b ? node : NULL;
    }
    case SF_PROGN:
        return compile_list(cc, run_progn, form, args, scope);
    case SF_AND:
        return compile_list(cc, run_and, form, args, scope);
    case SF_OR:
        return compile_list(cc, run_or, form, args, scope);
    case SF_COND: {
        uint32_t count = 0;
        if (!proper_list(args, &count))
            return NULL;
        struct cnode *node = node_new(cc, run_cond, form);
        node->count = count * 2;
        node->kids = kids_new(cc, count * 2);
        uint32_t i = 0;
        for (lisp_value c = args; consp(c); c = CDR(c)) {
            lisp_value clause = CAR(c);
            if (!consp(clause))
                return NULL;
            node->kids[i] = compile_form(cc, CAR(clause), scope);
            if (!node->kids[i])
                return NULL;
            node->kids[i + 1] = NULL;
            if (consp(CDR(clause))) {
                node->kids[i + 1] = compile_body(cc, CDR(clause), scope);
                if (!node->kids[i + 1])
                    return NULL;
            }
            i += 2;
        }
        return node;
    }
    case SF_LET:
        return compile_let(cc, form, scope, false);
    case SF_LET_STAR:
        return compile_let(cc, form, scope, true);
    case SF_SETQ:
        return compile_assign(cc, run_setq, form, scope);
    case SF_SETF: {
        uint32_t count = 0;
        if (!proper_list(args, &count) || count % 2)
            return NULL;
        bool symbols = true;
        for (lisp_value p = args; consp(p); p = CDR(CDR(p)))
            symbols = symbols && symbolp(CAR(p));
        if (symbols)
            return compile_assign(cc, run_setf_vars, form, scope);
        struct cnode *node = node_new(cc, run_setf, form);
        node->count = count / 2;
        node->kids = kids_new(cc, node->count);
        node->syms = heap_alloc_in(cc->region, sizeof(lisp_value) * node->count);
        uint32_t i = 0;
        for (lisp_value p = args; consp(p); p = CDR(CDR(p)), i++) {
            node->syms[i] = CAR(p);
            node->kids[i] = compile_form(cc, CAR(CDR(p)), scope);
            if (!node->kids[i])
                return NULL;
        }
        return node;
    }
    case SF_PUSH: {
        lisp_value place = CAR(CDR(args));
        if (!symbolp(place))
            return NULL;
        struct cnode *node = node_new(cc, run_push, form);
        node->b = compile_form(cc, CAR(args), scope);
        node->a = compile_var_target(cc, place, scope);
        return node->a && node->b ? node : NULL;
    }
    case SF_INCF:
    case SF_DECF: {
        if (!symbolp(CAR(args)))
            return NULL;
        struct cnode *node = node_new(cc, run_incf, form);
        node->index = id == SF_DECF;
        node->a = compile_var_target(cc, CAR(args), scope);
        if (consp(CDR(args))) {
            node->b = compile_form(cc, CAR(CDR(args)), scope);
            if (!node->b)
                return NULL;
        }
        return node->a ? node : NULL;
    }
    case SF_DECLARE:
    case SF_DECLAIM: {
        struct cnode *node = node_new(cc, run_const, form);
        node->sym = NIL_VAL;
        return node;
    }
    case SF_DOLIST:
        return compile_loop_var(cc, run_dolist, form, scope);
    case SF_DOTIMES:
        return compile_loop_var(cc, run_dotimes, form, scope);
    case SF_BLOCK: {
        struct cnode *node = node_new(cc, run_block, form);
        node->sym = CAR(args);
        node->a = compile_body(cc, CDR(args), scope);
        return node->a ? node : NULL;
    }
    case SF_RETURN_FROM: {
        struct cnode *node = node_new(cc, run_return_from, form);
        node->sym = CAR(args);
        lisp_value value_form = consp(CDR(args)) ? CAR(CDR(args)) : NIL_VAL;
        if (!nilp(value_form)) {
            node->a = compile_form(cc, value_form, scope);
            if (!node->a)
                return NULL;
        }
        return node;
    }
    case SF_RETURN: {
        struct cnode *node = node_new(cc, run_return, form);
        node->a = compile_form(cc, consp(args) ? CAR(args) : NIL_VAL, scope);
        return node->a ? node : NULL;
    }
    case SF_CASE:
    case SF_ECASE: {
        uint32_t count = 0;
        if (!proper_list(CDR(args), &count))
            return NULL;
        struct cnode *node = node_new(cc, run_case, form);
        node->index = id == SF_ECASE;
        node->a = compile_form(cc, CAR(args), scope);
        if (!node->a)
            return NULL;
        node->count = count;
        node->kids = kids_new(cc, count);
        node->syms = count ? heap_alloc_in(cc->region, sizeof(lisp_value) * count) : NULL;
        uint32_t i = 0;
        for (lisp_value c = CDR(args); consp(c); c = CDR(c), i++) {
            lisp_value clause = CAR(c);
            if (!consp(clause))
                return NULL;
            node->syms[i] = CAR(clause);
            node->kids[i] = compile_body(cc, CDR(clause), scope);
            if (!node->kids[i])
                return NULL;
        }
        return node;
    }
    case SF_MULTIPLE_VALUE_BIND: {
        lisp_value vars = CAR(args);
        uint32_t count = 0;
        if (!proper_list(vars, &count) || count > LISP_MAX_MV * 4)
            return NULL;
        lisp_value *slots = count ? heap_alloc_in(cc->region, sizeof(lisp_value) * count) : NULL;
        uint32_t i = 0;
        for (lisp_value v = vars; consp(v); v = CDR(v)) {
            if (!symbolp(CAR(v)) || (SYM(CAR(v))->h.flags & SYM_SPECIAL))
                return NULL;
            slots[i++] = CAR(v);
        }
        struct cnode *node = node_new(cc, run_mvbind, form);
        node->count = count;
        node->syms = slots;
        node->stack = cc->stack;
        node->a = compile_form(cc, CAR(CDR(args)), scope);
        struct cscope inner = { scope, true, false, count, slots };
        node->b = compile_body(cc, CDR(CDR(args)), &inner);
        return node->a && node->b ? node : NULL;
    }
    case SF_DESTRUCTURING_BIND: {
        struct cnode *node = node_new(cc, run_destructure, form);
        node->sym = CAR(args);
        node->index = pattern_slot_count(CAR(args));
        node->a = compile_form(cc, CAR(CDR(args)), scope);
        struct cscope opaque = { scope, true, true, 0, NULL };
        node->b = compile_body(cc, CDR(CDR(args)), &opaque);
        return node->a && node->b ? node : NULL;
    }
    case SF_PROG1: {
        struct cnode *node = node_new(cc, run_prog1, form);
        node->a = compile_form(cc, CAR(args), scope);
        node->b = compile_list(cc, run_progn, form, CDR(args), scope);
        return node->a && node->b ? node : NULL;
    }
    case SF_CHECK_TYPE: {
        lisp_value target = CAR(args);
        if (consp(target) && lisp_place_head(CAR(target)))
            return NULL;
        struct cnode *node = node_new(cc, run_check_type, form);
        node->sym = CAR(CDR(args));
        node->a = symbolp(target) ? compile_var_target(cc, target, scope) : compile_form(cc, target, scope);
        return node->a ? node : NULL;
    }
    case SF_FLET:
    case SF_LABELS: {
        lisp_value bindings = CAR(args);
        uint32_t count = 0;
        if (!proper_list(bindings, &count))
            return NULL;
        struct cnode *node = node_new(cc, run_flet, form);
        node->index = id == SF_LABELS;
        node->count = count;
        node->kids = kids_new(cc, count);
        node->syms = count ? heap_alloc_in(cc->region, sizeof(lisp_value) * count) : NULL;
        struct cscope inner = { scope, true, false, 0, NULL };
        uint32_t i = 0;
        for (lisp_value b = bindings; consp(b); b = CDR(b), i++) {
            lisp_value binding = CAR(b);
            if (!consp(binding) || !symbolp(CAR(binding)) || !consp(CDR(binding)))
                return NULL;
            node->syms[i] = CAR(binding);
            node->kids[i] = compile_lambda_node(cc, binding, binding, node->index ? &inner : scope);
            if (!node->kids[i])
                return NULL;
        }
        node->a = compile_body(cc, CDR(args), &inner);
        return node->a ? node : NULL;
    }
    case SF_MULTIPLE_VALUE_LIST: {
        struct cnode *node = node_new(cc, run_mvlist, form);
        node->a = compile_form(cc, CAR(args), scope);
        return node->a ? node : NULL;
    }
    default:
        return NULL;
    }
}

static const struct cnode *compile_form(struct compiler *cc, lisp_value form, struct cscope *scope)
{
    if (TYPE(form) == LT_SYMBOL)
        return compile_variable(cc, form, scope);
    if (TYPE(form) != LT_CONS) {
        struct cnode *node = node_new(cc, run_const, form);
        node->sym = form;
        return node;
    }
    if (++cc->depth > 200) {
        cc->depth--;
        return fallback(cc, form);
    }
    const struct cnode *node = NULL;
    lisp_value head = CAR(form);
    if (symbolp(head)) {
        struct lisp_symbol *sym = SYM(head);
        bool local = sym->h.flags & SYM_LOCAL_FUNC;
        if (sym->special_id && !local)
            node = compile_special(cc, form, scope, sym->special_id);
        else if (!sym->special_id && !(sym->h.flags & SYM_MACRO)) {
            struct cnode *call = compile_list(cc, run_call, form, CDR(form), scope);
            if (call) {
                call->sym = head;
                const struct wrapper *wrapper = local ? NULL : wrapper_for(cc->region, sym->function);
                if (wrapper) {
                    call->run = run_wrapper;
                    call->wrapper = wrapper;
                }
                node = call;
            }
        }
    }
    cc->depth--;
    return node ? node : fallback(cc, form);
}

static bool parameter_name(lisp_value *names, uint32_t slots, uint32_t *count, lisp_value name)
{
    if (!symbolp(name) || *count >= slots)
        return false;
    names[(*count)++] = name;
    return true;
}

static bool lambda_slot_names(lisp_value params, lisp_value *names, uint32_t slots, uint32_t *count)
{
    bool optional = false, key = false, rest = false;
    for (lisp_value spec = params; consp(spec); spec = CDR(spec)) {
        lisp_value item = CAR(spec);
        if (item == VAL(lisp.sym_optional)) {
            optional = true;
            continue;
        }
        if (item == VAL(lisp.sym_key)) {
            key = true;
            continue;
        }
        if (item == VAL(lisp.sym_rest) || item == VAL(lisp.sym_body)) {
            rest = true;
            continue;
        }
        if (item == VAL(lisp.sym_allow_other_keys))
            continue;
        if (item == VAL(lisp.sym_aux) || item == VAL(lisp.sym_whole) ||
                item == VAL(lisp.sym_environment))
            return false;
        if (rest) {
            if (!parameter_name(names, slots, count, item))
                return false;
            rest = false;
            continue;
        }
        if (!key && !optional) {
            if (!parameter_name(names, slots, count, item))
                return false;
            continue;
        }
        lisp_value var = item;
        if (consp(item)) {
            var = CAR(item);
            if (key && consp(var))
                var = consp(CDR(var)) ? CAR(CDR(var)) : NIL_VAL;
        }
        if (!parameter_name(names, slots, count, var))
            return false;
        if (consp(item) && consp(CDR(item)) && consp(CDR(CDR(item))) &&
                symbolp(CAR(CDR(CDR(item)))) &&
                !parameter_name(names, slots, count, CAR(CDR(CDR(item)))))
            return false;
    }
    return true;
}

static const struct lisp_code *compile_lambda(struct compiler *cc, lisp_value params, lisp_value body,
    uint8_t info, uint32_t slots, struct cscope *outer)
{
    struct compiler inner = { cc->region, !(info & FNI_CAPTURES), cc->depth };
    lisp_value *names = slots ? heap_alloc_in(cc->region, sizeof(lisp_value) * slots) : NULL;
    struct cscope scope = { outer, true, !(info & FNI_SIMPLE), 0, names };
    if (info & FNI_SIMPLE) {
        for (lisp_value p = params; consp(p) && scope.count < slots; p = CDR(p))
            names[scope.count++] = CAR(p);
    } else if (lambda_slot_names(params, names, slots, &scope.count)) {
        scope.barrier = false;
    } else {
        scope.count = 0;
    }
    const struct cnode *code = compile_body(&inner, body, &scope);
    if (!code)
        return NULL;
    struct lisp_code *out = heap_alloc_in(cc->region, sizeof(*out));
    out->body = code;
    out->wrapper = NULL;
    out->info = info;
    out->slots = slots;
    return out;
}

void lisp_compile_closure(struct lisp_function *fn)
{
    struct lisp_region *region = fn->gen;
    if (!region || region->kind != REG_GENERATION || !region->in_use)
        return;
    struct compiler cc = { region, false, 0 };
    const struct lisp_code *code = compile_lambda(&cc, fn->params, fn->body, fn->info, fn->slots, NULL);
    if (code)
        ((struct lisp_code *)code)->wrapper = wrapper_for(region, VAL(fn));
    fn->code = code;
    fn->info |= FNI_COMPILED;
}

bool lisp_run_wrapper(const struct lisp_code *code, int argc, lisp_value *argv, lisp_value *result)
{
    const struct wrapper *w = code->wrapper;
    if (!w || (uint32_t)argc != w->params || w->callee->function != w->builtin)
        return false;
    lisp_value args[WRAPPER_ARGS];
    for (uint32_t i = 0; i < w->count; i++)
        args[i] = w->source[i] < 0 ? w->constant[i] : argv[w->source[i]];
    *result = apply_fast(w->builtin, (int)w->count, args);
    return true;
}

enum { FORM_CACHE = 1024 };

static struct {
    lisp_value form;
    struct lisp_region *region;
    const struct cnode *node;
} form_cache[FORM_CACHE];

lisp_value lisp_eval_cached(lisp_value form, struct lisp_frame *env)
{
    if (TYPE(form) != LT_CONS)
        return eval(form, env);
    struct lisp_region *region = heap_region_of((void *)(uintptr_t)form);
    if (!region || region->kind != REG_GENERATION)
        return eval(form, env);
    size_t slot = (size_t)((((uint64_t)form >> 4) * 0x9e3779b97f4a7c15ull) >> 54) & (FORM_CACHE - 1);
    if (form_cache[slot].form != form || form_cache[slot].region != region) {
        struct compiler cc = { region, false, 0 };
        struct cscope opaque = { NULL, true, true, 0, NULL };
        const struct cnode *node = compile_form(&cc, form, &opaque);
        if (!node)
            return eval(form, env);
        form_cache[slot].form = form;
        form_cache[slot].region = region;
        form_cache[slot].node = node;
    }
    return form_cache[slot].node->run(form_cache[slot].node, env);
}

void lisp_forget_forms(struct lisp_region *region)
{
    for (size_t i = 0; i < FORM_CACHE; i++)
        if (form_cache[i].region == region)
            form_cache[i].form = 0, form_cache[i].region = NULL, form_cache[i].node = NULL;
}

lisp_value lisp_run_code(const struct lisp_code *code, struct lisp_frame *frame)
{
    return code->body->run(code->body, frame);
}
