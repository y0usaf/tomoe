#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <setjmp.h>

#include "lisp-internal.h"

enum {
    VM_STACK_SIZE = 65536,
    VM_FRAMES_SIZE = 2048,
    VM_MV_SAVE_SIZE = 256,
    CC_MAX_INS = 16384,
    CC_MAX_CONSTS = 1024,
    CC_MAX_PROTOS = 256,
    CC_MAX_LOCALS = 256,
    CC_MAX_UPVALS = 128,
    CC_MAX_LOOPS = 64,
    CC_MAX_BREAKS = 128,
    CC_MAX_MUTATED = 256
};

enum vm_op {
    OP_NOP = 0,
    OP_POP,
    OP_DUP,
    OP_NIL,
    OP_T,
    OP_CONST,
    OP_GET_LOCAL,
    OP_SET_LOCAL,
    OP_SET_LOCAL_POP,
    OP_BOX_LOCAL,
    OP_GET_BOX,
    OP_SET_BOX,
    OP_GET_UPVAL,
    OP_GET_UPVAL_BOX,
    OP_SET_UPVAL_BOX,
    OP_GET_SPECIAL,
    OP_SET_SPECIAL,
    OP_GET_FUNC,
    OP_BIND_DYN,
    OP_UNBIND_DYN,
    OP_JMP,
    OP_JMP_NIL,
    OP_JMP_NOT_NIL,
    OP_JMP_NIL_KEEP,
    OP_JMP_NOT_NIL_KEEP,
    OP_MV1,
    OP_VALUES,
    OP_MV_BIND,
    OP_MV_LIST,
    OP_MV_SAVE,
    OP_MV_RESTORE,
    OP_RET,
    OP_RET_ONE,
    OP_CALL_SYM,
    OP_TAIL_CALL_SYM,
    OP_FUNCALL,
    OP_APPLY,
    OP_MAKE_CLOSURE,
    OP_CAR,
    OP_CDR,
    OP_CDDR,
    OP_CAAR,
    OP_CDAR,
    OP_SECOND,
    OP_THIRD,
    OP_FOURTH,
    OP_FIFTH,
    OP_CONS,
    OP_LIST,
    OP_NTH,
    OP_NTH_IMM,
    OP_NTHCDR,
    OP_LAST,
    OP_LENGTH,
    OP_GETF,
    OP_GETF_CONST,
    OP_GETHASH,
    OP_NULL,
    OP_CONSP,
    OP_SYMBOLP,
    OP_LISTP,
    OP_STRINGP,
    OP_NUMBERP,
    OP_INTEGERP,
    OP_KEYWORDP,
    OP_FUNCTIONP,
    OP_EQ,
    OP_EQL,
    OP_EQUAL,
    OP_ADD2,
    OP_SUB2,
    OP_MUL2,
    OP_ADD1,
    OP_SUB1,
    OP_NEG,
    OP_LT2,
    OP_LE2,
    OP_GT2,
    OP_GE2,
    OP_NUMEQ2,
    OP_NUMNE2,
    OP_ZEROP,
    OP_PLUSP,
    OP_MINUSP,
    OP_MIN2,
    OP_MAX2,
    OP_MEMBER2,
    OP_ASSOC2,
    OP_APPEND2,
    OP_STEP,
    OP_LOOP_STEP_IN,
    OP_LOOP_STEP_ON,
    OP_LOOP_STEP_IN_CDDR,
    OP_LOOP_STEP_ON_CDDR,
    OP_LOOP_COLLECT,
    OP_LOOP_APPEND,
    OP_LOOP_NCONC,
    OP_LOOP_ACROSS_LEN,
    OP_LOOP_ACROSS_REF,
    OP_CASE_MATCH,
    OP_ECASE_FAIL,
    OP_CHECK_TYPE,
    OP_RETURN_FROM,
    OP_SETF_GETF,
    OP_SETF_GETHASH,
    OP_SETF_CDR,
    OP_SETF_NTH,
    OP_UNWIND_THUNK,
    OP_IGNORE_ERRORS_THUNK,
    OP_HANDLER_CASE_THUNK,
    OP_BLOCK_THUNK,
    OP_DESTRUCTURE,
    OP_PUSHNEW_EQ,
    OP_CALL_BUILTIN,
    OP_GET_LOCAL_GETF_CONST,
    OP_GET_LOCAL_NTH_IMM,
    OP_EQ_CONST,
    OP_STRING_EQ,
    OP_COPY_LIST,
    OP_NREVERSE,
    OP_REVERSE,
    OP_ACONS,
    OP_MAX_OP
};

#define INST(op, a, b) ((uint32_t)(op) | ((uint32_t)((a) & 0xffu) << 8) | ((uint32_t)((uint16_t)(b)) << 16))
#define OP_OF(i) ((i) & 0xffu)
#define A_OF(i) (((i) >> 8) & 0xffu)
#define B_OF(i) ((uint16_t)((i) >> 16))
#define SB_OF(i) ((int16_t)((i) >> 16))

struct vm_updesc {
    uint8_t is_local;
    uint8_t boxed;
    uint16_t index;
};

struct vm_param_spec {
    uint8_t kind;
    uint8_t slot;
    uint8_t supplied_slot;
    uint8_t has_supplied;
    lisp_value name;
    lisp_value keyword;
    const struct lisp_code *default_code;
};

enum {
    PS_REQ = 0,
    PS_OPT = 1,
    PS_REST = 2,
    PS_KEY = 3
};

struct lisp_code {
    struct lisp_region *gen;
    lisp_value name;
    const uint32_t *ins;
    uint32_t ins_count;
    const lisp_value *consts;
    uint32_t const_count;
    const struct lisp_code *const *protos;
    uint32_t proto_count;
    const struct vm_updesc *updesc;
    uint32_t upval_count;
    const struct vm_param_spec *pspecs;
    uint32_t pspec_count;
    uint16_t min_args;
    uint16_t param_count;
    uint16_t nlocals;
    uint8_t info;
    uint8_t simple;
    uint8_t allow_other_keys;
    uint8_t box_mask_count;
    const uint8_t *box_slots;
};

struct vm_frame {
    const struct lisp_code *code;
    struct lisp_function *fn;
    const uint32_t *pc;
    uint32_t base;
    uint32_t ret_sp;
    int ctl_mark;
};

struct vm_mv_saved {
    int count;
    lisp_value values[LISP_MAX_MV];
};

static lisp_value vm_stack[VM_STACK_SIZE];
static struct vm_frame vm_frames[VM_FRAMES_SIZE];
static struct vm_mv_saved vm_mv_saved[VM_MV_SAVE_SIZE];
static uint32_t vm_sp;
static uint32_t vm_fp;
static uint32_t vm_mv_sp;

uint32_t lisp_vm_sp_get(void) { return vm_sp; }
void lisp_vm_sp_set(uint32_t sp) { vm_sp = sp; }
uint32_t lisp_vm_fp_get(void) { return vm_fp; }
void lisp_vm_fp_set(uint32_t fp) { vm_fp = fp; }

lisp_value lisp_vm_current_name(void)
{
    for (uint32_t i = vm_fp; i-- > 0;) {
        if (vm_frames[i].code && symbolp(vm_frames[i].code->name) && !nilp(vm_frames[i].code->name))
            return vm_frames[i].code->name;
    }
    return NIL_VAL;
}

void lisp_vm_reset(void)
{
    vm_sp = 0;
    vm_fp = 0;
    vm_mv_sp = 0;
}

struct cc_var {
    lisp_value sym;
    uint16_t slot;
    uint8_t kind;
    uint8_t boxed;
};

struct cc_loop {
    int breaks[CC_MAX_BREAKS];
    int break_count;
    int dyn_depth;
    int stack_depth;
};

struct cc_fn {
    struct cc_fn *parent;
    struct lisp_region *region;
    lisp_value name;
    uint32_t *ins;
    uint32_t ins_count;
    lisp_value *consts;
    uint32_t const_count;
    const struct lisp_code **protos;
    uint32_t proto_count;
    struct vm_updesc updesc[CC_MAX_UPVALS];
    uint32_t upval_count;
    lisp_value up_syms[CC_MAX_UPVALS];
    uint8_t up_kinds[CC_MAX_UPVALS];
    struct cc_var vars[CC_MAX_LOCALS];
    uint32_t var_count;
    uint16_t next_slot;
    uint16_t max_slot;
    int dyn_depth;
    int stack_depth;
    struct cc_loop loops[CC_MAX_LOOPS];
    int loop_depth;
    lisp_value mutated[CC_MAX_MUTATED];
    uint32_t mutated_count;
    bool has_inner_closure;
    bool failed;
};

static bool cc_is_mutated(struct cc_fn *cc, lisp_value sym)
{
    for (uint32_t i = 0; i < cc->mutated_count; i++)
        if (cc->mutated[i] == sym)
            return true;
    return false;
}

static void cc_mark_mutated(struct cc_fn *cc, lisp_value sym)
{
    if (!symbolp(sym) || cc_is_mutated(cc, sym))
        return;
    if (cc->mutated_count < CC_MAX_MUTATED)
        cc->mutated[cc->mutated_count++] = sym;
}

static void cc_scan_form(struct cc_fn *cc, lisp_value form, int depth)
{
    if (depth > 256 || !consp(form))
        return;
    lisp_value head = CAR(form);
    lisp_value args = CDR(form);
    if (symbolp(head)) {
        uint32_t id = SYM(head)->special_id;
        switch (id) {
        case SF_QUOTE:
        case SF_DECLARE:
        case SF_DECLAIM:
            return;
        case SF_LAMBDA:
        case SF_FLET:
        case SF_LABELS:
        case SF_BLOCK:
        case SF_UNWIND_PROTECT:
        case SF_IGNORE_ERRORS:
        case SF_HANDLER_CASE:
            cc->has_inner_closure = true;
            break;
        case SF_FUNCTION:
            if (consp(args) && consp(CAR(args)) && symbolp(CAR(CAR(args))) &&
                    SYM(CAR(CAR(args)))->special_id == SF_LAMBDA)
                cc->has_inner_closure = true;
            break;
        case SF_SETQ:
        case SF_SETF:
            for (lisp_value p = args; consp(p) && consp(CDR(p)); p = CDR(CDR(p))) {
                lisp_value place = CAR(p);
                if (symbolp(place))
                    cc_mark_mutated(cc, place);
                else if (consp(place) && consp(CDR(place)) && symbolp(CAR(CDR(place))))
                    cc_mark_mutated(cc, CAR(CDR(place)));
                cc_scan_form(cc, place, depth + 1);
                cc_scan_form(cc, CAR(CDR(p)), depth + 1);
            }
            return;
        case SF_PUSH:
        case SF_PUSHNEW:
            if (consp(args) && consp(CDR(args))) {
                lisp_value place = CAR(CDR(args));
                if (symbolp(place))
                    cc_mark_mutated(cc, place);
            }
            break;
        case SF_POP:
        case SF_INCF:
        case SF_DECF:
            if (consp(args) && symbolp(CAR(args)))
                cc_mark_mutated(cc, CAR(args));
            break;
        case SF_ROTATEF:
            for (lisp_value p = args; consp(p); p = CDR(p))
                if (symbolp(CAR(p)))
                    cc_mark_mutated(cc, CAR(p));
            break;
        case SF_MULTIPLE_VALUE_SETQ:
            if (consp(args)) {
                for (lisp_value p = CAR(args); consp(p); p = CDR(p))
                    if (symbolp(CAR(p)))
                        cc_mark_mutated(cc, CAR(p));
            }
            break;
        case SF_DOLIST:
        case SF_DOTIMES:
            if (consp(args) && consp(CAR(args)) && symbolp(CAR(CAR(args))))
                cc_mark_mutated(cc, CAR(CAR(args)));
            break;
        case SF_LOOP:
            for (lisp_value p = args; consp(p); p = CDR(p))
                if (symbolp(CAR(p)))
                    cc_mark_mutated(cc, CAR(p));
            break;
        default:
            break;
        }
        if (id == SF_LABELS && consp(args)) {
            for (lisp_value d = CAR(args); consp(d); d = CDR(d))
                if (consp(CAR(d)) && symbolp(CAR(CAR(d))))
                    cc_mark_mutated(cc, CAR(CAR(d)));
        }
    }
    for (lisp_value p = form; consp(p); p = CDR(p))
        cc_scan_form(cc, CAR(p), depth + 1);
}

static uint16_t cc_add_const(struct cc_fn *cc, lisp_value v)
{
    for (uint32_t i = 0; i < cc->const_count; i++)
        if (cc->consts[i] == v)
            return (uint16_t)i;
    if (cc->const_count >= CC_MAX_CONSTS) {
        cc->failed = true;
        return 0;
    }
    uint16_t idx = (uint16_t)cc->const_count++;
    cc->consts[idx] = v;
    return idx;
}

static uint16_t cc_add_proto(struct cc_fn *cc, const struct lisp_code *proto)
{
    if (cc->proto_count >= CC_MAX_PROTOS) {
        cc->failed = true;
        return 0;
    }
    uint16_t idx = (uint16_t)cc->proto_count++;
    cc->protos[idx] = proto;
    return idx;
}

static void cc_emit(struct cc_fn *cc, uint32_t inst)
{
    if (cc->ins_count >= CC_MAX_INS) {
        cc->failed = true;
        return;
    }
    cc->ins[cc->ins_count++] = inst;
}

static int cc_emit_jmp(struct cc_fn *cc, enum vm_op op)
{
    int pos = (int)cc->ins_count;
    cc_emit(cc, INST(op, 0, 0));
    return pos;
}

static void cc_patch_jmp(struct cc_fn *cc, int pos)
{
    if (cc->failed)
        return;
    int32_t diff = (int32_t)cc->ins_count - (int32_t)pos - 1;
    if (diff < -32768 || diff > 32767) {
        cc->failed = true;
        return;
    }
    uint32_t old = cc->ins[pos];
    cc->ins[pos] = INST(OP_OF(old), A_OF(old), (int16_t)diff);
}

static void cc_emit_jmp_to(struct cc_fn *cc, enum vm_op op, int target)
{
    int32_t diff = (int32_t)target - (int32_t)cc->ins_count - 1;
    if (diff < -32768 || diff > 32767) {
        cc->failed = true;
        return;
    }
    cc_emit(cc, INST(op, 0, (int16_t)diff));
}

static uint16_t cc_alloc_slot(struct cc_fn *cc)
{
    if (cc->next_slot >= CC_MAX_LOCALS) {
        cc->failed = true;
        return 0;
    }
    uint16_t slot = cc->next_slot++;
    if (cc->next_slot > cc->max_slot)
        cc->max_slot = cc->next_slot;
    return slot;
}

static uint16_t cc_push_var(struct cc_fn *cc, lisp_value sym, uint8_t kind, bool *boxed_out)
{
    if (cc->var_count >= CC_MAX_LOCALS) {
        cc->failed = true;
        if (boxed_out)
            *boxed_out = false;
        return 0;
    }
    uint16_t slot = cc_alloc_slot(cc);
    bool boxed = cc->has_inner_closure && (cc_is_mutated(cc, sym) || kind == BIND_FUNC);
    cc->vars[cc->var_count++] = (struct cc_var){ sym, slot, kind, (uint8_t)boxed };
    if (boxed_out)
        *boxed_out = boxed;
    return slot;
}

static int cc_find_local(struct cc_fn *cc, lisp_value sym, uint8_t kind, bool *boxed_out)
{
    for (uint32_t i = cc->var_count; i-- > 0;) {
        if (cc->vars[i].sym == sym && cc->vars[i].kind == kind) {
            if (boxed_out)
                *boxed_out = cc->vars[i].boxed != 0;
            return cc->vars[i].slot;
        }
    }
    return -1;
}

static int cc_find_upval(struct cc_fn *cc, lisp_value sym, uint8_t kind, bool *boxed_out)
{
    for (uint32_t i = 0; i < cc->upval_count; i++) {
        if (cc->up_syms[i] == sym && cc->up_kinds[i] == kind) {
            if (boxed_out)
                *boxed_out = cc->updesc[i].boxed != 0;
            return (int)i;
        }
    }
    if (!cc->parent)
        return -1;
    bool boxed = false;
    int slot = cc_find_local(cc->parent, sym, kind, &boxed);
    if (slot >= 0) {
        if (cc->upval_count >= CC_MAX_UPVALS) {
            cc->failed = true;
            return -1;
        }
        uint32_t idx = cc->upval_count++;
        cc->updesc[idx] = (struct vm_updesc){ 1, (uint8_t)boxed, (uint16_t)slot };
        cc->up_syms[idx] = sym;
        cc->up_kinds[idx] = kind;
        if (boxed_out)
            *boxed_out = boxed;
        return (int)idx;
    }
    int up = cc_find_upval(cc->parent, sym, kind, &boxed);
    if (up >= 0) {
        if (cc->upval_count >= CC_MAX_UPVALS) {
            cc->failed = true;
            return -1;
        }
        uint32_t idx = cc->upval_count++;
        cc->updesc[idx] = (struct vm_updesc){ 0, (uint8_t)boxed, (uint16_t)up };
        cc->up_syms[idx] = sym;
        cc->up_kinds[idx] = kind;
        if (boxed_out)
            *boxed_out = boxed;
        return (int)idx;
    }
    return -1;
}

static bool proper_list_len(lisp_value list, uint32_t *count)
{
    uint32_t n = 0;
    while (consp(list)) {
        n++;
        list = CDR(list);
    }
    *count = n;
    return nilp(list);
}

static bool cc_emit_expr(struct cc_fn *cc, lisp_value form, bool want_mv, bool tail);
static bool cc_emit_body(struct cc_fn *cc, lisp_value body, bool want_mv, bool tail);
static const struct lisp_code *cc_compile_fn(struct cc_fn *parent, struct lisp_region *region,
    lisp_value name, lisp_value params, lisp_value body, uint8_t info);

static bool cc_emit_var_get(struct cc_fn *cc, lisp_value sym, bool want_mv)
{
    struct lisp_symbol *s = SYM(sym);
    if (sym == NIL_VAL) {
        cc_emit(cc, INST(OP_NIL, 0, 0));
    } else if (sym == T_VAL) {
        cc_emit(cc, INST(OP_T, 0, 0));
    } else if (s->h.flags & SYM_KEYWORD) {
        cc_emit(cc, INST(OP_CONST, 0, cc_add_const(cc, sym)));
    } else if (s->h.flags & SYM_SPECIAL) {
        cc_emit(cc, INST(OP_GET_SPECIAL, 0, cc_add_const(cc, sym)));
    } else {
        bool boxed = false;
        int slot = cc_find_local(cc, sym, BIND_VAR, &boxed);
        if (slot >= 0) {
            cc_emit(cc, INST(boxed ? OP_GET_BOX : OP_GET_LOCAL, 0, slot));
        } else {
            int up = cc_find_upval(cc, sym, BIND_VAR, &boxed);
            if (up >= 0) {
                cc_emit(cc, INST(boxed ? OP_GET_UPVAL_BOX : OP_GET_UPVAL, 0, up));
            } else {
                cc_emit(cc, INST(OP_GET_SPECIAL, 0, cc_add_const(cc, sym)));
            }
        }
    }
    if (want_mv)
        cc_emit(cc, INST(OP_MV1, 0, 0));
    return !cc->failed;
}

static bool cc_emit_var_set(struct cc_fn *cc, lisp_value sym)
{
    struct lisp_symbol *s = SYM(sym);
    if (s->h.flags & (SYM_CONSTANT | SYM_KEYWORD)) {
        cc->failed = true;
        return false;
    }
    if (s->h.flags & SYM_SPECIAL) {
        cc_emit(cc, INST(OP_SET_SPECIAL, 0, cc_add_const(cc, sym)));
        return !cc->failed;
    }
    bool boxed = false;
    int slot = cc_find_local(cc, sym, BIND_VAR, &boxed);
    if (slot >= 0) {
        cc_emit(cc, INST(boxed ? OP_SET_BOX : OP_SET_LOCAL, 0, slot));
        return !cc->failed;
    }
    int up = cc_find_upval(cc, sym, BIND_VAR, &boxed);
    if (up >= 0) {
        if (!boxed) {
            cc->failed = true;
            return false;
        }
        cc_emit(cc, INST(OP_SET_UPVAL_BOX, 0, up));
        return !cc->failed;
    }
    cc_emit(cc, INST(OP_SET_SPECIAL, 0, cc_add_const(cc, sym)));
    return !cc->failed;
}

static bool cc_emit_func_get(struct cc_fn *cc, lisp_value sym)
{
    bool boxed = false;
    int slot = cc_find_local(cc, sym, BIND_FUNC, &boxed);
    if (slot >= 0) {
        cc_emit(cc, INST(boxed ? OP_GET_BOX : OP_GET_LOCAL, 0, slot));
        return !cc->failed;
    }
    int up = cc_find_upval(cc, sym, BIND_FUNC, &boxed);
    if (up >= 0) {
        cc_emit(cc, INST(boxed ? OP_GET_UPVAL_BOX : OP_GET_UPVAL, 0, up));
        return !cc->failed;
    }
    cc_emit(cc, INST(OP_GET_FUNC, 0, cc_add_const(cc, sym)));
    return !cc->failed;
}

static bool cc_emit_thunk(struct cc_fn *cc, lisp_value params, lisp_value body)
{
    const struct lisp_code *proto = cc_compile_fn(cc, cc->region, NIL_VAL, params, body, 0);
    if (!proto)
        return false;
    cc_emit(cc, INST(OP_MAKE_CLOSURE, 0, cc_add_proto(cc, proto)));
    return !cc->failed;
}

static bool cc_is_simple_pattern(lisp_value pat)
{
    if (nilp(pat) || symbolp(pat))
        return true;
    if (!consp(pat))
        return false;
    for (lisp_value p = pat; consp(p); p = CDR(p)) {
        lisp_value item = CAR(p);
        if (item == VAL(lisp.sym_optional) || item == VAL(lisp.sym_key) ||
                item == VAL(lisp.sym_rest) || item == VAL(lisp.sym_body) ||
                item == VAL(lisp.sym_allow_other_keys) || item == VAL(lisp.sym_aux))
            return false;
        if (!cc_is_simple_pattern(item))
            return false;
    }
    return true;
}

static bool cc_pattern_has_special(lisp_value pat)
{
    if (nilp(pat))
        return false;
    if (symbolp(pat))
        return (SYM(pat)->h.flags & SYM_SPECIAL) != 0;
    if (!consp(pat))
        return true;
    return cc_pattern_has_special(CAR(pat)) || cc_pattern_has_special(CDR(pat));
}

static void cc_emit_simple_bind(struct cc_fn *cc, lisp_value pat)
{
    if (nilp(pat)) {
        cc_emit(cc, INST(OP_POP, 0, 0));
        return;
    }
    if (symbolp(pat)) {
        bool boxed = false;
        uint16_t slot = cc_push_var(cc, pat, BIND_VAR, &boxed);
        cc_emit(cc, INST(OP_SET_LOCAL_POP, 0, slot));
        if (boxed)
            cc_emit(cc, INST(OP_BOX_LOCAL, 0, slot));
        return;
    }
    uint16_t tmp = cc_alloc_slot(cc);
    cc_emit(cc, INST(OP_SET_LOCAL_POP, 0, tmp));
    lisp_value p = pat;
    while (consp(p)) {
        cc_emit(cc, INST(OP_GET_LOCAL, 0, tmp));
        cc_emit(cc, INST(OP_CAR, 0, 0));
        cc_emit_simple_bind(cc, CAR(p));
        p = CDR(p);
        if (consp(p) || !nilp(p)) {
            cc_emit(cc, INST(OP_GET_LOCAL, 0, tmp));
            cc_emit(cc, INST(OP_CDR, 0, 0));
            cc_emit(cc, INST(OP_SET_LOCAL_POP, 0, tmp));
        }
    }
    if (!nilp(p)) {
        cc_emit(cc, INST(OP_GET_LOCAL, 0, tmp));
        cc_emit_simple_bind(cc, p);
    }
}

static bool cc_bind_simple_pattern_existing(struct cc_fn *cc, lisp_value pat, uint16_t *slots, uint32_t *idx)
{
    if (nilp(pat)) {
        cc_emit(cc, INST(OP_POP, 0, 0));
        return true;
    }
    if (symbolp(pat)) {
        uint16_t slot = slots[(*idx)++];
        bool boxed = false;
        cc_find_local(cc, pat, BIND_VAR, &boxed);
        if (boxed) {
            cc_emit(cc, INST(OP_SET_BOX, 0, slot));
            cc_emit(cc, INST(OP_POP, 0, 0));
        } else {
            cc_emit(cc, INST(OP_SET_LOCAL_POP, 0, slot));
        }
        return true;
    }
    uint16_t tmp = cc_alloc_slot(cc);
    cc_emit(cc, INST(OP_SET_LOCAL_POP, 0, tmp));
    lisp_value p = pat;
    while (consp(p)) {
        cc_emit(cc, INST(OP_GET_LOCAL, 0, tmp));
        cc_emit(cc, INST(OP_CAR, 0, 0));
        if (!cc_bind_simple_pattern_existing(cc, CAR(p), slots, idx))
            return false;
        p = CDR(p);
        if (consp(p) || !nilp(p)) {
            cc_emit(cc, INST(OP_GET_LOCAL, 0, tmp));
            cc_emit(cc, INST(OP_CDR, 0, 0));
            cc_emit(cc, INST(OP_SET_LOCAL_POP, 0, tmp));
        }
    }
    if (!nilp(p)) {
        cc_emit(cc, INST(OP_GET_LOCAL, 0, tmp));
        if (!cc_bind_simple_pattern_existing(cc, p, slots, idx))
            return false;
    }
    return !cc->failed;
}

static void cc_declare_pattern_vars(struct cc_fn *cc, lisp_value pat, uint16_t *slots, uint32_t *count)
{
    if (nilp(pat))
        return;
    if (symbolp(pat)) {
        bool boxed = false;
        uint16_t slot = cc_push_var(cc, pat, BIND_VAR, &boxed);
        cc_emit(cc, INST(OP_NIL, 0, 0));
        cc_emit(cc, INST(OP_SET_LOCAL_POP, 0, slot));
        if (boxed)
            cc_emit(cc, INST(OP_BOX_LOCAL, 0, slot));
        slots[(*count)++] = slot;
        return;
    }
    for (lisp_value p = pat; consp(p); p = CDR(p)) {
        cc_declare_pattern_vars(cc, CAR(p), slots, count);
        if (!consp(CDR(p)) && !nilp(CDR(p)))
            cc_declare_pattern_vars(cc, CDR(p), slots, count);
    }
}

static bool cc_emit_loop(struct cc_fn *cc, lisp_value clauses, bool want_mv)
{
    struct loop_plan plan;
    if (!loop_parse(clauses, &plan, false))
        return false;
    for (int i = 0; i < plan.nw; i++)
        if (!symbolp(plan.with_vars[i]) || (SYM(plan.with_vars[i])->h.flags & SYM_SPECIAL))
            return false;
    for (int i = 0; i < plan.nf; i++) {
        if (!cc_is_simple_pattern(plan.fors[i].pattern) || cc_pattern_has_special(plan.fors[i].pattern))
            return false;
        if (plan.fors[i].kind == LF_IN || plan.fors[i].kind == LF_ON) {
            if (!nilp(plan.fors[i].by)) {
                lisp_value by = plan.fors[i].by;
                bool is_cddr = consp(by) && (CAR(by) == VAL(lisp.sym_function) || CAR(by) == VAL(lisp.sym_quote)) &&
                    consp(CDR(by)) && symbolp(CAR(CDR(by))) &&
                    TYPE(SYM(CAR(CDR(by)))->function) == LT_FUNCTION &&
                    PTR(SYM(CAR(CDR(by)))->function, lisp_function)->kind == FN_BUILTIN &&
                    PTR(SYM(CAR(CDR(by)))->function, lisp_function)->fn == builtin_cddr;
                if (!is_cddr)
                    return false;
            }
        }
        if (plan.fors[i].kind == LF_FROM && !nilp(plan.fors[i].by)) {
            if (!intp(plan.fors[i].by))
                return false;
        }
    }
    for (int i = 0; i < plan.na; i++)
        if (!plan.accs[i].anonymous && (!symbolp(plan.accs[i].var) || (SYM(plan.accs[i].var)->h.flags & SYM_SPECIAL)))
            return false;

    uint32_t saved_vars = cc->var_count;
    uint16_t saved_slot = cc->next_slot;

    for (int i = 0; i < plan.nw; i++) {
        if (!nilp(plan.with_inits[i])) {
            if (!cc_emit_expr(cc, plan.with_inits[i], false, false))
                return false;
        } else {
            cc_emit(cc, INST(OP_NIL, 0, 0));
        }
        bool boxed = false;
        uint16_t slot = cc_push_var(cc, plan.with_vars[i], BIND_VAR, &boxed);
        cc_emit(cc, INST(OP_SET_LOCAL_POP, 0, slot));
        if (boxed)
            cc_emit(cc, INST(OP_BOX_LOCAL, 0, slot));
    }

    uint16_t repeat_slot = 0;
    if (!nilp(plan.repeat_expr)) {
        if (!cc_emit_expr(cc, plan.repeat_expr, false, false))
            return false;
        repeat_slot = cc_alloc_slot(cc);
        cc_emit(cc, INST(OP_SET_LOCAL_POP, 0, repeat_slot));
    }

    uint16_t acc_head[16];
    uint16_t acc_tail[16];
    uint8_t acc_boxed[16];
    for (int i = 0; i < plan.na; i++) {
        struct loop_acc *a = &plan.accs[i];
        bool boxed = false;
        if (!a->anonymous) {
            acc_head[i] = cc_push_var(cc, a->var, BIND_VAR, &boxed);
            acc_boxed[i] = (uint8_t)boxed;
        } else {
            acc_head[i] = cc_alloc_slot(cc);
            acc_boxed[i] = 0;
        }
        if (a->kind == ACC_SUM || a->kind == ACC_COUNT)
            cc_emit(cc, INST(OP_CONST, 0, cc_add_const(cc, make_int(0))));
        else
            cc_emit(cc, INST(OP_NIL, 0, 0));
        cc_emit(cc, INST(OP_SET_LOCAL_POP, 0, acc_head[i]));
        if (acc_boxed[i])
            cc_emit(cc, INST(OP_BOX_LOCAL, 0, acc_head[i]));
        if (a->kind == ACC_COLLECT || a->kind == ACC_APPEND || a->kind == ACC_NCONC) {
            acc_tail[i] = cc_alloc_slot(cc);
            cc_emit(cc, INST(OP_NIL, 0, 0));
            cc_emit(cc, INST(OP_SET_LOCAL_POP, 0, acc_tail[i]));
        }
    }

    uint16_t for_state[16];
    uint16_t for_extra[16];
    uint16_t for_bound[16];
    uint16_t for_var_slots[16][16];
    uint32_t for_var_counts[16];
    uint16_t first_flag = cc_alloc_slot(cc);
    cc_emit(cc, INST(OP_T, 0, 0));
    cc_emit(cc, INST(OP_SET_LOCAL_POP, 0, first_flag));

    for (int i = 0; i < plan.nf; i++) {
        struct loop_for *f = &plan.fors[i];
        for_var_counts[i] = 0;
        if (f->kind == LF_IN || f->kind == LF_ON) {
            if (!cc_emit_expr(cc, f->expr, false, false))
                return false;
            for_state[i] = cc_alloc_slot(cc);
            cc_emit(cc, INST(OP_SET_LOCAL_POP, 0, for_state[i]));
        } else if (f->kind == LF_ACROSS) {
            if (!cc_emit_expr(cc, f->expr, false, false))
                return false;
            for_state[i] = cc_alloc_slot(cc);
            cc_emit(cc, INST(OP_SET_LOCAL_POP, 0, for_state[i]));
            for_bound[i] = cc_alloc_slot(cc);
            cc_emit(cc, INST(OP_GET_LOCAL, 0, for_state[i]));
            cc_emit(cc, INST(OP_LOOP_ACROSS_LEN, 0, 0));
            cc_emit(cc, INST(OP_SET_LOCAL_POP, 0, for_bound[i]));
            for_extra[i] = cc_alloc_slot(cc);
            cc_emit(cc, INST(OP_CONST, 0, cc_add_const(cc, make_int(0))));
            cc_emit(cc, INST(OP_SET_LOCAL_POP, 0, for_extra[i]));
        } else if (f->kind == LF_FROM) {
            if (!cc_emit_expr(cc, f->from_expr, false, false))
                return false;
            for_state[i] = cc_alloc_slot(cc);
            cc_emit(cc, INST(OP_SET_LOCAL_POP, 0, for_state[i]));
            if (f->bound_kind != LB_NONE) {
                if (!cc_emit_expr(cc, f->bound, false, false))
                    return false;
                for_bound[i] = cc_alloc_slot(cc);
                cc_emit(cc, INST(OP_SET_LOCAL_POP, 0, for_bound[i]));
            }
        }
        cc_declare_pattern_vars(cc, f->pattern, for_var_slots[i], &for_var_counts[i]);
    }

    if (cc->loop_depth >= CC_MAX_LOOPS) {
        cc->failed = true;
        return false;
    }
    int loop_idx = cc->loop_depth++;
    cc->loops[loop_idx].break_count = 0;
    cc->loops[loop_idx].dyn_depth = cc->dyn_depth;
    cc->loops[loop_idx].stack_depth = cc->stack_depth;

    int exit_jmps[64];
    int exit_jmp_count = 0;

    int loop_start = (int)cc->ins_count;
    cc_emit(cc, INST(OP_STEP, 0, 0));

    if (!nilp(plan.repeat_expr)) {
        cc_emit(cc, INST(OP_GET_LOCAL, 0, repeat_slot));
        cc_emit(cc, INST(OP_CONST, 0, cc_add_const(cc, make_int(0))));
        cc_emit(cc, INST(OP_LE2, 0, 0));
        exit_jmps[exit_jmp_count++] = cc_emit_jmp(cc, OP_JMP_NOT_NIL);
        cc_emit(cc, INST(OP_GET_LOCAL, 0, repeat_slot));
        cc_emit(cc, INST(OP_SUB1, 0, 0));
        cc_emit(cc, INST(OP_SET_LOCAL_POP, 0, repeat_slot));
    }

    if (!nilp(plan.while_expr) && !plan.while_after_for) {
        if (!cc_emit_expr(cc, plan.while_expr, false, false))
            return false;
        exit_jmps[exit_jmp_count++] = cc_emit_jmp(cc, OP_JMP_NIL);
    }

    for (int i = 0; i < plan.nf; i++) {
        struct loop_for *f = &plan.fors[i];
        uint32_t idx = 0;
        if (f->kind == LF_IN) {
            int j = (int)cc->ins_count;
            cc_emit(cc, INST(nilp(f->by) ? OP_LOOP_STEP_IN : OP_LOOP_STEP_IN_CDDR, for_state[i], 0));
            exit_jmps[exit_jmp_count++] = j;
            cc_bind_simple_pattern_existing(cc, f->pattern, for_var_slots[i], &idx);
        } else if (f->kind == LF_ON) {
            int j = (int)cc->ins_count;
            cc_emit(cc, INST(nilp(f->by) ? OP_LOOP_STEP_ON : OP_LOOP_STEP_ON_CDDR, for_state[i], 0));
            exit_jmps[exit_jmp_count++] = j;
            cc_bind_simple_pattern_existing(cc, f->pattern, for_var_slots[i], &idx);
        } else if (f->kind == LF_ACROSS) {
            cc_emit(cc, INST(OP_GET_LOCAL, 0, for_extra[i]));
            cc_emit(cc, INST(OP_GET_LOCAL, 0, for_bound[i]));
            cc_emit(cc, INST(OP_GE2, 0, 0));
            exit_jmps[exit_jmp_count++] = cc_emit_jmp(cc, OP_JMP_NOT_NIL);
            cc_emit(cc, INST(OP_GET_LOCAL, 0, for_state[i]));
            cc_emit(cc, INST(OP_GET_LOCAL, 0, for_extra[i]));
            cc_emit(cc, INST(OP_LOOP_ACROSS_REF, 0, 0));
            cc_bind_simple_pattern_existing(cc, f->pattern, for_var_slots[i], &idx);
            cc_emit(cc, INST(OP_GET_LOCAL, 0, for_extra[i]));
            cc_emit(cc, INST(OP_ADD1, 0, 0));
            cc_emit(cc, INST(OP_SET_LOCAL_POP, 0, for_extra[i]));
        } else if (f->kind == LF_FROM) {
            if (f->bound_kind != LB_NONE) {
                cc_emit(cc, INST(OP_GET_LOCAL, 0, for_state[i]));
                cc_emit(cc, INST(OP_GET_LOCAL, 0, for_bound[i]));
                if (f->bound_kind == LB_TO)
                    cc_emit(cc, INST(OP_GT2, 0, 0));
                else if (f->bound_kind == LB_BELOW)
                    cc_emit(cc, INST(OP_GE2, 0, 0));
                else
                    cc_emit(cc, INST(OP_LT2, 0, 0));
                exit_jmps[exit_jmp_count++] = cc_emit_jmp(cc, OP_JMP_NOT_NIL);
            }
            cc_emit(cc, INST(OP_GET_LOCAL, 0, for_state[i]));
            cc_bind_simple_pattern_existing(cc, f->pattern, for_var_slots[i], &idx);
            cc_emit(cc, INST(OP_GET_LOCAL, 0, for_state[i]));
            int64_t step = nilp(f->by) ? 1 : INTVAL(f->by);
            if (f->bound_kind == LB_DOWNTO) {
                if (step == 1)
                    cc_emit(cc, INST(OP_SUB1, 0, 0));
                else {
                    cc_emit(cc, INST(OP_CONST, 0, cc_add_const(cc, make_int(step))));
                    cc_emit(cc, INST(OP_SUB2, 0, 0));
                }
            } else {
                if (step == 1)
                    cc_emit(cc, INST(OP_ADD1, 0, 0));
                else {
                    cc_emit(cc, INST(OP_CONST, 0, cc_add_const(cc, make_int(step))));
                    cc_emit(cc, INST(OP_ADD2, 0, 0));
                }
            }
            cc_emit(cc, INST(OP_SET_LOCAL_POP, 0, for_state[i]));
        } else if (f->kind == LF_EQ) {
            if (!nilp(f->then_expr)) {
                cc_emit(cc, INST(OP_GET_LOCAL, 0, first_flag));
                int j_then = cc_emit_jmp(cc, OP_JMP_NIL);
                if (!cc_emit_expr(cc, f->expr, false, false))
                    return false;
                int j_done = cc_emit_jmp(cc, OP_JMP);
                cc_patch_jmp(cc, j_then);
                if (!cc_emit_expr(cc, f->then_expr, false, false))
                    return false;
                cc_patch_jmp(cc, j_done);
            } else {
                if (!cc_emit_expr(cc, f->expr, false, false))
                    return false;
            }
            cc_bind_simple_pattern_existing(cc, f->pattern, for_var_slots[i], &idx);
        }
    }

    cc_emit(cc, INST(OP_NIL, 0, 0));
    cc_emit(cc, INST(OP_SET_LOCAL_POP, 0, first_flag));

    if (!nilp(plan.while_expr) && plan.while_after_for) {
        if (!cc_emit_expr(cc, plan.while_expr, false, false))
            return false;
        exit_jmps[exit_jmp_count++] = cc_emit_jmp(cc, OP_JMP_NIL);
    }

    int early_return_jmps[64];
    int early_return_count = 0;

    for (int i = 0; i < plan.nb; i++) {
        struct loop_act *act = &plan.acts[i];
        int skip_jmp = -1;
        if (!nilp(act->guard)) {
            if (!cc_emit_expr(cc, act->guard, false, false))
                return false;
            skip_jmp = cc_emit_jmp(cc, act->negate ? OP_JMP_NOT_NIL : OP_JMP_NIL);
        }
        switch (act->kind) {
        case ACT_DO:
            for (lisp_value f = act->forms; consp(f); f = CDR(f)) {
                if (!cc_emit_expr(cc, CAR(f), false, false))
                    return false;
                cc_emit(cc, INST(OP_POP, 0, 0));
            }
            break;
        case ACT_COLLECT:
        case ACT_APPEND:
        case ACT_NCONC: {
            if (!cc_emit_expr(cc, act->expr, false, false))
                return false;
            int ai = act->acc;
            if (acc_boxed[ai]) {
                uint16_t tmp = cc_alloc_slot(cc);
                cc_emit(cc, INST(OP_GET_BOX, 0, acc_head[ai]));
                cc_emit(cc, INST(OP_SET_LOCAL_POP, 0, tmp));
                enum vm_op op = act->kind == ACT_COLLECT ? OP_LOOP_COLLECT :
                    (act->kind == ACT_APPEND ? OP_LOOP_APPEND : OP_LOOP_NCONC);
                cc_emit(cc, INST(op, tmp, acc_tail[ai]));
                cc_emit(cc, INST(OP_GET_LOCAL, 0, tmp));
                cc_emit(cc, INST(OP_SET_BOX, 0, acc_head[ai]));
                cc_emit(cc, INST(OP_POP, 0, 0));
            } else {
                enum vm_op op = act->kind == ACT_COLLECT ? OP_LOOP_COLLECT :
                    (act->kind == ACT_APPEND ? OP_LOOP_APPEND : OP_LOOP_NCONC);
                cc_emit(cc, INST(op, acc_head[ai], acc_tail[ai]));
            }
            break;
        }
        case ACT_SUM: {
            int ai = act->acc;
            cc_emit(cc, INST(acc_boxed[ai] ? OP_GET_BOX : OP_GET_LOCAL, 0, acc_head[ai]));
            if (!cc_emit_expr(cc, act->expr, false, false))
                return false;
            cc_emit(cc, INST(OP_ADD2, 0, 0));
            if (acc_boxed[ai]) {
                cc_emit(cc, INST(OP_SET_BOX, 0, acc_head[ai]));
                cc_emit(cc, INST(OP_POP, 0, 0));
            } else {
                cc_emit(cc, INST(OP_SET_LOCAL_POP, 0, acc_head[ai]));
            }
            break;
        }
        case ACT_COUNT: {
            if (!cc_emit_expr(cc, act->expr, false, false))
                return false;
            int j_no = cc_emit_jmp(cc, OP_JMP_NIL);
            int ai = act->acc;
            cc_emit(cc, INST(acc_boxed[ai] ? OP_GET_BOX : OP_GET_LOCAL, 0, acc_head[ai]));
            cc_emit(cc, INST(OP_ADD1, 0, 0));
            if (acc_boxed[ai]) {
                cc_emit(cc, INST(OP_SET_BOX, 0, acc_head[ai]));
                cc_emit(cc, INST(OP_POP, 0, 0));
            } else {
                cc_emit(cc, INST(OP_SET_LOCAL_POP, 0, acc_head[ai]));
            }
            cc_patch_jmp(cc, j_no);
            break;
        }
        case ACT_MAX:
        case ACT_MIN: {
            int ai = act->acc;
            if (!cc_emit_expr(cc, act->expr, false, false))
                return false;
            uint16_t tmp = cc_alloc_slot(cc);
            cc_emit(cc, INST(OP_SET_LOCAL_POP, 0, tmp));
            cc_emit(cc, INST(acc_boxed[ai] ? OP_GET_BOX : OP_GET_LOCAL, 0, acc_head[ai]));
            int j_first = cc_emit_jmp(cc, OP_JMP_NIL);
            cc_emit(cc, INST(OP_GET_LOCAL, 0, tmp));
            cc_emit(cc, INST(acc_boxed[ai] ? OP_GET_BOX : OP_GET_LOCAL, 0, acc_head[ai]));
            cc_emit(cc, INST(act->kind == ACT_MAX ? OP_GT2 : OP_LT2, 0, 0));
            int j_keep = cc_emit_jmp(cc, OP_JMP_NIL);
            cc_patch_jmp(cc, j_first);
            cc_emit(cc, INST(OP_GET_LOCAL, 0, tmp));
            if (acc_boxed[ai]) {
                cc_emit(cc, INST(OP_SET_BOX, 0, acc_head[ai]));
                cc_emit(cc, INST(OP_POP, 0, 0));
            } else {
                cc_emit(cc, INST(OP_SET_LOCAL_POP, 0, acc_head[ai]));
            }
            cc_patch_jmp(cc, j_keep);
            break;
        }
        case ACT_RETURN:
            if (!cc_emit_expr(cc, act->expr, want_mv, false))
                return false;
            early_return_jmps[early_return_count++] = cc_emit_jmp(cc, OP_JMP);
            break;
        case ACT_THEREIS:
            if (!cc_emit_expr(cc, act->expr, false, false))
                return false;
            cc_emit(cc, INST(OP_DUP, 0, 0));
            {
                int j_no = cc_emit_jmp(cc, OP_JMP_NIL);
                if (want_mv)
                    cc_emit(cc, INST(OP_MV1, 0, 0));
                early_return_jmps[early_return_count++] = cc_emit_jmp(cc, OP_JMP);
                cc_patch_jmp(cc, j_no);
                cc_emit(cc, INST(OP_POP, 0, 0));
            }
            break;
        case ACT_ALWAYS:
            if (!cc_emit_expr(cc, act->expr, false, false))
                return false;
            {
                int j_ok = cc_emit_jmp(cc, OP_JMP_NOT_NIL);
                cc_emit(cc, INST(OP_NIL, 0, 0));
                if (want_mv)
                    cc_emit(cc, INST(OP_MV1, 0, 0));
                early_return_jmps[early_return_count++] = cc_emit_jmp(cc, OP_JMP);
                cc_patch_jmp(cc, j_ok);
            }
            break;
        }
        if (skip_jmp >= 0)
            cc_patch_jmp(cc, skip_jmp);
    }

    cc_emit_jmp_to(cc, OP_JMP, loop_start);

    for (int i = 0; i < exit_jmp_count; i++)
        cc_patch_jmp(cc, exit_jmps[i]);

    for (int i = 0; i < plan.nfin; i++) {
        if (!cc_emit_expr(cc, plan.finally_forms[i], false, false))
            return false;
        cc_emit(cc, INST(OP_POP, 0, 0));
    }

    int anon = -1;
    for (int i = 0; i < plan.na; i++) {
        if (plan.accs[i].anonymous) {
            anon = i;
            break;
        }
    }
    if (plan.always_used) {
        cc_emit(cc, INST(OP_T, 0, 0));
    } else if (anon >= 0) {
        cc_emit(cc, INST(acc_boxed[anon] ? OP_GET_BOX : OP_GET_LOCAL, 0, acc_head[anon]));
    } else {
        cc_emit(cc, INST(OP_NIL, 0, 0));
    }
    if (want_mv)
        cc_emit(cc, INST(OP_MV1, 0, 0));

    for (int i = 0; i < early_return_count; i++)
        cc_patch_jmp(cc, early_return_jmps[i]);
    for (int i = 0; i < cc->loops[loop_idx].break_count; i++)
        cc_patch_jmp(cc, cc->loops[loop_idx].breaks[i]);

    cc->loop_depth--;
    cc->var_count = saved_vars;
    cc->next_slot = saved_slot;
    return !cc->failed;
}

static bool cc_try_inline_builtin(struct cc_fn *cc, struct lisp_symbol *sym, lisp_value args, uint32_t argc,
    bool want_mv)
{
    if (cc_find_local(cc, VAL(sym), BIND_FUNC, NULL) >= 0 ||
            cc_find_upval(cc, VAL(sym), BIND_FUNC, NULL) >= 0)
        return false;
    if (TYPE(sym->function) != LT_FUNCTION || PTR(sym->function, lisp_function)->kind != FN_BUILTIN)
        return false;
    lisp_primitive fn = PTR(sym->function, lisp_function)->fn;

    enum vm_op op = OP_NOP;
    if (argc == 1) {
        if (fn == builtin_car || fn == builtin_first) op = OP_CAR;
        else if (fn == builtin_cdr || fn == builtin_rest) op = OP_CDR;
        else if (fn == builtin_cddr) op = OP_CDDR;
        else if (fn == builtin_caar) op = OP_CAAR;
        else if (fn == builtin_cdar) op = OP_CDAR;
        else if (fn == builtin_second) op = OP_SECOND;
        else if (fn == builtin_third) op = OP_THIRD;
        else if (fn == builtin_fourth) op = OP_FOURTH;
        else if (fn == builtin_fifth) op = OP_FIFTH;
        else if (fn == builtin_null || fn == builtin_not) op = OP_NULL;
        else if (fn == builtin_consp) op = OP_CONSP;
        else if (fn == builtin_symbolp) op = OP_SYMBOLP;
        else if (fn == builtin_listp) op = OP_LISTP;
        else if (fn == builtin_stringp) op = OP_STRINGP;
        else if (fn == builtin_numberp) op = OP_NUMBERP;
        else if (fn == builtin_integerp) op = OP_INTEGERP;
        else if (fn == builtin_keywordp) op = OP_KEYWORDP;
        else if (fn == builtin_functionp) op = OP_FUNCTIONP;
        else if (fn == builtin_length) op = OP_LENGTH;
        else if (fn == builtin_last) op = OP_LAST;
        else if (fn == builtin_one_plus) op = OP_ADD1;
        else if (fn == builtin_one_minus) op = OP_SUB1;
        else if (fn == builtin_sub) op = OP_NEG;
        else if (fn == builtin_zerop) op = OP_ZEROP;
        else if (fn == builtin_plusp) op = OP_PLUSP;
        else if (fn == builtin_minusp) op = OP_MINUSP;
        else if (fn == builtin_copy_list) op = OP_COPY_LIST;
        else if (fn == builtin_nreverse) op = OP_NREVERSE;
        else if (fn == builtin_reverse) op = OP_REVERSE;
        if (op != OP_NOP) {
            if (!cc_emit_expr(cc, CAR(args), false, false))
                return false;
            cc_emit(cc, INST(op, 0, 0));
            if (want_mv)
                cc_emit(cc, INST(OP_MV1, 0, 0));
            return !cc->failed;
        }
    } else if (argc == 2) {
        if (fn == builtin_nth && intp(CAR(args)) && INTVAL(CAR(args)) >= 0 && INTVAL(CAR(args)) <= 255) {
            lisp_value target = CAR(CDR(args));
            bool bx = false;
            int slot = symbolp(target) && !(SYM(target)->h.flags & (SYM_SPECIAL | SYM_KEYWORD)) &&
                target != NIL_VAL && target != T_VAL ? cc_find_local(cc, target, BIND_VAR, &bx) : -1;
            if (slot >= 0 && !bx && slot <= 255) {
                cc_emit(cc, INST(OP_GET_LOCAL_NTH_IMM, (uint8_t)slot, (uint16_t)INTVAL(CAR(args))));
            } else {
                if (!cc_emit_expr(cc, target, false, false))
                    return false;
                cc_emit(cc, INST(OP_NTH_IMM, (uint8_t)INTVAL(CAR(args)), 0));
            }
            if (want_mv)
                cc_emit(cc, INST(OP_MV1, 0, 0));
            return !cc->failed;
        }
        if (fn == builtin_getf) {
            lisp_value key_form = CAR(CDR(args));
            lisp_value key_const = VAL(lisp.sym_unbound);
            if (symbolp(key_form) && (SYM(key_form)->h.flags & SYM_KEYWORD))
                key_const = key_form;
            else if (consp(key_form) && CAR(key_form) == VAL(lisp.sym_quote) &&
                    consp(CDR(key_form)) && nilp(CDR(CDR(key_form))) && symbolp(CAR(CDR(key_form))))
                key_const = CAR(CDR(key_form));
            if (key_const != VAL(lisp.sym_unbound)) {
                lisp_value target = CAR(args);
                bool bx = false;
                int slot = symbolp(target) && !(SYM(target)->h.flags & (SYM_SPECIAL | SYM_KEYWORD)) &&
                    target != NIL_VAL && target != T_VAL ? cc_find_local(cc, target, BIND_VAR, &bx) : -1;
                if (slot >= 0 && !bx && slot <= 255) {
                    cc_emit(cc, INST(OP_GET_LOCAL_GETF_CONST, (uint8_t)slot, cc_add_const(cc, key_const)));
                } else {
                    if (!cc_emit_expr(cc, target, false, false))
                        return false;
                    cc_emit(cc, INST(OP_GETF_CONST, 0, cc_add_const(cc, key_const)));
                }
                if (want_mv)
                    cc_emit(cc, INST(OP_MV1, 0, 0));
                return !cc->failed;
            }
        }
        if (fn == builtin_eq) {
            lisp_value b = CAR(CDR(args));
            lisp_value kconst = VAL(lisp.sym_unbound);
            if (b == NIL_VAL || b == T_VAL || (symbolp(b) && (SYM(b)->h.flags & SYM_KEYWORD)))
                kconst = b;
            else if (consp(b) && CAR(b) == VAL(lisp.sym_quote) && consp(CDR(b)) && nilp(CDR(CDR(b))))
                kconst = CAR(CDR(b));
            if (kconst != VAL(lisp.sym_unbound)) {
                if (!cc_emit_expr(cc, CAR(args), false, false))
                    return false;
                cc_emit(cc, INST(OP_EQ_CONST, 0, cc_add_const(cc, kconst)));
                if (want_mv)
                    cc_emit(cc, INST(OP_MV1, 0, 0));
                return !cc->failed;
            }
        }
        if (fn == builtin_cons) op = OP_CONS;
        else if (fn == builtin_nth) op = OP_NTH;
        else if (fn == builtin_nthcdr) op = OP_NTHCDR;
        else if (fn == builtin_eq) op = OP_EQ;
        else if (fn == builtin_eql) op = OP_EQL;
        else if (fn == builtin_equal) op = OP_EQUAL;
        else if (fn == builtin_add) op = OP_ADD2;
        else if (fn == builtin_sub) op = OP_SUB2;
        else if (fn == builtin_mul) op = OP_MUL2;
        else if (fn == builtin_lt) op = OP_LT2;
        else if (fn == builtin_le) op = OP_LE2;
        else if (fn == builtin_gt) op = OP_GT2;
        else if (fn == builtin_ge) op = OP_GE2;
        else if (fn == builtin_num_eq) op = OP_NUMEQ2;
        else if (fn == builtin_num_ne) op = OP_NUMNE2;
        else if (fn == builtin_min) op = OP_MIN2;
        else if (fn == builtin_max) op = OP_MAX2;
        else if (fn == builtin_member) op = OP_MEMBER2;
        else if (fn == builtin_assoc) op = OP_ASSOC2;
        else if (fn == builtin_append) op = OP_APPEND2;
        else if (fn == builtin_string_eq) op = OP_STRING_EQ;
        if (op != OP_NOP) {
            if (!cc_emit_expr(cc, CAR(args), false, false))
                return false;
            if (!cc_emit_expr(cc, CAR(CDR(args)), false, false))
                return false;
            cc_emit(cc, INST(op, 0, 0));
            if (want_mv)
                cc_emit(cc, INST(OP_MV1, 0, 0));
            return !cc->failed;
        }
    }
    if (fn == builtin_getf && (argc == 2 || argc == 3)) {
        for (lisp_value a = args; consp(a); a = CDR(a))
            if (!cc_emit_expr(cc, CAR(a), false, false))
                return false;
        cc_emit(cc, INST(OP_GETF, (uint8_t)argc, 0));
        if (want_mv)
            cc_emit(cc, INST(OP_MV1, 0, 0));
        return !cc->failed;
    }
    if (fn == builtin_gethash && (argc == 2 || argc == 3)) {
        for (lisp_value a = args; consp(a); a = CDR(a))
            if (!cc_emit_expr(cc, CAR(a), false, false))
                return false;
        cc_emit(cc, INST(OP_GETHASH, (uint8_t)argc, 0));
        return !cc->failed;
    }
    if (fn == builtin_list && argc <= 255) {
        for (lisp_value a = args; consp(a); a = CDR(a))
            if (!cc_emit_expr(cc, CAR(a), false, false))
                return false;
        cc_emit(cc, INST(OP_LIST, (uint8_t)argc, 0));
        if (want_mv)
            cc_emit(cc, INST(OP_MV1, 0, 0));
        return !cc->failed;
    }
    if (fn == builtin_values && argc <= LISP_MAX_MV) {
        for (lisp_value a = args; consp(a); a = CDR(a))
            if (!cc_emit_expr(cc, CAR(a), false, false))
                return false;
        cc_emit(cc, INST(OP_VALUES, (uint8_t)argc, 0));
        return !cc->failed;
    }
    if (fn == builtin_funcall && argc >= 1 && argc <= 255) {
        for (lisp_value a = args; consp(a); a = CDR(a))
            if (!cc_emit_expr(cc, CAR(a), false, false))
                return false;
        cc_emit(cc, INST(OP_FUNCALL, (uint8_t)(argc - 1), 0));
        return !cc->failed;
    }
    if (fn == builtin_apply && argc >= 2 && argc <= 255) {
        for (lisp_value a = args; consp(a); a = CDR(a))
            if (!cc_emit_expr(cc, CAR(a), false, false))
                return false;
        cc_emit(cc, INST(OP_APPLY, (uint8_t)(argc - 1), 0));
        return !cc->failed;
    }
    if (fn == builtin_acons && argc == 3) {
        for (lisp_value a = args; consp(a); a = CDR(a))
            if (!cc_emit_expr(cc, CAR(a), false, false))
                return false;
        cc_emit(cc, INST(OP_ACONS, 0, 0));
        if (want_mv)
            cc_emit(cc, INST(OP_MV1, 0, 0));
        return !cc->failed;
    }
    struct lisp_function *bf = PTR(sym->function, lisp_function);
    if ((int)argc >= bf->min_args && (bf->max_args < 0 || (int)argc <= bf->max_args) && argc <= 255) {
        for (lisp_value a = args; consp(a); a = CDR(a))
            if (!cc_emit_expr(cc, CAR(a), false, false))
                return false;
        cc_emit(cc, INST(OP_CALL_BUILTIN, (uint8_t)argc, cc_add_const(cc, sym->function)));
        return !cc->failed;
    }
    return false;
}

static bool cc_emit_setf_pair(struct cc_fn *cc, lisp_value place, lisp_value val_form, bool keep_mv)
{
    if (symbolp(place)) {
        if (!cc_emit_expr(cc, val_form, keep_mv, false))
            return false;
        if (keep_mv)
            cc_emit(cc, INST(OP_MV_SAVE, 0, 0));
        if (!cc_emit_var_set(cc, place))
            return false;
        if (keep_mv)
            cc_emit(cc, INST(OP_MV_RESTORE, 0, 0));
        return !cc->failed;
    }
    if (!consp(place) || !symbolp(CAR(place)))
        return false;
    struct lisp_symbol *head = SYM(CAR(place));
    lisp_value pargs = CDR(place);
    if (TYPE(head->function) != LT_FUNCTION || PTR(head->function, lisp_function)->kind != FN_BUILTIN)
        return false;
    lisp_primitive fn = PTR(head->function, lisp_function)->fn;
    if (fn == builtin_getf && consp(pargs) && consp(CDR(pargs))) {
        lisp_value parent = CAR(pargs);
        if (symbolp(parent)) {
            if (!cc_emit_var_get(cc, parent, false))
                return false;
            if (!cc_emit_expr(cc, CAR(CDR(pargs)), false, false))
                return false;
            if (!cc_emit_expr(cc, val_form, keep_mv, false))
                return false;
            if (keep_mv)
                cc_emit(cc, INST(OP_MV_SAVE, 0, 0));
            uint16_t vtmp = cc_alloc_slot(cc);
            cc_emit(cc, INST(OP_SET_LOCAL, 0, vtmp));
            cc_emit(cc, INST(OP_SETF_GETF, 0, 0));
            if (!cc_emit_var_set(cc, parent))
                return false;
            cc_emit(cc, INST(OP_POP, 0, 0));
            cc_emit(cc, INST(OP_GET_LOCAL, 0, vtmp));
            if (keep_mv)
                cc_emit(cc, INST(OP_MV_RESTORE, 0, 0));
            return !cc->failed;
        }
        if (consp(parent) && symbolp(CAR(parent)) &&
                TYPE(SYM(CAR(parent))->function) == LT_FUNCTION &&
                PTR(SYM(CAR(parent))->function, lisp_function)->kind == FN_BUILTIN &&
                PTR(SYM(CAR(parent))->function, lisp_function)->fn == builtin_getf &&
                consp(CDR(parent)) && consp(CDR(CDR(parent))) && symbolp(CAR(CDR(parent)))) {
            lisp_value root = CAR(CDR(parent));
            uint16_t k1 = cc_alloc_slot(cc);
            uint16_t k2 = cc_alloc_slot(cc);
            uint16_t vtmp = cc_alloc_slot(cc);
            if (!cc_emit_expr(cc, CAR(CDR(CDR(parent))), false, false))
                return false;
            cc_emit(cc, INST(OP_SET_LOCAL_POP, 0, k1));
            if (!cc_emit_expr(cc, CAR(CDR(pargs)), false, false))
                return false;
            cc_emit(cc, INST(OP_SET_LOCAL_POP, 0, k2));
            if (!cc_emit_expr(cc, val_form, keep_mv, false))
                return false;
            if (keep_mv)
                cc_emit(cc, INST(OP_MV_SAVE, 0, 0));
            cc_emit(cc, INST(OP_SET_LOCAL_POP, 0, vtmp));
            if (!cc_emit_var_get(cc, root, false))
                return false;
            cc_emit(cc, INST(OP_GET_LOCAL, 0, k1));
            if (!cc_emit_var_get(cc, root, false))
                return false;
            cc_emit(cc, INST(OP_GET_LOCAL, 0, k1));
            cc_emit(cc, INST(OP_GETF, 2, 0));
            cc_emit(cc, INST(OP_GET_LOCAL, 0, k2));
            cc_emit(cc, INST(OP_GET_LOCAL, 0, vtmp));
            cc_emit(cc, INST(OP_SETF_GETF, 0, 0));
            cc_emit(cc, INST(OP_SETF_GETF, 0, 0));
            if (!cc_emit_var_set(cc, root))
                return false;
            cc_emit(cc, INST(OP_POP, 0, 0));
            cc_emit(cc, INST(OP_GET_LOCAL, 0, vtmp));
            if (keep_mv)
                cc_emit(cc, INST(OP_MV_RESTORE, 0, 0));
            return !cc->failed;
        }
        return false;
    }
    if (fn == builtin_gethash && consp(pargs) && consp(CDR(pargs))) {
        if (!cc_emit_expr(cc, CAR(pargs), false, false))
            return false;
        if (!cc_emit_expr(cc, CAR(CDR(pargs)), false, false))
            return false;
        if (!cc_emit_expr(cc, val_form, keep_mv, false))
            return false;
        if (keep_mv)
            cc_emit(cc, INST(OP_MV_SAVE, 0, 0));
        cc_emit(cc, INST(OP_SETF_GETHASH, 0, 0));
        if (keep_mv)
            cc_emit(cc, INST(OP_MV_RESTORE, 0, 0));
        return !cc->failed;
    }
    if ((fn == builtin_cdr || fn == builtin_rest) && consp(pargs)) {
        if (!cc_emit_expr(cc, CAR(pargs), false, false))
            return false;
        if (!cc_emit_expr(cc, val_form, keep_mv, false))
            return false;
        if (keep_mv)
            cc_emit(cc, INST(OP_MV_SAVE, 0, 0));
        cc_emit(cc, INST(OP_SETF_CDR, 0, 0));
        if (keep_mv)
            cc_emit(cc, INST(OP_MV_RESTORE, 0, 0));
        return !cc->failed;
    }
    if (fn == builtin_nth && consp(pargs) && consp(CDR(pargs))) {
        if (!cc_emit_expr(cc, CAR(pargs), false, false))
            return false;
        if (!cc_emit_expr(cc, CAR(CDR(pargs)), false, false))
            return false;
        if (!cc_emit_expr(cc, val_form, keep_mv, false))
            return false;
        if (keep_mv)
            cc_emit(cc, INST(OP_MV_SAVE, 0, 0));
        cc_emit(cc, INST(OP_SETF_NTH, 0, 0));
        if (keep_mv)
            cc_emit(cc, INST(OP_MV_RESTORE, 0, 0));
        return !cc->failed;
    }
    return false;
}

static bool cc_emit_quasiquote(struct cc_fn *cc, lisp_value form, int depth)
{
    if (!consp(form)) {
        if (form == NIL_VAL)
            cc_emit(cc, INST(OP_NIL, 0, 0));
        else if (form == T_VAL)
            cc_emit(cc, INST(OP_T, 0, 0));
        else
            cc_emit(cc, INST(OP_CONST, 0, cc_add_const(cc, form)));
        return !cc->failed;
    }
    lisp_value head = CAR(form);
    if (head == VAL(lisp.sym_unquote) && depth == 0)
        return cc_emit_expr(cc, CAR(CDR(form)), false, false);
    if (head == VAL(lisp.sym_quasiquote)) {
        cc_emit(cc, INST(OP_CONST, 0, cc_add_const(cc, VAL(lisp.sym_quasiquote))));
        if (!cc_emit_quasiquote(cc, CAR(CDR(form)), depth + 1))
            return false;
        cc_emit(cc, INST(OP_LIST, 2, 0));
        return !cc->failed;
    }
    if (consp(head) && CAR(head) == VAL(lisp.sym_unquote_splicing) && depth == 0) {
        if (!cc_emit_expr(cc, CAR(CDR(head)), false, false))
            return false;
        if (!cc_emit_quasiquote(cc, CDR(form), depth))
            return false;
        cc_emit(cc, INST(OP_APPEND2, 0, 0));
        return !cc->failed;
    }
    if (!cc_emit_quasiquote(cc, head, depth))
        return false;
    if (!cc_emit_quasiquote(cc, CDR(form), depth))
        return false;
    cc_emit(cc, INST(OP_CONS, 0, 0));
    return !cc->failed;
}

static bool cc_emit_special(struct cc_fn *cc, lisp_value form, uint32_t id, bool want_mv, bool tail)
{
    lisp_value args = CDR(form);
    switch (id) {
    case SF_QUOTE: {
        lisp_value v = CAR(args);
        if (v == NIL_VAL)
            cc_emit(cc, INST(OP_NIL, 0, 0));
        else if (v == T_VAL)
            cc_emit(cc, INST(OP_T, 0, 0));
        else
            cc_emit(cc, INST(OP_CONST, 0, cc_add_const(cc, v)));
        if (want_mv)
            cc_emit(cc, INST(OP_MV1, 0, 0));
        return !cc->failed;
    }
    case SF_FUNCTION: {
        lisp_value target = CAR(args);
        if (symbolp(target)) {
            if (!cc_emit_func_get(cc, target))
                return false;
            if (want_mv)
                cc_emit(cc, INST(OP_MV1, 0, 0));
            return !cc->failed;
        }
        if (consp(target) && symbolp(CAR(target)) && SYM(CAR(target))->special_id == SF_LAMBDA) {
            if (!consp(CDR(target)))
                return false;
            if (!cc_emit_thunk(cc, CAR(CDR(target)), CDR(CDR(target))))
                return false;
            if (want_mv)
                cc_emit(cc, INST(OP_MV1, 0, 0));
            return !cc->failed;
        }
        return false;
    }
    case SF_LAMBDA:
        if (!consp(args))
            return false;
        if (!cc_emit_thunk(cc, CAR(args), CDR(args)))
            return false;
        if (want_mv)
            cc_emit(cc, INST(OP_MV1, 0, 0));
        return !cc->failed;
    case SF_IF: {
        if (!consp(args) || !consp(CDR(args)))
            return false;
        if (!cc_emit_expr(cc, CAR(args), false, false))
            return false;
        int j_else = cc_emit_jmp(cc, OP_JMP_NIL);
        if (!cc_emit_expr(cc, CAR(CDR(args)), want_mv, tail))
            return false;
        int j_end = cc_emit_jmp(cc, OP_JMP);
        cc_patch_jmp(cc, j_else);
        if (consp(CDR(CDR(args)))) {
            if (!cc_emit_expr(cc, CAR(CDR(CDR(args))), want_mv, tail))
                return false;
        } else {
            cc_emit(cc, INST(OP_NIL, 0, 0));
            if (want_mv)
                cc_emit(cc, INST(OP_MV1, 0, 0));
        }
        cc_patch_jmp(cc, j_end);
        return !cc->failed;
    }
    case SF_WHEN:
    case SF_UNLESS: {
        if (!consp(args))
            return false;
        if (!cc_emit_expr(cc, CAR(args), false, false))
            return false;
        int j_skip = cc_emit_jmp(cc, id == SF_UNLESS ? OP_JMP_NOT_NIL : OP_JMP_NIL);
        if (!cc_emit_body(cc, CDR(args), want_mv, tail))
            return false;
        int j_end = cc_emit_jmp(cc, OP_JMP);
        cc_patch_jmp(cc, j_skip);
        cc_emit(cc, INST(OP_NIL, 0, 0));
        if (want_mv)
            cc_emit(cc, INST(OP_MV1, 0, 0));
        cc_patch_jmp(cc, j_end);
        return !cc->failed;
    }
    case SF_PROGN:
        return cc_emit_body(cc, args, want_mv, tail);
    case SF_PROG1: {
        if (!consp(args)) {
            cc_emit(cc, INST(OP_NIL, 0, 0));
            if (want_mv)
                cc_emit(cc, INST(OP_MV1, 0, 0));
            return !cc->failed;
        }
        if (!cc_emit_expr(cc, CAR(args), want_mv, false))
            return false;
        if (want_mv)
            cc_emit(cc, INST(OP_MV_SAVE, 0, 0));
        for (lisp_value p = CDR(args); consp(p); p = CDR(p)) {
            if (!cc_emit_expr(cc, CAR(p), false, false))
                return false;
            cc_emit(cc, INST(OP_POP, 0, 0));
        }
        if (want_mv)
            cc_emit(cc, INST(OP_MV_RESTORE, 0, 0));
        return !cc->failed;
    }
    case SF_AND: {
        if (nilp(args)) {
            cc_emit(cc, INST(OP_T, 0, 0));
            if (want_mv)
                cc_emit(cc, INST(OP_MV1, 0, 0));
            return !cc->failed;
        }
        int jmps[128];
        int nj = 0;
        for (lisp_value p = args; consp(p); p = CDR(p)) {
            bool last = nilp(CDR(p));
            if (!cc_emit_expr(cc, CAR(p), last ? want_mv : false, last ? tail : false))
                return false;
            if (!last) {
                if (nj >= 128)
                    return false;
                jmps[nj++] = cc_emit_jmp(cc, OP_JMP_NIL_KEEP);
            }
        }
        int j_end = cc_emit_jmp(cc, OP_JMP);
        for (int i = 0; i < nj; i++)
            cc_patch_jmp(cc, jmps[i]);
        if (want_mv)
            cc_emit(cc, INST(OP_MV1, 0, 0));
        cc_patch_jmp(cc, j_end);
        return !cc->failed;
    }
    case SF_OR: {
        if (nilp(args)) {
            cc_emit(cc, INST(OP_NIL, 0, 0));
            if (want_mv)
                cc_emit(cc, INST(OP_MV1, 0, 0));
            return !cc->failed;
        }
        int jmps[128];
        int nj = 0;
        for (lisp_value p = args; consp(p); p = CDR(p)) {
            bool last = nilp(CDR(p));
            if (!cc_emit_expr(cc, CAR(p), last ? want_mv : false, last ? tail : false))
                return false;
            if (!last) {
                if (nj >= 128)
                    return false;
                jmps[nj++] = cc_emit_jmp(cc, OP_JMP_NOT_NIL_KEEP);
            }
        }
        int j_end = cc_emit_jmp(cc, OP_JMP);
        for (int i = 0; i < nj; i++)
            cc_patch_jmp(cc, jmps[i]);
        if (want_mv)
            cc_emit(cc, INST(OP_MV1, 0, 0));
        cc_patch_jmp(cc, j_end);
        return !cc->failed;
    }
    case SF_COND: {
        int end_jmps[128];
        int nend = 0;
        for (lisp_value c = args; consp(c); c = CDR(c)) {
            lisp_value clause = CAR(c);
            if (!consp(clause))
                return false;
            bool has_body = consp(CDR(clause));
            if (!cc_emit_expr(cc, CAR(clause), !has_body && want_mv, false))
                return false;
            if (!has_body) {
                if (nend >= 128)
                    return false;
                end_jmps[nend++] = cc_emit_jmp(cc, OP_JMP_NOT_NIL_KEEP);
                cc_emit(cc, INST(OP_POP, 0, 0));
            } else {
                int j_next = cc_emit_jmp(cc, OP_JMP_NIL);
                if (!cc_emit_body(cc, CDR(clause), want_mv, tail))
                    return false;
                if (nend >= 128)
                    return false;
                end_jmps[nend++] = cc_emit_jmp(cc, OP_JMP);
                cc_patch_jmp(cc, j_next);
            }
        }
        cc_emit(cc, INST(OP_NIL, 0, 0));
        if (want_mv)
            cc_emit(cc, INST(OP_MV1, 0, 0));
        for (int i = 0; i < nend; i++)
            cc_patch_jmp(cc, end_jmps[i]);
        return !cc->failed;
    }
    case SF_CASE:
    case SF_ECASE: {
        if (!consp(args))
            return false;
        if (!cc_emit_expr(cc, CAR(args), false, false))
            return false;
        int end_jmps[128];
        int nend = 0;
        bool has_default = false;
        for (lisp_value c = CDR(args); consp(c); c = CDR(c)) {
            lisp_value clause = CAR(c);
            if (!consp(clause))
                return false;
            lisp_value spec = CAR(clause);
            if (spec == T_VAL || eql_value(spec, VAL(lisp.sym_otherwise))) {
                cc_emit(cc, INST(OP_POP, 0, 0));
                if (!cc_emit_body(cc, CDR(clause), want_mv, tail))
                    return false;
                has_default = true;
                break;
            }
            cc_emit(cc, INST(OP_CASE_MATCH, 0, cc_add_const(cc, spec)));
            int j_next = cc_emit_jmp(cc, OP_JMP_NIL);
            cc_emit(cc, INST(OP_POP, 0, 0));
            if (!cc_emit_body(cc, CDR(clause), want_mv, tail))
                return false;
            if (nend >= 128)
                return false;
            end_jmps[nend++] = cc_emit_jmp(cc, OP_JMP);
            cc_patch_jmp(cc, j_next);
        }
        if (!has_default) {
            if (id == SF_ECASE) {
                cc_emit(cc, INST(OP_ECASE_FAIL, 0, cc_add_const(cc, form)));
            } else {
                cc_emit(cc, INST(OP_POP, 0, 0));
                cc_emit(cc, INST(OP_NIL, 0, 0));
                if (want_mv)
                    cc_emit(cc, INST(OP_MV1, 0, 0));
            }
        }
        for (int i = 0; i < nend; i++)
            cc_patch_jmp(cc, end_jmps[i]);
        return !cc->failed;
    }
    case SF_LET:
    case SF_LET_STAR: {
        if (!consp(args))
            return false;
        bool star = id == SF_LET_STAR;
        lisp_value bindings = CAR(args);
        uint32_t count = 0;
        if (!proper_list_len(bindings, &count) || count > 64)
            return false;
        uint32_t saved_vars = cc->var_count;
        uint16_t saved_slot = cc->next_slot;
        int dyn_count = 0;
        lisp_value names[64];
        bool is_dyn[64];
        uint16_t slots[64];
        bool boxed[64];
        uint32_t i = 0;
        if (!star) {
            for (lisp_value b = bindings; consp(b); b = CDR(b), i++) {
                lisp_value item = CAR(b);
                lisp_value name = consp(item) ? CAR(item) : item;
                lisp_value init = (consp(item) && consp(CDR(item))) ? CAR(CDR(item)) : NIL_VAL;
                if (!symbolp(name))
                    return false;
                names[i] = name;
                is_dyn[i] = (SYM(name)->h.flags & SYM_SPECIAL) != 0;
                if (!cc_emit_expr(cc, init, false, false))
                    return false;
            }
            for (uint32_t k = 0; k < count; k++) {
                if (!is_dyn[k])
                    slots[k] = cc_push_var(cc, names[k], BIND_VAR, &boxed[k]);
            }
            for (uint32_t k = count; k-- > 0;) {
                if (is_dyn[k]) {
                    cc_emit(cc, INST(OP_BIND_DYN, 0, cc_add_const(cc, names[k])));
                    dyn_count++;
                } else {
                    cc_emit(cc, INST(OP_SET_LOCAL_POP, 0, slots[k]));
                    if (boxed[k])
                        cc_emit(cc, INST(OP_BOX_LOCAL, 0, slots[k]));
                }
            }
        } else {
            for (lisp_value b = bindings; consp(b); b = CDR(b), i++) {
                lisp_value item = CAR(b);
                lisp_value name = consp(item) ? CAR(item) : item;
                lisp_value init = (consp(item) && consp(CDR(item))) ? CAR(CDR(item)) : NIL_VAL;
                if (!symbolp(name))
                    return false;
                if (!cc_emit_expr(cc, init, false, false))
                    return false;
                if (SYM(name)->h.flags & SYM_SPECIAL) {
                    cc_emit(cc, INST(OP_BIND_DYN, 0, cc_add_const(cc, name)));
                    dyn_count++;
                } else {
                    bool bx = false;
                    uint16_t slot = cc_push_var(cc, name, BIND_VAR, &bx);
                    cc_emit(cc, INST(OP_SET_LOCAL_POP, 0, slot));
                    if (bx)
                        cc_emit(cc, INST(OP_BOX_LOCAL, 0, slot));
                }
            }
        }
        cc->dyn_depth += dyn_count;
        bool ok = cc_emit_body(cc, CDR(args), want_mv, tail && dyn_count == 0);
        cc->dyn_depth -= dyn_count;
        if (dyn_count > 0)
            cc_emit(cc, INST(OP_UNBIND_DYN, 0, dyn_count));
        cc->var_count = saved_vars;
        cc->next_slot = saved_slot;
        return ok && !cc->failed;
    }
    case SF_SETQ: {
        uint32_t count = 0;
        if (!proper_list_len(args, &count) || (count & 1))
            return false;
        if (count == 0) {
            cc_emit(cc, INST(OP_NIL, 0, 0));
            if (want_mv)
                cc_emit(cc, INST(OP_MV1, 0, 0));
            return !cc->failed;
        }
        for (lisp_value p = args; consp(p); p = CDR(CDR(p))) {
            lisp_value target = CAR(p);
            if (!symbolp(target))
                return false;
            bool last = nilp(CDR(CDR(p)));
            if (!cc_emit_expr(cc, CAR(CDR(p)), false, false))
                return false;
            if (!cc_emit_var_set(cc, target))
                return false;
            if (!last)
                cc_emit(cc, INST(OP_POP, 0, 0));
        }
        if (want_mv)
            cc_emit(cc, INST(OP_MV1, 0, 0));
        return !cc->failed;
    }
    case SF_SETF: {
        uint32_t count = 0;
        if (!proper_list_len(args, &count) || (count & 1))
            return false;
        if (count == 0) {
            cc_emit(cc, INST(OP_NIL, 0, 0));
            if (want_mv)
                cc_emit(cc, INST(OP_MV1, 0, 0));
            return !cc->failed;
        }
        for (lisp_value p = args; consp(p); p = CDR(CDR(p))) {
            bool last = nilp(CDR(CDR(p)));
            if (!cc_emit_setf_pair(cc, CAR(p), CAR(CDR(p)), last && want_mv))
                return false;
            if (!last)
                cc_emit(cc, INST(OP_POP, 0, 0));
        }
        return !cc->failed;
    }
    case SF_PUSH: {
        if (!consp(args) || !consp(CDR(args)))
            return false;
        lisp_value place = CAR(CDR(args));
        if (symbolp(place)) {
            if (!cc_emit_expr(cc, CAR(args), false, false))
                return false;
            if (!cc_emit_var_get(cc, place, false))
                return false;
            cc_emit(cc, INST(OP_CONS, 0, 0));
            if (!cc_emit_var_set(cc, place))
                return false;
            if (want_mv)
                cc_emit(cc, INST(OP_MV1, 0, 0));
            return !cc->failed;
        }
        if (consp(place) && symbolp(CAR(place)) &&
                TYPE(SYM(CAR(place))->function) == LT_FUNCTION &&
                PTR(SYM(CAR(place))->function, lisp_function)->kind == FN_BUILTIN &&
                PTR(SYM(CAR(place))->function, lisp_function)->fn == builtin_getf &&
                consp(CDR(place)) && consp(CDR(CDR(place))) && symbolp(CAR(CDR(place)))) {
            lisp_value var = CAR(CDR(place));
            uint16_t item_s = cc_alloc_slot(cc);
            uint16_t key_s = cc_alloc_slot(cc);
            uint16_t res_s = cc_alloc_slot(cc);
            if (!cc_emit_expr(cc, CAR(args), false, false))
                return false;
            cc_emit(cc, INST(OP_SET_LOCAL_POP, 0, item_s));
            if (!cc_emit_expr(cc, CAR(CDR(CDR(place))), false, false))
                return false;
            cc_emit(cc, INST(OP_SET_LOCAL_POP, 0, key_s));
            if (!cc_emit_var_get(cc, var, false))
                return false;
            cc_emit(cc, INST(OP_GET_LOCAL, 0, key_s));
            cc_emit(cc, INST(OP_GET_LOCAL, 0, item_s));
            if (!cc_emit_var_get(cc, var, false))
                return false;
            cc_emit(cc, INST(OP_GET_LOCAL, 0, key_s));
            cc_emit(cc, INST(OP_GETF, 2, 0));
            cc_emit(cc, INST(OP_CONS, 0, 0));
            cc_emit(cc, INST(OP_SET_LOCAL, 0, res_s));
            cc_emit(cc, INST(OP_SETF_GETF, 0, 0));
            if (!cc_emit_var_set(cc, var))
                return false;
            cc_emit(cc, INST(OP_POP, 0, 0));
            cc_emit(cc, INST(OP_GET_LOCAL, 0, res_s));
            if (want_mv)
                cc_emit(cc, INST(OP_MV1, 0, 0));
            return !cc->failed;
        }
        return false;
    }
    case SF_PUSHNEW: {
        if (!consp(args) || !consp(CDR(args)) || !symbolp(CAR(CDR(args))))
            return false;
        if (consp(CDR(CDR(args))))
            return false;
        lisp_value place = CAR(CDR(args));
        if (!cc_emit_expr(cc, CAR(args), false, false))
            return false;
        if (!cc_emit_var_get(cc, place, false))
            return false;
        cc_emit(cc, INST(OP_PUSHNEW_EQ, 0, 0));
        if (!cc_emit_var_set(cc, place))
            return false;
        if (want_mv)
            cc_emit(cc, INST(OP_MV1, 0, 0));
        return !cc->failed;
    }
    case SF_POP: {
        if (!consp(args) || !symbolp(CAR(args)))
            return false;
        lisp_value place = CAR(args);
        uint16_t tmp = cc_alloc_slot(cc);
        if (!cc_emit_var_get(cc, place, false))
            return false;
        cc_emit(cc, INST(OP_SET_LOCAL, 0, tmp));
        cc_emit(cc, INST(OP_CDR, 0, 0));
        if (!cc_emit_var_set(cc, place))
            return false;
        cc_emit(cc, INST(OP_POP, 0, 0));
        cc_emit(cc, INST(OP_GET_LOCAL, 0, tmp));
        cc_emit(cc, INST(OP_CAR, 0, 0));
        if (want_mv)
            cc_emit(cc, INST(OP_MV1, 0, 0));
        return !cc->failed;
    }
    case SF_INCF:
    case SF_DECF: {
        if (!consp(args) || !symbolp(CAR(args)))
            return false;
        lisp_value place = CAR(args);
        if (!cc_emit_var_get(cc, place, false))
            return false;
        if (consp(CDR(args))) {
            if (!cc_emit_expr(cc, CAR(CDR(args)), false, false))
                return false;
            cc_emit(cc, INST(id == SF_DECF ? OP_SUB2 : OP_ADD2, 0, 0));
        } else {
            cc_emit(cc, INST(id == SF_DECF ? OP_SUB1 : OP_ADD1, 0, 0));
        }
        if (!cc_emit_var_set(cc, place))
            return false;
        if (want_mv)
            cc_emit(cc, INST(OP_MV1, 0, 0));
        return !cc->failed;
    }
    case SF_ROTATEF: {
        uint32_t count = 0;
        if (!proper_list_len(args, &count) || count > 16)
            return false;
        if (count >= 2) {
            lisp_value syms[16];
            uint32_t i = 0;
            for (lisp_value p = args; consp(p); p = CDR(p), i++) {
                if (!symbolp(CAR(p)))
                    return false;
                syms[i] = CAR(p);
            }
            uint16_t first_tmp = cc_alloc_slot(cc);
            if (!cc_emit_var_get(cc, syms[0], false))
                return false;
            cc_emit(cc, INST(OP_SET_LOCAL_POP, 0, first_tmp));
            for (uint32_t k = 0; k + 1 < count; k++) {
                if (!cc_emit_var_get(cc, syms[k + 1], false))
                    return false;
                if (!cc_emit_var_set(cc, syms[k]))
                    return false;
                cc_emit(cc, INST(OP_POP, 0, 0));
            }
            cc_emit(cc, INST(OP_GET_LOCAL, 0, first_tmp));
            if (!cc_emit_var_set(cc, syms[count - 1]))
                return false;
            cc_emit(cc, INST(OP_POP, 0, 0));
        }
        cc_emit(cc, INST(OP_NIL, 0, 0));
        if (want_mv)
            cc_emit(cc, INST(OP_MV1, 0, 0));
        return !cc->failed;
    }
    case SF_DOLIST: {
        if (!consp(args) || !consp(CAR(args)) || !symbolp(CAR(CAR(args))) || !consp(CDR(CAR(args))))
            return false;
        lisp_value spec = CAR(args);
        lisp_value var = CAR(spec);
        if (SYM(var)->h.flags & SYM_SPECIAL)
            return false;
        uint32_t saved_vars = cc->var_count;
        uint16_t saved_slot = cc->next_slot;
        if (!cc_emit_expr(cc, CAR(CDR(spec)), false, false))
            return false;
        uint16_t list_slot = cc_alloc_slot(cc);
        cc_emit(cc, INST(OP_SET_LOCAL_POP, 0, list_slot));
        bool boxed = false;
        uint16_t var_slot = cc_push_var(cc, var, BIND_VAR, &boxed);
        cc_emit(cc, INST(OP_NIL, 0, 0));
        cc_emit(cc, INST(OP_SET_LOCAL_POP, 0, var_slot));
        if (boxed)
            cc_emit(cc, INST(OP_BOX_LOCAL, 0, var_slot));
        if (cc->loop_depth >= CC_MAX_LOOPS)
            return false;
        int loop_idx = cc->loop_depth++;
        cc->loops[loop_idx].break_count = 0;
        cc->loops[loop_idx].dyn_depth = cc->dyn_depth;
        cc->loops[loop_idx].stack_depth = cc->stack_depth;
        int loop_start = (int)cc->ins_count;
        int j_exit = (int)cc->ins_count;
        cc_emit(cc, INST(OP_LOOP_STEP_IN, list_slot, 0));
        if (boxed) {
            cc_emit(cc, INST(OP_SET_BOX, 0, var_slot));
            cc_emit(cc, INST(OP_POP, 0, 0));
        } else {
            cc_emit(cc, INST(OP_SET_LOCAL_POP, 0, var_slot));
        }
        for (lisp_value b = CDR(args); consp(b); b = CDR(b)) {
            if (!cc_emit_expr(cc, CAR(b), false, false))
                return false;
            cc_emit(cc, INST(OP_POP, 0, 0));
        }
        cc_emit_jmp_to(cc, OP_JMP, loop_start);
        cc_patch_jmp(cc, j_exit);
        if (consp(CDR(CDR(spec)))) {
            if (boxed) {
                cc_emit(cc, INST(OP_NIL, 0, 0));
                cc_emit(cc, INST(OP_SET_BOX, 0, var_slot));
                cc_emit(cc, INST(OP_POP, 0, 0));
            } else {
                cc_emit(cc, INST(OP_NIL, 0, 0));
                cc_emit(cc, INST(OP_SET_LOCAL_POP, 0, var_slot));
            }
            if (!cc_emit_expr(cc, CAR(CDR(CDR(spec))), want_mv, false))
                return false;
        } else {
            cc_emit(cc, INST(OP_NIL, 0, 0));
            if (want_mv)
                cc_emit(cc, INST(OP_MV1, 0, 0));
        }
        for (int i = 0; i < cc->loops[loop_idx].break_count; i++)
            cc_patch_jmp(cc, cc->loops[loop_idx].breaks[i]);
        cc->loop_depth--;
        cc->var_count = saved_vars;
        cc->next_slot = saved_slot;
        return !cc->failed;
    }
    case SF_DOTIMES: {
        if (!consp(args) || !consp(CAR(args)) || !symbolp(CAR(CAR(args))) || !consp(CDR(CAR(args))))
            return false;
        lisp_value spec = CAR(args);
        lisp_value var = CAR(spec);
        if (SYM(var)->h.flags & SYM_SPECIAL)
            return false;
        uint32_t saved_vars = cc->var_count;
        uint16_t saved_slot = cc->next_slot;
        if (!cc_emit_expr(cc, CAR(CDR(spec)), false, false))
            return false;
        uint16_t bound_slot = cc_alloc_slot(cc);
        cc_emit(cc, INST(OP_SET_LOCAL_POP, 0, bound_slot));
        bool boxed = false;
        uint16_t var_slot = cc_push_var(cc, var, BIND_VAR, &boxed);
        cc_emit(cc, INST(OP_CONST, 0, cc_add_const(cc, make_int(0))));
        cc_emit(cc, INST(OP_SET_LOCAL_POP, 0, var_slot));
        if (boxed)
            cc_emit(cc, INST(OP_BOX_LOCAL, 0, var_slot));
        if (cc->loop_depth >= CC_MAX_LOOPS)
            return false;
        int loop_idx = cc->loop_depth++;
        cc->loops[loop_idx].break_count = 0;
        cc->loops[loop_idx].dyn_depth = cc->dyn_depth;
        cc->loops[loop_idx].stack_depth = cc->stack_depth;
        int loop_start = (int)cc->ins_count;
        cc_emit(cc, INST(OP_STEP, 0, 0));
        cc_emit(cc, INST(boxed ? OP_GET_BOX : OP_GET_LOCAL, 0, var_slot));
        cc_emit(cc, INST(OP_GET_LOCAL, 0, bound_slot));
        cc_emit(cc, INST(OP_GE2, 0, 0));
        int j_exit = cc_emit_jmp(cc, OP_JMP_NOT_NIL);
        for (lisp_value b = CDR(args); consp(b); b = CDR(b)) {
            if (!cc_emit_expr(cc, CAR(b), false, false))
                return false;
            cc_emit(cc, INST(OP_POP, 0, 0));
        }
        cc_emit(cc, INST(boxed ? OP_GET_BOX : OP_GET_LOCAL, 0, var_slot));
        cc_emit(cc, INST(OP_ADD1, 0, 0));
        if (boxed) {
            cc_emit(cc, INST(OP_SET_BOX, 0, var_slot));
            cc_emit(cc, INST(OP_POP, 0, 0));
        } else {
            cc_emit(cc, INST(OP_SET_LOCAL_POP, 0, var_slot));
        }
        cc_emit_jmp_to(cc, OP_JMP, loop_start);
        cc_patch_jmp(cc, j_exit);
        if (consp(CDR(CDR(spec)))) {
            if (!cc_emit_expr(cc, CAR(CDR(CDR(spec))), want_mv, false))
                return false;
        } else {
            cc_emit(cc, INST(OP_NIL, 0, 0));
            if (want_mv)
                cc_emit(cc, INST(OP_MV1, 0, 0));
        }
        for (int i = 0; i < cc->loops[loop_idx].break_count; i++)
            cc_patch_jmp(cc, cc->loops[loop_idx].breaks[i]);
        cc->loop_depth--;
        cc->var_count = saved_vars;
        cc->next_slot = saved_slot;
        return !cc->failed;
    }
    case SF_LOOP:
        return cc_emit_loop(cc, args, want_mv);
    case SF_RETURN: {
        if (consp(args)) {
            if (!cc_emit_expr(cc, CAR(args), true, false))
                return false;
        } else {
            cc_emit(cc, INST(OP_NIL, 0, 0));
            cc_emit(cc, INST(OP_MV1, 0, 0));
        }
        if (cc->loop_depth > 0) {
            struct cc_loop *lp = &cc->loops[cc->loop_depth - 1];
            if (lp->break_count >= CC_MAX_BREAKS)
                return false;
            if (cc->dyn_depth > lp->dyn_depth)
                cc_emit(cc, INST(OP_UNBIND_DYN, 0, cc->dyn_depth - lp->dyn_depth));
            lp->breaks[lp->break_count++] = cc_emit_jmp(cc, OP_JMP);
            return !cc->failed;
        }
        cc_emit(cc, INST(OP_RETURN_FROM, 0, cc_add_const(cc, NIL_VAL)));
        return !cc->failed;
    }
    case SF_RETURN_FROM: {
        if (!consp(args) || !symbolp(CAR(args)))
            return false;
        lisp_value bname = CAR(args);
        if (consp(CDR(args))) {
            if (!cc_emit_expr(cc, CAR(CDR(args)), true, false))
                return false;
        } else {
            cc_emit(cc, INST(OP_NIL, 0, 0));
            cc_emit(cc, INST(OP_MV1, 0, 0));
        }
        if (bname == NIL_VAL && cc->loop_depth > 0) {
            struct cc_loop *lp = &cc->loops[cc->loop_depth - 1];
            if (lp->break_count >= CC_MAX_BREAKS)
                return false;
            if (cc->dyn_depth > lp->dyn_depth)
                cc_emit(cc, INST(OP_UNBIND_DYN, 0, cc->dyn_depth - lp->dyn_depth));
            lp->breaks[lp->break_count++] = cc_emit_jmp(cc, OP_JMP);
            return !cc->failed;
        }
        cc_emit(cc, INST(OP_RETURN_FROM, 0, cc_add_const(cc, bname)));
        return !cc->failed;
    }
    case SF_BLOCK: {
        if (!consp(args) || !symbolp(CAR(args)))
            return false;
        lisp_value bname = CAR(args);
        if (!cc_emit_thunk(cc, NIL_VAL, CDR(args)))
            return false;
        cc_emit(cc, INST(OP_BLOCK_THUNK, 0, cc_add_const(cc, bname)));
        return !cc->failed;
    }
    case SF_UNWIND_PROTECT: {
        if (!consp(args))
            return false;
        if (!cc_emit_thunk(cc, NIL_VAL, LIST1(CAR(args))))
            return false;
        if (!cc_emit_thunk(cc, NIL_VAL, CDR(args)))
            return false;
        cc_emit(cc, INST(OP_UNWIND_THUNK, 0, 0));
        return !cc->failed;
    }
    case SF_IGNORE_ERRORS: {
        if (!cc_emit_thunk(cc, NIL_VAL, args))
            return false;
        cc_emit(cc, INST(OP_IGNORE_ERRORS_THUNK, 0, 0));
        return !cc->failed;
    }
    case SF_HANDLER_CASE: {
        if (!consp(args))
            return false;
        if (!cc_emit_thunk(cc, NIL_VAL, LIST1(CAR(args))))
            return false;
        lisp_value clause = consp(CDR(args)) ? CAR(CDR(args)) : NIL_VAL;
        if (consp(clause) && consp(CDR(clause))) {
            lisp_value vars = CAR(CDR(clause));
            lisp_value hparams = consp(vars) ? LIST1(CAR(vars)) : LIST1(VAL(symbol_intern_c(PKG_TOMOE, "%COND")));
            if (!cc_emit_thunk(cc, hparams, CDR(CDR(clause))))
                return false;
        } else {
            cc_emit(cc, INST(OP_NIL, 0, 0));
        }
        cc_emit(cc, INST(OP_HANDLER_CASE_THUNK, 0, 0));
        return !cc->failed;
    }
    case SF_MULTIPLE_VALUE_BIND: {
        if (!consp(args) || !consp(CDR(args)))
            return false;
        lisp_value vars = CAR(args);
        uint32_t count = 0;
        if (!proper_list_len(vars, &count) || count > LISP_MAX_MV)
            return false;
        if (!cc_emit_expr(cc, CAR(CDR(args)), true, false))
            return false;
        uint32_t saved_vars = cc->var_count;
        uint16_t saved_slot = cc->next_slot;
        uint16_t first_slot = cc->next_slot;
        bool boxed[LISP_MAX_MV];
        uint32_t i = 0;
        for (lisp_value v = vars; consp(v); v = CDR(v), i++) {
            if (!symbolp(CAR(v)) || (SYM(CAR(v))->h.flags & SYM_SPECIAL))
                return false;
            cc_push_var(cc, CAR(v), BIND_VAR, &boxed[i]);
        }
        cc_emit(cc, INST(OP_MV_BIND, (uint8_t)count, first_slot));
        for (uint32_t k = 0; k < count; k++)
            if (boxed[k])
                cc_emit(cc, INST(OP_BOX_LOCAL, 0, first_slot + k));
        bool ok = cc_emit_body(cc, CDR(CDR(args)), want_mv, tail);
        cc->var_count = saved_vars;
        cc->next_slot = saved_slot;
        return ok && !cc->failed;
    }
    case SF_MULTIPLE_VALUE_SETQ: {
        if (!consp(args) || !consp(CDR(args)))
            return false;
        lisp_value vars = CAR(args);
        uint32_t count = 0;
        if (!proper_list_len(vars, &count) || count > LISP_MAX_MV)
            return false;
        if (!cc_emit_expr(cc, CAR(CDR(args)), true, false))
            return false;
        uint16_t tmp_base = cc->next_slot;
        for (uint32_t k = 0; k < count; k++)
            cc_alloc_slot(cc);
        cc_emit(cc, INST(OP_MV_BIND, (uint8_t)count, tmp_base));
        uint32_t i = 0;
        for (lisp_value v = vars; consp(v); v = CDR(v), i++) {
            if (!symbolp(CAR(v)))
                return false;
            cc_emit(cc, INST(OP_GET_LOCAL, 0, tmp_base + i));
            if (!cc_emit_var_set(cc, CAR(v)))
                return false;
            cc_emit(cc, INST(OP_POP, 0, 0));
        }
        if (count > 0)
            cc_emit(cc, INST(OP_GET_LOCAL, 0, tmp_base));
        else
            cc_emit(cc, INST(OP_NIL, 0, 0));
        if (want_mv)
            cc_emit(cc, INST(OP_MV1, 0, 0));
        cc->next_slot = tmp_base;
        return !cc->failed;
    }
    case SF_MULTIPLE_VALUE_LIST: {
        if (!consp(args))
            return false;
        if (!cc_emit_expr(cc, CAR(args), true, false))
            return false;
        cc_emit(cc, INST(OP_MV_LIST, 0, 0));
        if (want_mv)
            cc_emit(cc, INST(OP_MV1, 0, 0));
        return !cc->failed;
    }
    case SF_DESTRUCTURING_BIND: {
        if (!consp(args) || !consp(CDR(args)))
            return false;
        lisp_value pat = CAR(args);
        if (!cc_emit_expr(cc, CAR(CDR(args)), false, false))
            return false;
        uint32_t saved_vars = cc->var_count;
        uint16_t saved_slot = cc->next_slot;
        if (cc_is_simple_pattern(pat) && !cc_pattern_has_special(pat)) {
            cc_emit_simple_bind(cc, pat);
        } else if (consp(pat)) {
            uint16_t cur = cc_alloc_slot(cc);
            uint16_t keys_slot = cur;
            cc_emit(cc, INST(OP_SET_LOCAL_POP, 0, cur));
            int mode = 0;
            lisp_value p = pat;
            while (consp(p)) {
                lisp_value head = CAR(p);
                p = CDR(p);
                if (head == VAL(lisp.sym_rest) || head == VAL(lisp.sym_body)) {
                    if (!consp(p) || !cc_is_simple_pattern(CAR(p)) || cc_pattern_has_special(CAR(p)))
                        return false;
                    cc_emit(cc, INST(OP_GET_LOCAL, 0, cur));
                    cc_emit_simple_bind(cc, CAR(p));
                    p = CDR(p);
                    continue;
                }
                if (head == VAL(lisp.sym_optional)) { mode = 1; continue; }
                if (head == VAL(lisp.sym_key)) { mode = 2; keys_slot = cur; continue; }
                if (head == VAL(lisp.sym_allow_other_keys)) continue;
                if (head == VAL(lisp.sym_aux) || head == VAL(lisp.sym_whole) || head == VAL(lisp.sym_environment))
                    return false;
                if (mode == 0) {
                    if (!cc_is_simple_pattern(head) || cc_pattern_has_special(head))
                        return false;
                    cc_emit(cc, INST(OP_GET_LOCAL, 0, cur));
                    cc_emit(cc, INST(OP_CAR, 0, 0));
                    cc_emit_simple_bind(cc, head);
                    cc_emit(cc, INST(OP_GET_LOCAL, 0, cur));
                    cc_emit(cc, INST(OP_CDR, 0, 0));
                    cc_emit(cc, INST(OP_SET_LOCAL_POP, 0, cur));
                    continue;
                }
                lisp_value var = head, def_form = NIL_VAL, sup = NIL_VAL, kw = NIL_VAL;
                if (consp(head)) {
                    var = CAR(head);
                    if (consp(CDR(head))) def_form = CAR(CDR(head));
                    if (consp(CDR(head)) && consp(CDR(CDR(head)))) sup = CAR(CDR(CDR(head)));
                }
                if (mode == 2 && consp(var)) {
                    if (!consp(CDR(var)) || !symbolp(CAR(var)))
                        return false;
                    kw = CAR(var);
                    var = CAR(CDR(var));
                }
                if (!symbolp(var) || (SYM(var)->h.flags & SYM_SPECIAL))
                    return false;
                if (symbolp(sup) && !nilp(sup) && (SYM(sup)->h.flags & SYM_SPECIAL))
                    return false;
                if (mode == 1) {
                    bool vbx = false;
                    uint16_t vslot = cc_push_var(cc, var, BIND_VAR, &vbx);
                    bool sbx = false;
                    uint16_t sslot = 0;
                    bool has_sup = symbolp(sup) && !nilp(sup);
                    if (has_sup)
                        sslot = cc_push_var(cc, sup, BIND_VAR, &sbx);
                    cc_emit(cc, INST(OP_GET_LOCAL, 0, cur));
                    cc_emit(cc, INST(OP_CONSP, 0, 0));
                    int j_def = cc_emit_jmp(cc, OP_JMP_NIL);
                    cc_emit(cc, INST(OP_GET_LOCAL, 0, cur));
                    cc_emit(cc, INST(OP_CAR, 0, 0));
                    cc_emit(cc, INST(OP_SET_LOCAL_POP, 0, vslot));
                    cc_emit(cc, INST(OP_GET_LOCAL, 0, cur));
                    cc_emit(cc, INST(OP_CDR, 0, 0));
                    cc_emit(cc, INST(OP_SET_LOCAL_POP, 0, cur));
                    if (has_sup) {
                        cc_emit(cc, INST(OP_T, 0, 0));
                        cc_emit(cc, INST(OP_SET_LOCAL_POP, 0, sslot));
                    }
                    int j_done = cc_emit_jmp(cc, OP_JMP);
                    cc_patch_jmp(cc, j_def);
                    if (nilp(def_form))
                        cc_emit(cc, INST(OP_NIL, 0, 0));
                    else if (!cc_emit_expr(cc, def_form, false, false))
                        return false;
                    cc_emit(cc, INST(OP_SET_LOCAL_POP, 0, vslot));
                    if (has_sup) {
                        cc_emit(cc, INST(OP_NIL, 0, 0));
                        cc_emit(cc, INST(OP_SET_LOCAL_POP, 0, sslot));
                    }
                    cc_patch_jmp(cc, j_done);
                    if (vbx)
                        cc_emit(cc, INST(OP_BOX_LOCAL, 0, vslot));
                    if (has_sup && sbx)
                        cc_emit(cc, INST(OP_BOX_LOCAL, 0, sslot));
                } else {
                    if (nilp(kw))
                        kw = VAL(symbol_intern(PKG_KEYWORD, SYM(var)->name, SYM(var)->name_len, false));
                    bool has_sup = symbolp(sup) && !nilp(sup);
                    if (nilp(def_form) && !has_sup) {
                        bool vbx = false;
                        uint16_t vslot = cc_push_var(cc, var, BIND_VAR, &vbx);
                        cc_emit(cc, INST(OP_GET_LOCAL, 0, keys_slot));
                        cc_emit(cc, INST(OP_GETF_CONST, 0, cc_add_const(cc, kw)));
                        cc_emit(cc, INST(OP_SET_LOCAL_POP, 0, vslot));
                        if (vbx)
                            cc_emit(cc, INST(OP_BOX_LOCAL, 0, vslot));
                    } else {
                        bool vbx = false;
                        uint16_t vslot = cc_push_var(cc, var, BIND_VAR, &vbx);
                        bool sbx = false;
                        uint16_t sslot = 0;
                        if (has_sup)
                            sslot = cc_push_var(cc, sup, BIND_VAR, &sbx);
                        cc_emit(cc, INST(OP_GET_LOCAL, 0, keys_slot));
                        cc_emit(cc, INST(OP_CONST, 0, cc_add_const(cc, kw)));
                        cc_emit(cc, INST(OP_CONST, 0, cc_add_const(cc, VAL(lisp.sym_unbound))));
                        cc_emit(cc, INST(OP_GETF, 3, 0));
                        cc_emit(cc, INST(OP_DUP, 0, 0));
                        cc_emit(cc, INST(OP_CONST, 0, cc_add_const(cc, VAL(lisp.sym_unbound))));
                        cc_emit(cc, INST(OP_EQ, 0, 0));
                        int j_found = cc_emit_jmp(cc, OP_JMP_NIL);
                        cc_emit(cc, INST(OP_POP, 0, 0));
                        if (nilp(def_form))
                            cc_emit(cc, INST(OP_NIL, 0, 0));
                        else if (!cc_emit_expr(cc, def_form, false, false))
                            return false;
                        cc_emit(cc, INST(OP_SET_LOCAL_POP, 0, vslot));
                        if (has_sup) {
                            cc_emit(cc, INST(OP_NIL, 0, 0));
                            cc_emit(cc, INST(OP_SET_LOCAL_POP, 0, sslot));
                        }
                        int j_done = cc_emit_jmp(cc, OP_JMP);
                        cc_patch_jmp(cc, j_found);
                        cc_emit(cc, INST(OP_SET_LOCAL_POP, 0, vslot));
                        if (has_sup) {
                            cc_emit(cc, INST(OP_T, 0, 0));
                            cc_emit(cc, INST(OP_SET_LOCAL_POP, 0, sslot));
                        }
                        cc_patch_jmp(cc, j_done);
                        if (vbx)
                            cc_emit(cc, INST(OP_BOX_LOCAL, 0, vslot));
                        if (has_sup && sbx)
                            cc_emit(cc, INST(OP_BOX_LOCAL, 0, sslot));
                    }
                }
            }
            if (!nilp(p)) {
                if (!cc_is_simple_pattern(p) || cc_pattern_has_special(p))
                    return false;
                cc_emit(cc, INST(OP_GET_LOCAL, 0, cur));
                cc_emit_simple_bind(cc, p);
            }
        } else {
            return false;
        }
        bool ok = cc_emit_body(cc, CDR(CDR(args)), want_mv, tail);
        cc->var_count = saved_vars;
        cc->next_slot = saved_slot;
        return ok && !cc->failed;
    }
    case SF_FLET:
    case SF_LABELS: {
        if (!consp(args))
            return false;
        bool recursive = id == SF_LABELS;
        lisp_value defs = CAR(args);
        uint32_t count = 0;
        if (!proper_list_len(defs, &count) || count > 32)
            return false;
        uint32_t saved_vars = cc->var_count;
        uint16_t saved_slot = cc->next_slot;
        lisp_value fnames[32];
        lisp_value fparams[32];
        lisp_value fbodies[32];
        uint16_t slots[32];
        bool boxed[32];
        uint32_t i = 0;
        for (lisp_value d = defs; consp(d); d = CDR(d), i++) {
            lisp_value def = CAR(d);
            if (!consp(def) || !symbolp(CAR(def)) || !consp(CDR(def)))
                return false;
            fnames[i] = CAR(def);
            fparams[i] = CAR(CDR(def));
            fbodies[i] = CDR(CDR(def));
        }
        if (recursive) {
            for (uint32_t k = 0; k < count; k++) {
                slots[k] = cc_push_var(cc, fnames[k], BIND_FUNC, &boxed[k]);
                cc_emit(cc, INST(OP_NIL, 0, 0));
                cc_emit(cc, INST(OP_SET_LOCAL_POP, 0, slots[k]));
                if (boxed[k])
                    cc_emit(cc, INST(OP_BOX_LOCAL, 0, slots[k]));
            }
            for (uint32_t k = 0; k < count; k++) {
                const struct lisp_code *proto = cc_compile_fn(cc, cc->region, fnames[k], fparams[k], fbodies[k], 0);
                if (!proto)
                    return false;
                cc_emit(cc, INST(OP_MAKE_CLOSURE, 0, cc_add_proto(cc, proto)));
                if (boxed[k]) {
                    cc_emit(cc, INST(OP_SET_BOX, 0, slots[k]));
                    cc_emit(cc, INST(OP_POP, 0, 0));
                } else {
                    cc_emit(cc, INST(OP_SET_LOCAL_POP, 0, slots[k]));
                }
            }
        } else {
            const struct lisp_code *protos[32];
            for (uint32_t k = 0; k < count; k++) {
                protos[k] = cc_compile_fn(cc, cc->region, fnames[k], fparams[k], fbodies[k], 0);
                if (!protos[k])
                    return false;
            }
            for (uint32_t k = 0; k < count; k++) {
                slots[k] = cc_push_var(cc, fnames[k], BIND_FUNC, &boxed[k]);
                cc_emit(cc, INST(OP_MAKE_CLOSURE, 0, cc_add_proto(cc, protos[k])));
                cc_emit(cc, INST(OP_SET_LOCAL_POP, 0, slots[k]));
                if (boxed[k])
                    cc_emit(cc, INST(OP_BOX_LOCAL, 0, slots[k]));
            }
        }
        bool ok = cc_emit_body(cc, CDR(args), want_mv, tail);
        cc->var_count = saved_vars;
        cc->next_slot = saved_slot;
        return ok && !cc->failed;
    }
    case SF_CHECK_TYPE: {
        if (!consp(args) || !consp(CDR(args)))
            return false;
        if (!cc_emit_expr(cc, CAR(args), false, false))
            return false;
        cc_emit(cc, INST(OP_CHECK_TYPE, 0, cc_add_const(cc, CAR(CDR(args)))));
        if (want_mv)
            cc_emit(cc, INST(OP_MV1, 0, 0));
        return !cc->failed;
    }
    case SF_DECLARE:
    case SF_DECLAIM:
        cc_emit(cc, INST(OP_NIL, 0, 0));
        if (want_mv)
            cc_emit(cc, INST(OP_MV1, 0, 0));
        return !cc->failed;
    case SF_QUASIQUOTE:
        if (!consp(args))
            return false;
        if (!cc_emit_quasiquote(cc, CAR(args), 0))
            return false;
        if (want_mv)
            cc_emit(cc, INST(OP_MV1, 0, 0));
        return !cc->failed;
    default:
        return false;
    }
}

static bool cc_emit_body(struct cc_fn *cc, lisp_value body, bool want_mv, bool tail)
{
    if (nilp(body)) {
        cc_emit(cc, INST(OP_NIL, 0, 0));
        if (want_mv)
            cc_emit(cc, INST(OP_MV1, 0, 0));
        return !cc->failed;
    }
    while (consp(body)) {
        lisp_value form = CAR(body);
        bool last = nilp(CDR(body));
        if (!last && consp(form) && symbolp(CAR(form)) &&
                (SYM(CAR(form))->special_id == SF_DECLARE || SYM(CAR(form))->special_id == SF_DECLAIM)) {
            body = CDR(body);
            continue;
        }
        if (!cc_emit_expr(cc, form, last ? want_mv : false, last ? tail : false))
            return false;
        if (!last)
            cc_emit(cc, INST(OP_POP, 0, 0));
        body = CDR(body);
    }
    return !cc->failed;
}

static bool cc_emit_expr(struct cc_fn *cc, lisp_value form, bool want_mv, bool tail)
{
    if (cc->failed)
        return false;
    if (symbolp(form))
        return cc_emit_var_get(cc, form, want_mv);
    if (!consp(form)) {
        cc_emit(cc, INST(OP_CONST, 0, cc_add_const(cc, form)));
        if (want_mv)
            cc_emit(cc, INST(OP_MV1, 0, 0));
        return !cc->failed;
    }
    lisp_value head = CAR(form);
    if (consp(head) && symbolp(CAR(head)) && SYM(CAR(head))->special_id == SF_LAMBDA) {
        uint32_t argc = 0;
        if (!proper_list_len(CDR(form), &argc) || argc > 255)
            return false;
        if (!cc_emit_thunk(cc, CAR(CDR(head)), CDR(CDR(head))))
            return false;
        for (lisp_value a = CDR(form); consp(a); a = CDR(a))
            if (!cc_emit_expr(cc, CAR(a), false, false))
                return false;
        cc_emit(cc, INST(OP_FUNCALL, (uint8_t)argc, 0));
        return !cc->failed;
    }
    if (!symbolp(head))
        return false;
    struct lisp_symbol *sym = SYM(head);
    if (sym->special_id != SF_NONE)
        return cc_emit_special(cc, form, sym->special_id, want_mv, tail);
    if (sym->h.flags & SYM_MACRO)
        return false;
    uint32_t argc = 0;
    if (!proper_list_len(CDR(form), &argc) || argc > 255)
        return false;
    if (cc_try_inline_builtin(cc, sym, CDR(form), argc, want_mv))
        return true;
    bool boxed = false;
    int fslot = cc_find_local(cc, head, BIND_FUNC, &boxed);
    if (fslot >= 0) {
        cc_emit(cc, INST(boxed ? OP_GET_BOX : OP_GET_LOCAL, 0, fslot));
        for (lisp_value a = CDR(form); consp(a); a = CDR(a))
            if (!cc_emit_expr(cc, CAR(a), false, false))
                return false;
        cc_emit(cc, INST(OP_FUNCALL, (uint8_t)argc, 0));
        return !cc->failed;
    }
    int fup = cc_find_upval(cc, head, BIND_FUNC, &boxed);
    if (fup >= 0) {
        cc_emit(cc, INST(boxed ? OP_GET_UPVAL_BOX : OP_GET_UPVAL, 0, fup));
        for (lisp_value a = CDR(form); consp(a); a = CDR(a))
            if (!cc_emit_expr(cc, CAR(a), false, false))
                return false;
        cc_emit(cc, INST(OP_FUNCALL, (uint8_t)argc, 0));
        return !cc->failed;
    }
    for (lisp_value a = CDR(form); consp(a); a = CDR(a))
        if (!cc_emit_expr(cc, CAR(a), false, false))
            return false;
    cc_emit(cc, INST(tail ? OP_TAIL_CALL_SYM : OP_CALL_SYM, (uint8_t)argc, cc_add_const(cc, head)));
    return !cc->failed;
}

static bool cc_parse_params(struct cc_fn *cc, lisp_value params, struct vm_param_spec *specs,
    uint32_t *spec_count, uint16_t *min_args, bool *simple_out, bool *allow_other_out)
{
    uint32_t n = 0;
    uint16_t req = 0;
    bool simple = true;
    bool allow_other = false;
    int mode = PS_REQ;
    lisp_value p = params;
    while (consp(p)) {
        lisp_value item = CAR(p);
        p = CDR(p);
        if (item == VAL(lisp.sym_optional)) { simple = false; mode = PS_OPT; continue; }
        if (item == VAL(lisp.sym_key)) { simple = false; mode = PS_KEY; continue; }
        if (item == VAL(lisp.sym_rest) || item == VAL(lisp.sym_body)) {
            simple = false;
            if (!consp(p) || !symbolp(CAR(p)) || (SYM(CAR(p))->h.flags & SYM_SPECIAL))
                return false;
            if (n >= 64)
                return false;
            bool bx = false;
            uint16_t slot = cc_push_var(cc, CAR(p), BIND_VAR, &bx);
            specs[n++] = (struct vm_param_spec){ PS_REST, (uint8_t)slot, 0, 0, CAR(p), NIL_VAL, NULL };
            p = CDR(p);
            continue;
        }
        if (item == VAL(lisp.sym_allow_other_keys)) { simple = false; allow_other = true; continue; }
        if (item == VAL(lisp.sym_aux) || item == VAL(lisp.sym_whole) || item == VAL(lisp.sym_environment))
            return false;
        if (n >= 64)
            return false;
        if (mode == PS_REQ) {
            if (!symbolp(item) || (SYM(item)->h.flags & SYM_SPECIAL))
                return false;
            bool bx = false;
            uint16_t slot = cc_push_var(cc, item, BIND_VAR, &bx);
            specs[n++] = (struct vm_param_spec){ PS_REQ, (uint8_t)slot, 0, 0, item, NIL_VAL, NULL };
            req++;
            continue;
        }
        lisp_value var = item, def_form = NIL_VAL, sup = NIL_VAL, kw = NIL_VAL;
        if (consp(item)) {
            var = CAR(item);
            if (consp(CDR(item)))
                def_form = CAR(CDR(item));
            if (consp(CDR(item)) && consp(CDR(CDR(item))))
                sup = CAR(CDR(CDR(item)));
        }
        if (mode == PS_KEY && consp(var)) {
            if (!consp(CDR(var)) || !symbolp(CAR(var)) || !symbolp(CAR(CDR(var))))
                return false;
            kw = CAR(var);
            var = CAR(CDR(var));
        }
        if (!symbolp(var) || (SYM(var)->h.flags & SYM_SPECIAL))
            return false;
        if (mode == PS_KEY && nilp(kw))
            kw = VAL(symbol_intern(PKG_KEYWORD, SYM(var)->name, SYM(var)->name_len, false));
        const struct lisp_code *def_code = NULL;
        if (!nilp(def_form)) {
            def_code = cc_compile_fn(cc, cc->region, NIL_VAL, NIL_VAL, LIST1(def_form), 0);
            if (!def_code)
                return false;
        }
        bool bx = false;
        uint16_t slot = cc_push_var(cc, var, BIND_VAR, &bx);
        uint8_t has_sup = 0;
        uint8_t sup_slot = 0;
        if (symbolp(sup) && !nilp(sup)) {
            if (SYM(sup)->h.flags & SYM_SPECIAL)
                return false;
            bool sbx = false;
            sup_slot = (uint8_t)cc_push_var(cc, sup, BIND_VAR, &sbx);
            has_sup = 1;
        }
        specs[n++] = (struct vm_param_spec){ (uint8_t)mode, (uint8_t)slot, sup_slot, has_sup, var, kw, def_code };
    }
    if (!nilp(p))
        return false;
    *spec_count = n;
    *min_args = req;
    *simple_out = simple;
    *allow_other_out = allow_other;
    return true;
}

struct cc_scratch {
    struct cc_scratch *prev;
    int ctl_top;
    uint32_t ins_buf[CC_MAX_INS];
    lisp_value const_buf[CC_MAX_CONSTS];
    const struct lisp_code *proto_buf[CC_MAX_PROTOS];
    struct cc_fn cc;
    struct vm_param_spec specs[64];
};

static struct cc_scratch *cc_active;

void lisp_compile_cleanup(int target_ctl)
{
    while (cc_active && cc_active->ctl_top > target_ctl) {
        struct cc_scratch *sc = cc_active;
        cc_active = sc->prev;
        free(sc);
    }
}

static const struct lisp_code *cc_compile_fn(struct cc_fn *parent, struct lisp_region *region,
    lisp_value name, lisp_value params, lisp_value body, uint8_t info)
{
    struct cc_scratch *sc = calloc(1, sizeof(*sc));
    if (!sc)
        return NULL;
    sc->prev = cc_active;
    sc->ctl_top = lisp.ctl_top;
    cc_active = sc;
    struct cc_fn *cc = &sc->cc;
    cc->parent = parent;
    cc->region = region;
    cc->name = name;
    cc->ins = sc->ins_buf;
    cc->consts = sc->const_buf;
    cc->protos = sc->proto_buf;

    cc_scan_form(cc, params, 0);
    cc_scan_form(cc, body, 0);

    uint32_t spec_count = 0;
    uint16_t min_args = 0;
    bool simple = true;
    bool allow_other = false;
    if (!cc_parse_params(cc, params, sc->specs, &spec_count, &min_args, &simple, &allow_other)) {
        cc_active = sc->prev;
        free(sc);
        return NULL;
    }

    uint16_t param_slots = cc->next_slot;
    uint8_t box_slots[64];
    uint8_t box_count = 0;
    for (uint32_t i = 0; i < cc->var_count; i++) {
        if (cc->vars[i].boxed) {
            if (box_count < 64)
                box_slots[box_count++] = (uint8_t)cc->vars[i].slot;
            cc_emit(cc, INST(OP_BOX_LOCAL, 0, cc->vars[i].slot));
        }
    }

    lisp_value b = body;
    if (consp(b) && stringp(CAR(b)) && consp(CDR(b)))
        b = CDR(b);
    bool want_mv = true;
    bool tail_ok = !(info & FNI_ARMED);
    if (!cc_emit_body(cc, b, want_mv, tail_ok) || cc->failed) {
        cc_active = sc->prev;
        free(sc);
        return NULL;
    }
    cc_emit(cc, INST(OP_RET, 0, 0));
    if (cc->failed) {
        cc_active = sc->prev;
        free(sc);
        return NULL;
    }

    struct lisp_code *out = heap_alloc_in(region, sizeof(*out));
    memset(out, 0, sizeof(*out));
    out->gen = region;
    out->name = name;
    out->ins_count = cc->ins_count;
    out->ins = heap_alloc_in(region, sizeof(uint32_t) * cc->ins_count);
    memcpy((void *)out->ins, cc->ins, sizeof(uint32_t) * cc->ins_count);
    if (cc->const_count) {
        out->const_count = cc->const_count;
        out->consts = heap_alloc_in(region, sizeof(lisp_value) * cc->const_count);
        memcpy((void *)out->consts, cc->consts, sizeof(lisp_value) * cc->const_count);
    }
    if (cc->proto_count) {
        out->proto_count = cc->proto_count;
        out->protos = heap_alloc_in(region, sizeof(struct lisp_code *) * cc->proto_count);
        memcpy((void *)out->protos, cc->protos, sizeof(struct lisp_code *) * cc->proto_count);
    }
    if (cc->upval_count) {
        out->upval_count = cc->upval_count;
        out->updesc = heap_alloc_in(region, sizeof(struct vm_updesc) * cc->upval_count);
        memcpy((void *)out->updesc, cc->updesc, sizeof(struct vm_updesc) * cc->upval_count);
    }
    if (!simple && spec_count) {
        out->pspec_count = spec_count;
        out->pspecs = heap_alloc_in(region, sizeof(struct vm_param_spec) * spec_count);
        memcpy((void *)out->pspecs, sc->specs, sizeof(struct vm_param_spec) * spec_count);
    }
    out->min_args = min_args;
    out->param_count = param_slots;
    out->nlocals = cc->max_slot;
    out->info = info;
    out->simple = (uint8_t)simple;
    out->allow_other_keys = (uint8_t)allow_other;
    out->box_mask_count = box_count;
    if (box_count) {
        out->box_slots = heap_alloc_in(region, box_count);
        memcpy((void *)out->box_slots, box_slots, box_count);
    }
    cc_active = sc->prev;
    free(sc);
    return out;
}

void lisp_compile_closure(struct lisp_function *fn)
{
    if (fn->env != NULL) {
        fn->info |= FNI_COMPILED;
        return;
    }
    struct lisp_region *region = fn->gen;
    if (!region || region->kind != REG_GENERATION || !region->in_use)
        return;
    const struct lisp_code *code = cc_compile_fn(NULL, region, fn->self_name, fn->params, fn->body, fn->info);
    fn->code = code;
    fn->info |= FNI_COMPILED;
}

static lisp_value vm_run(uint32_t stop_fp);

static void vm_bind_complex(const struct lisp_code *code, struct lisp_function *fn,
    int argc, lisp_value *argv, uint32_t base_idx)
{
    int arg_i = 0;
    int key_start = -1;
    lisp_value *base = &vm_stack[base_idx];
    for (uint32_t i = 0; i < code->nlocals; i++)
        base[i] = NIL_VAL;
    for (uint32_t i = 0; i < code->pspec_count; i++) {
        const struct vm_param_spec *ps = &code->pspecs[i];
        if (ps->kind == PS_REQ) {
            if (arg_i >= argc)
                lisp_error("missing argument %s", SYM(ps->name)->name);
            base[ps->slot] = argv[arg_i++];
        } else if (ps->kind == PS_OPT) {
            bool sup = arg_i < argc;
            if (sup) {
                base[ps->slot] = argv[arg_i++];
            } else if (ps->default_code) {
                struct lisp_function tmp;
                memset(&tmp, 0, sizeof(tmp));
                tmp.h.type = LT_FUNCTION;
                tmp.kind = FN_CLOSURE;
                tmp.code = ps->default_code;
                tmp.gen = ps->default_code->gen;
                lisp_value up[CC_MAX_UPVALS];
                if (ps->default_code->upval_count) {
                    for (uint32_t u = 0; u < ps->default_code->upval_count; u++) {
                        const struct vm_updesc *d = &ps->default_code->updesc[u];
                        up[u] = d->is_local ? base[d->index] : fn->upvals[d->index];
                    }
                    tmp.upvals = up;
                    tmp.upval_count = ps->default_code->upval_count;
                }
                base[ps->slot] = lisp_vm_apply(&tmp, 0, NULL);
            } else {
                base[ps->slot] = NIL_VAL;
            }
            if (ps->has_supplied)
                base[ps->supplied_slot] = sup ? T_VAL : NIL_VAL;
        } else if (ps->kind == PS_REST) {
            if (key_start < 0)
                key_start = arg_i;
            lisp_value lst = NIL_VAL;
            for (int k = argc - 1; k >= arg_i; k--)
                lst = cons(argv[k], lst);
            base[ps->slot] = lst;
        } else if (ps->kind == PS_KEY) {
            if (key_start < 0)
                key_start = arg_i;
            bool sup = false;
            lisp_value val = NIL_VAL;
            for (int k = key_start; k + 1 < argc; k += 2) {
                if (argv[k] == ps->keyword) {
                    val = argv[k + 1];
                    sup = true;
                    break;
                }
            }
            if (!sup && ps->default_code) {
                struct lisp_function tmp;
                memset(&tmp, 0, sizeof(tmp));
                tmp.h.type = LT_FUNCTION;
                tmp.kind = FN_CLOSURE;
                tmp.code = ps->default_code;
                tmp.gen = ps->default_code->gen;
                lisp_value up[CC_MAX_UPVALS];
                if (ps->default_code->upval_count) {
                    for (uint32_t u = 0; u < ps->default_code->upval_count; u++) {
                        const struct vm_updesc *d = &ps->default_code->updesc[u];
                        up[u] = d->is_local ? base[d->index] : fn->upvals[d->index];
                    }
                    tmp.upvals = up;
                    tmp.upval_count = ps->default_code->upval_count;
                }
                val = lisp_vm_apply(&tmp, 0, NULL);
            }
            base[ps->slot] = val;
            if (ps->has_supplied)
                base[ps->supplied_slot] = sup ? T_VAL : NIL_VAL;
        }
    }
}

lisp_value lisp_vm_apply(struct lisp_function *fn, int argc, lisp_value *argv)
{
    const struct lisp_code *code = fn->code;
    if (vm_fp >= VM_FRAMES_SIZE - 1 || vm_sp + code->nlocals + 64 >= VM_STACK_SIZE)
        lisp_error("call depth exceeded");
    uint32_t entry_fp = vm_fp;
    uint32_t entry_sp = vm_sp;
    uint32_t base = vm_sp;
    if (code->simple) {
        if (argc < (int)code->min_args)
            lisp_error("missing argument in %s", symbolp(code->name) ? SYM(code->name)->name : "lambda");
        for (int i = 0; i < (int)code->param_count; i++)
            vm_stack[base + i] = argv[i];
        vm_sp = base + code->nlocals;
    } else {
        vm_sp = base + code->nlocals;
        vm_bind_complex(code, fn, argc, argv, base);
    }
    vm_frames[vm_fp++] = (struct vm_frame){ code, fn, code->ins, base, entry_sp, lisp.ctl_top };
    if (!(fn->info & FNI_ARMED)) {
        return vm_run(entry_fp);
    }
    int ctl = push_ctl(CTL_BLOCK);
    lisp.ctl[ctl].name = fn->self_name;
    lisp.ctl[ctl].vm_sp = entry_sp;
    lisp.ctl[ctl].vm_fp = entry_fp;
    if (setjmp(lisp.ctl[ctl].jb) != 0) {
        lisp.ctl_top = ctl;
        int n = lisp_escape_count();
        const lisp_value *vals = lisp_escape_values_ptr();
        int cnt = n > 0 ? n : 1;
        lisp.mv_count = cnt;
        for (int i = 0; i < cnt; i++)
            lisp.mv[i] = i < n ? vals[i] : NIL_VAL;
        return lisp.mv[0];
    }
    lisp_value res = vm_run(entry_fp);
    lisp.ctl_top = ctl;
    return res;
}

static inline lisp_value vm_getf(lisp_value plist, lisp_value key, lisp_value deflt);

bool lisp_code_fast_apply1(const struct lisp_code *code, lisp_value arg, lisp_value *out)
{
    if (!code->simple || code->min_args != 1 || code->box_mask_count != 0)
        return false;
    uint32_t n = code->ins_count;
    if (n == 3 && OP_OF(code->ins[1]) == OP_MV1 && OP_OF(code->ins[2]) == OP_RET) {
        uint32_t op0 = OP_OF(code->ins[0]);
        if (op0 == OP_GET_LOCAL_GETF_CONST && A_OF(code->ins[0]) == 0) {
            lisp_value key = code->consts[B_OF(code->ins[0])];
            lisp_value p = arg;
            lisp_value res = NIL_VAL;
            uint32_t it = 0;
            while (consp(p) && consp(CDR(p))) {
                if ((++it & 63u) == 0)
                    check_steps();
                if (CAR(p) == key) {
                    res = CAR(CDR(p));
                    break;
                }
                p = CDR(CDR(p));
            }
            *out = res;
            return true;
        }
        if (op0 == OP_GET_LOCAL_NTH_IMM && A_OF(code->ins[0]) == 0) {
            uint32_t k = B_OF(code->ins[0]);
            lisp_value p = arg;
            while (k > 0 && consp(p)) {
                p = CDR(p);
                k--;
            }
            *out = (k == 0 && consp(p)) ? CAR(p) : NIL_VAL;
            return true;
        }
        return false;
    }
    if (n != 4 || OP_OF(code->ins[0]) != OP_GET_LOCAL || B_OF(code->ins[0]) != 0 ||
            OP_OF(code->ins[2]) != OP_MV1 || OP_OF(code->ins[3]) != OP_RET)
        return false;
    uint32_t op = OP_OF(code->ins[1]);
    if (op == OP_GETF_CONST) {
        lisp_value key = code->consts[B_OF(code->ins[1])];
        lisp_value p = arg;
        lisp_value res = NIL_VAL;
        uint32_t it = 0;
        while (consp(p) && consp(CDR(p))) {
            if ((++it & 63u) == 0)
                check_steps();
            if (CAR(p) == key) {
                res = CAR(CDR(p));
                break;
            }
            p = CDR(CDR(p));
        }
        *out = res;
        return true;
    }
    if (op == OP_NTH_IMM) {
        uint32_t k = A_OF(code->ins[1]);
        lisp_value p = arg;
        while (k > 0 && consp(p)) {
            p = CDR(p);
            k--;
        }
        *out = (k == 0 && consp(p)) ? CAR(p) : NIL_VAL;
        return true;
    }
    if (op == OP_CAR) { *out = consp(arg) ? CAR(arg) : NIL_VAL; return true; }
    if (op == OP_CDR) { *out = consp(arg) ? CDR(arg) : NIL_VAL; return true; }
    if (op == OP_NULL) { *out = arg == NIL_VAL ? T_VAL : NIL_VAL; return true; }
    return false;
}

lisp_value lisp_vm_call1(lisp_value fn, lisp_value arg)
{
    if (TYPE(fn) == LT_FUNCTION) {
        struct lisp_function *f = PTR(fn, lisp_function);
        if (f->kind == FN_CLOSURE) {
            check_steps();
            if (!(f->info & FNI_KNOWN))
                lisp_closure_info(f);
            if (!(f->info & FNI_COMPILED) && f->gen && ++f->calls >= 2)
                lisp_compile_closure(f);
            if (f->code && (!lisp.builtin_redefined || f->upval_count > 0)) {
                lisp_value fast;
                if (lisp_code_fast_apply1(f->code, arg, &fast)) {
                    mv_return1(fast);
                    return fast;
                }
                return lisp_vm_apply(f, 1, &arg);
            }
            return apply_closure(f, 1, &arg);
        }
        if (1 >= f->min_args && (f->max_args < 0 || 1 <= f->max_args)) {
            check_steps();
            lisp.mv_count = 0;
            lisp_value res = f->fn(1, &arg);
            if (lisp.mv_count == 0 || lisp.mv[0] != res)
                mv_return1(res);
            return lisp.mv[0];
        }
    }
    return apply_function(fn, 1, &arg);
}

lisp_value lisp_vm_call2(lisp_value fn, lisp_value a, lisp_value b)
{
    lisp_value args[2] = { a, b };
    if (TYPE(fn) == LT_FUNCTION) {
        struct lisp_function *f = PTR(fn, lisp_function);
        if (f->kind == FN_CLOSURE) {
            check_steps();
            if (!(f->info & FNI_KNOWN))
                lisp_closure_info(f);
            if (!(f->info & FNI_COMPILED) && f->gen && ++f->calls >= 2)
                lisp_compile_closure(f);
            if (f->code && (!lisp.builtin_redefined || f->upval_count > 0))
                return lisp_vm_apply(f, 2, args);
            return apply_closure(f, 2, args);
        }
        if (2 >= f->min_args && (f->max_args < 0 || 2 <= f->max_args)) {
            check_steps();
            lisp.mv_count = 0;
            lisp_value res = f->fn(2, args);
            if (lisp.mv_count == 0 || lisp.mv[0] != res)
                mv_return1(res);
            return lisp.mv[0];
        }
    }
    return apply_function(fn, 2, args);
}

static inline lisp_value vm_getf(lisp_value plist, lisp_value key, lisp_value deflt)
{
    uint32_t it = 0;
    if (symbolp(key)) {
        while (consp(plist) && consp(CDR(plist))) {
            if ((++it & 63u) == 0)
                check_steps();
            if (CAR(plist) == key)
                return CAR(CDR(plist));
            plist = CDR(CDR(plist));
        }
        return deflt;
    }
    while (consp(plist) && consp(CDR(plist))) {
        if ((++it & 63u) == 0)
            check_steps();
        if (eql_value(CAR(plist), key))
            return CAR(CDR(plist));
        plist = CDR(CDR(plist));
    }
    return deflt;
}

static lisp_value vm_run(uint32_t stop_fp)
{
    static const void *const dispatch[] = {
        [OP_NOP] = &&L_OP_NOP,
        [OP_POP] = &&L_OP_POP,
        [OP_DUP] = &&L_OP_DUP,
        [OP_NIL] = &&L_OP_NIL,
        [OP_T] = &&L_OP_T,
        [OP_CONST] = &&L_OP_CONST,
        [OP_GET_LOCAL] = &&L_OP_GET_LOCAL,
        [OP_SET_LOCAL] = &&L_OP_SET_LOCAL,
        [OP_SET_LOCAL_POP] = &&L_OP_SET_LOCAL_POP,
        [OP_BOX_LOCAL] = &&L_OP_BOX_LOCAL,
        [OP_GET_BOX] = &&L_OP_GET_BOX,
        [OP_SET_BOX] = &&L_OP_SET_BOX,
        [OP_GET_UPVAL] = &&L_OP_GET_UPVAL,
        [OP_GET_UPVAL_BOX] = &&L_OP_GET_UPVAL_BOX,
        [OP_SET_UPVAL_BOX] = &&L_OP_SET_UPVAL_BOX,
        [OP_GET_SPECIAL] = &&L_OP_GET_SPECIAL,
        [OP_SET_SPECIAL] = &&L_OP_SET_SPECIAL,
        [OP_GET_FUNC] = &&L_OP_GET_FUNC,
        [OP_BIND_DYN] = &&L_OP_BIND_DYN,
        [OP_UNBIND_DYN] = &&L_OP_UNBIND_DYN,
        [OP_JMP] = &&L_OP_JMP,
        [OP_JMP_NIL] = &&L_OP_JMP_NIL,
        [OP_JMP_NOT_NIL] = &&L_OP_JMP_NOT_NIL,
        [OP_JMP_NIL_KEEP] = &&L_OP_JMP_NIL_KEEP,
        [OP_JMP_NOT_NIL_KEEP] = &&L_OP_JMP_NOT_NIL_KEEP,
        [OP_MV1] = &&L_OP_MV1,
        [OP_VALUES] = &&L_OP_VALUES,
        [OP_MV_BIND] = &&L_OP_MV_BIND,
        [OP_MV_LIST] = &&L_OP_MV_LIST,
        [OP_MV_SAVE] = &&L_OP_MV_SAVE,
        [OP_MV_RESTORE] = &&L_OP_MV_RESTORE,
        [OP_RET] = &&L_OP_RET,
        [OP_RET_ONE] = &&L_OP_RET_ONE,
        [OP_CALL_SYM] = &&L_OP_CALL_SYM,
        [OP_TAIL_CALL_SYM] = &&L_OP_TAIL_CALL_SYM,
        [OP_FUNCALL] = &&L_OP_FUNCALL,
        [OP_APPLY] = &&L_OP_APPLY,
        [OP_MAKE_CLOSURE] = &&L_OP_MAKE_CLOSURE,
        [OP_CAR] = &&L_OP_CAR,
        [OP_CDR] = &&L_OP_CDR,
        [OP_CDDR] = &&L_OP_CDDR,
        [OP_CAAR] = &&L_OP_CAAR,
        [OP_CDAR] = &&L_OP_CDAR,
        [OP_SECOND] = &&L_OP_SECOND,
        [OP_THIRD] = &&L_OP_THIRD,
        [OP_FOURTH] = &&L_OP_FOURTH,
        [OP_FIFTH] = &&L_OP_FIFTH,
        [OP_CONS] = &&L_OP_CONS,
        [OP_LIST] = &&L_OP_LIST,
        [OP_NTH] = &&L_OP_NTH,
        [OP_NTH_IMM] = &&L_OP_NTH_IMM,
        [OP_NTHCDR] = &&L_OP_NTHCDR,
        [OP_LAST] = &&L_OP_LAST,
        [OP_LENGTH] = &&L_OP_LENGTH,
        [OP_GETF] = &&L_OP_GETF,
        [OP_GETF_CONST] = &&L_OP_GETF_CONST,
        [OP_GETHASH] = &&L_OP_GETHASH,
        [OP_NULL] = &&L_OP_NULL,
        [OP_CONSP] = &&L_OP_CONSP,
        [OP_SYMBOLP] = &&L_OP_SYMBOLP,
        [OP_LISTP] = &&L_OP_LISTP,
        [OP_STRINGP] = &&L_OP_STRINGP,
        [OP_NUMBERP] = &&L_OP_NUMBERP,
        [OP_INTEGERP] = &&L_OP_INTEGERP,
        [OP_KEYWORDP] = &&L_OP_KEYWORDP,
        [OP_FUNCTIONP] = &&L_OP_FUNCTIONP,
        [OP_EQ] = &&L_OP_EQ,
        [OP_EQL] = &&L_OP_EQL,
        [OP_EQUAL] = &&L_OP_EQUAL,
        [OP_ADD2] = &&L_OP_ADD2,
        [OP_SUB2] = &&L_OP_SUB2,
        [OP_MUL2] = &&L_OP_MUL2,
        [OP_ADD1] = &&L_OP_ADD1,
        [OP_SUB1] = &&L_OP_SUB1,
        [OP_NEG] = &&L_OP_NEG,
        [OP_LT2] = &&L_OP_LT2,
        [OP_LE2] = &&L_OP_LE2,
        [OP_GT2] = &&L_OP_GT2,
        [OP_GE2] = &&L_OP_GE2,
        [OP_NUMEQ2] = &&L_OP_NUMEQ2,
        [OP_NUMNE2] = &&L_OP_NUMNE2,
        [OP_ZEROP] = &&L_OP_ZEROP,
        [OP_PLUSP] = &&L_OP_PLUSP,
        [OP_MINUSP] = &&L_OP_MINUSP,
        [OP_MIN2] = &&L_OP_MIN2,
        [OP_MAX2] = &&L_OP_MAX2,
        [OP_MEMBER2] = &&L_OP_MEMBER2,
        [OP_ASSOC2] = &&L_OP_ASSOC2,
        [OP_APPEND2] = &&L_OP_APPEND2,
        [OP_STEP] = &&L_OP_STEP,
        [OP_LOOP_STEP_IN] = &&L_OP_LOOP_STEP_IN,
        [OP_LOOP_STEP_ON] = &&L_OP_LOOP_STEP_ON,
        [OP_LOOP_STEP_IN_CDDR] = &&L_OP_LOOP_STEP_IN_CDDR,
        [OP_LOOP_STEP_ON_CDDR] = &&L_OP_LOOP_STEP_ON_CDDR,
        [OP_LOOP_COLLECT] = &&L_OP_LOOP_COLLECT,
        [OP_LOOP_APPEND] = &&L_OP_LOOP_APPEND,
        [OP_LOOP_NCONC] = &&L_OP_LOOP_NCONC,
        [OP_LOOP_ACROSS_LEN] = &&L_OP_LOOP_ACROSS_LEN,
        [OP_LOOP_ACROSS_REF] = &&L_OP_LOOP_ACROSS_REF,
        [OP_CASE_MATCH] = &&L_OP_CASE_MATCH,
        [OP_ECASE_FAIL] = &&L_OP_ECASE_FAIL,
        [OP_CHECK_TYPE] = &&L_OP_CHECK_TYPE,
        [OP_RETURN_FROM] = &&L_OP_RETURN_FROM,
        [OP_SETF_GETF] = &&L_OP_SETF_GETF,
        [OP_SETF_GETHASH] = &&L_OP_SETF_GETHASH,
        [OP_SETF_CDR] = &&L_OP_SETF_CDR,
        [OP_SETF_NTH] = &&L_OP_SETF_NTH,
        [OP_UNWIND_THUNK] = &&L_OP_UNWIND_THUNK,
        [OP_IGNORE_ERRORS_THUNK] = &&L_OP_IGNORE_ERRORS_THUNK,
        [OP_HANDLER_CASE_THUNK] = &&L_OP_HANDLER_CASE_THUNK,
        [OP_BLOCK_THUNK] = &&L_OP_BLOCK_THUNK,
        [OP_DESTRUCTURE] = &&L_OP_DESTRUCTURE,
        [OP_PUSHNEW_EQ] = &&L_OP_PUSHNEW_EQ,
        [OP_CALL_BUILTIN] = &&L_OP_CALL_BUILTIN,
        [OP_GET_LOCAL_GETF_CONST] = &&L_OP_GET_LOCAL_GETF_CONST,
        [OP_GET_LOCAL_NTH_IMM] = &&L_OP_GET_LOCAL_NTH_IMM,
        [OP_EQ_CONST] = &&L_OP_EQ_CONST,
        [OP_STRING_EQ] = &&L_OP_STRING_EQ,
        [OP_COPY_LIST] = &&L_OP_COPY_LIST,
        [OP_NREVERSE] = &&L_OP_NREVERSE,
        [OP_REVERSE] = &&L_OP_REVERSE,
        [OP_ACONS] = &&L_OP_ACONS
    };

    struct vm_frame *fr = &vm_frames[vm_fp - 1];
    const struct lisp_code *code = fr->code;
    struct lisp_function *fn = fr->fn;
    const uint32_t *pc = fr->pc;
    lisp_value *base = &vm_stack[fr->base];
    lisp_value *sp = &vm_stack[vm_sp];
    const lisp_value *consts = code->consts;
    uint32_t inst;

#define SYNC() do { vm_sp = (uint32_t)(sp - vm_stack); fr->pc = pc; } while (0)
#define RELOAD() do { fr = &vm_frames[vm_fp - 1]; code = fr->code; fn = fr->fn; pc = fr->pc; base = &vm_stack[fr->base]; consts = code->consts; sp = &vm_stack[vm_sp]; } while (0)
#define NEXT() do { inst = *pc++; goto *dispatch[OP_OF(inst)]; } while (0)

    NEXT();

L_OP_NOP:
    NEXT();

L_OP_POP:
    sp--;
    NEXT();

L_OP_DUP:
    *sp = sp[-1];
    sp++;
    NEXT();

L_OP_NIL:
    *sp++ = NIL_VAL;
    NEXT();

L_OP_T:
    *sp++ = T_VAL;
    NEXT();

L_OP_CONST:
    *sp++ = consts[B_OF(inst)];
    NEXT();

L_OP_GET_LOCAL:
    *sp++ = base[B_OF(inst)];
    NEXT();

L_OP_SET_LOCAL:
    base[B_OF(inst)] = sp[-1];
    NEXT();

L_OP_SET_LOCAL_POP:
    base[B_OF(inst)] = *--sp;
    NEXT();

L_OP_BOX_LOCAL:
    SYNC();
    base[B_OF(inst)] = cons(base[B_OF(inst)], NIL_VAL);
    NEXT();

L_OP_GET_BOX:
    *sp++ = CAR(base[B_OF(inst)]);
    NEXT();

L_OP_SET_BOX: {
    lisp_value box = base[B_OF(inst)];
    CAR(box) = heap_store(OBJ(box), sp[-1]);
    NEXT();
}

L_OP_GET_UPVAL:
    *sp++ = fn->upvals[B_OF(inst)];
    NEXT();

L_OP_GET_UPVAL_BOX:
    *sp++ = CAR(fn->upvals[B_OF(inst)]);
    NEXT();

L_OP_SET_UPVAL_BOX: {
    lisp_value box = fn->upvals[B_OF(inst)];
    CAR(box) = heap_store(OBJ(box), sp[-1]);
    NEXT();
}

L_OP_GET_SPECIAL: {
    lisp_value sym = consts[B_OF(inst)];
    lisp_value val = SYM(sym)->value;
    if (val == VAL(lisp.sym_unbound)) {
        SYNC();
        lisp_error("unbound variable %s", SYM(sym)->name);
    }
    *sp++ = val;
    NEXT();
}

L_OP_SET_SPECIAL:
    SYNC();
    set_variable(consts[B_OF(inst)], sp[-1], NULL);
    NEXT();

L_OP_GET_FUNC: {
    lisp_value sym = consts[B_OF(inst)];
    lisp_value f = SYM(sym)->function;
    if (nilp(f)) {
        SYNC();
        lisp_error("undefined function %s", SYM(sym)->name);
    }
    *sp++ = f;
    NEXT();
}

L_OP_BIND_DYN: {
    lisp_value sym = consts[B_OF(inst)];
    lisp_value val = *--sp;
    SYNC();
    int ctl = push_ctl(CTL_DYNBIND);
    lisp.ctl[ctl].sym = SYM(sym);
    lisp.ctl[ctl].old = SYM(sym)->value;
    SYM(sym)->value = val;
    NEXT();
}

L_OP_UNBIND_DYN: {
    int n = B_OF(inst);
    restore_dynamic_to(lisp.ctl_top - n);
    NEXT();
}

L_OP_JMP:
    pc += SB_OF(inst);
    NEXT();

L_OP_JMP_NIL:
    if (*--sp == NIL_VAL)
        pc += SB_OF(inst);
    NEXT();

L_OP_JMP_NOT_NIL:
    if (*--sp != NIL_VAL)
        pc += SB_OF(inst);
    NEXT();

L_OP_JMP_NIL_KEEP:
    if (sp[-1] == NIL_VAL)
        pc += SB_OF(inst);
    else
        sp--;
    NEXT();

L_OP_JMP_NOT_NIL_KEEP:
    if (sp[-1] != NIL_VAL)
        pc += SB_OF(inst);
    else
        sp--;
    NEXT();

L_OP_MV1:
    mv_return1(sp[-1]);
    NEXT();

L_OP_VALUES: {
    uint32_t n = A_OF(inst);
    sp -= n;
    lisp.mv_count = (int)n;
    for (uint32_t i = 0; i < n; i++)
        lisp.mv[i] = sp[i];
    if (n == 0)
        lisp.mv[0] = NIL_VAL;
    *sp++ = n ? lisp.mv[0] : NIL_VAL;
    NEXT();
}

L_OP_MV_BIND: {
    uint32_t n = A_OF(inst);
    uint16_t slot = B_OF(inst);
    sp--;
    int cnt = lisp.mv_count;
    for (uint32_t i = 0; i < n; i++)
        base[slot + i] = (int)i < cnt ? lisp.mv[i] : NIL_VAL;
    NEXT();
}

L_OP_MV_LIST: {
    sp--;
    SYNC();
    lisp_value out = NIL_VAL;
    for (int i = lisp.mv_count - 1; i >= 0; i--)
        out = cons(lisp.mv[i], out);
    *sp++ = out;
    NEXT();
}

L_OP_MV_SAVE: {
    if (vm_mv_sp >= VM_MV_SAVE_SIZE) {
        SYNC();
        lisp_error("mv stack overflow");
    }
    struct vm_mv_saved *sv = &vm_mv_saved[vm_mv_sp++];
    int cnt = lisp.mv_count;
    if (cnt > LISP_MAX_MV)
        cnt = LISP_MAX_MV;
    sv->count = cnt;
    for (int i = 0; i < cnt; i++)
        sv->values[i] = lisp.mv[i];
    NEXT();
}

L_OP_MV_RESTORE: {
    struct vm_mv_saved *sv = &vm_mv_saved[--vm_mv_sp];
    lisp.mv_count = sv->count;
    for (int i = 0; i < sv->count; i++)
        lisp.mv[i] = sv->values[i];
    NEXT();
}

L_OP_RET_ONE:
    mv_return1(sp[-1]);
L_OP_RET: {
    lisp_value res = *--sp;
    uint32_t ret_sp = fr->ret_sp;
    vm_fp--;
    if (vm_fp == stop_fp) {
        vm_sp = ret_sp;
        return res;
    }
    vm_stack[ret_sp] = res;
    vm_sp = ret_sp + 1;
    RELOAD();
    NEXT();
}

L_OP_TAIL_CALL_SYM: {
    uint32_t argc = A_OF(inst);
    lisp_value sym = consts[B_OF(inst)];
    lisp_value callee = SYM(sym)->function;
    if (TYPE(callee) == LT_FUNCTION) {
        struct lisp_function *cf = PTR(callee, lisp_function);
        if (cf->kind == FN_CLOSURE) {
            check_steps();
            if (!(cf->info & FNI_KNOWN)) { SYNC(); lisp_closure_info(cf); }
            if (!(cf->info & FNI_COMPILED) && cf->gen && ++cf->calls >= 2) { SYNC(); lisp_compile_closure(cf); }
            if (cf->code && cf->code->simple && !(cf->info & FNI_ARMED) &&
                    (!lisp.builtin_redefined || cf->upval_count > 0) &&
                    lisp.ctl_top == fr->ctl_mark) {
                const struct lisp_code *ccode = cf->code;
                if (argc < ccode->min_args) {
                    SYNC();
                    lisp_error("missing argument in %s", SYM(sym)->name);
                }
                lisp_value *argp = sp - argc;
                for (uint32_t i = 0; i < ccode->param_count; i++)
                    base[i] = argp[i];
                fr->code = ccode;
                fr->fn = cf;
                code = ccode;
                fn = cf;
                consts = ccode->consts;
                pc = ccode->ins;
                sp = base + ccode->nlocals;
                NEXT();
            }
        }
    }
    goto do_call_sym;
}

L_OP_CALL_SYM:
do_call_sym: {
    uint32_t argc = A_OF(inst);
    lisp_value sym = consts[B_OF(inst)];
    lisp_value callee = SYM(sym)->function;
    if (nilp(callee)) {
        SYNC();
        lisp_error("undefined function %s", SYM(sym)->name);
    }
    if (TYPE(callee) == LT_FUNCTION) {
        struct lisp_function *cf = PTR(callee, lisp_function);
        if (cf->kind == FN_CLOSURE) {
            check_steps();
            if (!(cf->info & FNI_KNOWN)) { SYNC(); lisp_closure_info(cf); }
            if (!(cf->info & FNI_COMPILED) && cf->gen && ++cf->calls >= 2) { SYNC(); lisp_compile_closure(cf); }
            if (cf->code && cf->code->simple && !(cf->info & FNI_ARMED) &&
                    (!lisp.builtin_redefined || cf->upval_count > 0)) {
                const struct lisp_code *ccode = cf->code;
                if (argc < ccode->min_args) {
                    SYNC();
                    lisp_error("missing argument in %s", SYM(sym)->name);
                }
                uint32_t new_base = (uint32_t)(sp - vm_stack) - argc;
                if (vm_fp >= VM_FRAMES_SIZE - 1 || new_base + ccode->nlocals + 64 >= VM_STACK_SIZE) {
                    SYNC();
                    lisp_error("call depth exceeded");
                }
                fr->pc = pc;
                vm_frames[vm_fp++] = (struct vm_frame){ ccode, cf, ccode->ins, new_base, new_base, lisp.ctl_top };
                fr = &vm_frames[vm_fp - 1];
                code = ccode;
                fn = cf;
                consts = ccode->consts;
                pc = ccode->ins;
                base = &vm_stack[new_base];
                sp = base + ccode->nlocals;
                NEXT();
            }
            lisp_value *argp = sp - argc;
            SYNC();
            lisp_value res = cf->code && (!lisp.builtin_redefined || cf->upval_count > 0)
                ? lisp_vm_apply(cf, (int)argc, argp)
                : apply_closure(cf, (int)argc, argp);
            sp = argp;
            *sp++ = res;
            NEXT();
        }
        if ((int)argc >= cf->min_args && (cf->max_args < 0 || (int)argc <= cf->max_args)) {
            check_steps();
            lisp_value *argp = sp - argc;
            SYNC();
            lisp.mv_count = 0;
            lisp_value res = cf->fn((int)argc, argp);
            if (lisp.mv_count == 0 || lisp.mv[0] != res)
                mv_return1(res);
            sp = argp;
            *sp++ = lisp.mv[0];
            NEXT();
        }
    }
    lisp_value *argp = sp - argc;
    SYNC();
    lisp_value res = apply_function(callee, (int)argc, argp);
    sp = argp;
    *sp++ = res;
    NEXT();
}

L_OP_FUNCALL: {
    uint32_t argc = A_OF(inst);
    lisp_value *argp = sp - argc;
    lisp_value callee = argp[-1];
    if (TYPE(callee) == LT_FUNCTION) {
        struct lisp_function *cf = PTR(callee, lisp_function);
        if (cf->kind == FN_CLOSURE) {
            check_steps();
            if (!(cf->info & FNI_KNOWN)) { SYNC(); lisp_closure_info(cf); }
            if (!(cf->info & FNI_COMPILED) && cf->gen && ++cf->calls >= 2) { SYNC(); lisp_compile_closure(cf); }
            if (cf->code && cf->code->simple && !(cf->info & FNI_ARMED) &&
                    (!lisp.builtin_redefined || cf->upval_count > 0)) {
                const struct lisp_code *ccode = cf->code;
                if (argc < ccode->min_args) {
                    SYNC();
                    lisp_error("missing argument in funcall");
                }
                uint32_t ret_sp = (uint32_t)(argp - vm_stack) - 1;
                uint32_t new_base = ret_sp + 1;
                if (vm_fp >= VM_FRAMES_SIZE - 1 || new_base + ccode->nlocals + 64 >= VM_STACK_SIZE) {
                    SYNC();
                    lisp_error("call depth exceeded");
                }
                fr->pc = pc;
                vm_frames[vm_fp++] = (struct vm_frame){ ccode, cf, ccode->ins, new_base, ret_sp, lisp.ctl_top };
                fr = &vm_frames[vm_fp - 1];
                code = ccode;
                fn = cf;
                consts = ccode->consts;
                pc = ccode->ins;
                base = &vm_stack[new_base];
                sp = base + ccode->nlocals;
                NEXT();
            }
            SYNC();
            lisp_value res = cf->code && (!lisp.builtin_redefined || cf->upval_count > 0)
                ? lisp_vm_apply(cf, (int)argc, argp)
                : apply_closure(cf, (int)argc, argp);
            sp = argp - 1;
            *sp++ = res;
            NEXT();
        }
        if ((int)argc >= cf->min_args && (cf->max_args < 0 || (int)argc <= cf->max_args)) {
            check_steps();
            SYNC();
            lisp.mv_count = 0;
            lisp_value res = cf->fn((int)argc, argp);
            if (lisp.mv_count == 0 || lisp.mv[0] != res)
                mv_return1(res);
            sp = argp - 1;
            *sp++ = lisp.mv[0];
            NEXT();
        }
    }
    SYNC();
    lisp_value res = apply_function(callee, (int)argc, argp);
    sp = argp - 1;
    *sp++ = res;
    NEXT();
}

L_OP_APPLY: {
    uint32_t argc = A_OF(inst);
    lisp_value *argp = sp - argc - 1;
    SYNC();
    lisp_value res = builtin_apply((int)(argc + 1), argp);
    sp = argp;
    *sp++ = res;
    NEXT();
}

L_OP_MAKE_CLOSURE: {
    const struct lisp_code *proto = code->protos[B_OF(inst)];
    SYNC();
    struct lisp_function *cl = heap_alloc(sizeof(*cl));
    memset(cl, 0, sizeof(*cl));
    cl->h.type = LT_FUNCTION;
    cl->h.size = sizeof(*cl);
    cl->kind = FN_CLOSURE;
    cl->info = proto->info | FNI_KNOWN | FNI_COMPILED;
    cl->self_name = proto->name;
    cl->params = NIL_VAL;
    cl->body = NIL_VAL;
    cl->env = NULL;
    cl->gen = proto->gen;
    cl->code = proto;
    if (proto->upval_count) {
        size_t raw = sizeof(struct lisp_obj) + sizeof(lisp_value) * proto->upval_count;
        struct lisp_obj *hdr = heap_alloc(raw);
        hdr->type = 0;
        hdr->flags = 0;
        hdr->aux = 0;
        hdr->size = (uint32_t)((raw + 7u) & ~(size_t)7u);
        cl->upvals = (lisp_value *)(hdr + 1);
        cl->upval_count = proto->upval_count;
        for (uint32_t i = 0; i < proto->upval_count; i++) {
            const struct vm_updesc *d = &proto->updesc[i];
            cl->upvals[i] = d->is_local ? base[d->index] : fn->upvals[d->index];
        }
    }
    *sp++ = VAL(cl);
    NEXT();
}

L_OP_CAR: {
    lisp_value v = sp[-1];
    sp[-1] = consp(v) ? CAR(v) : NIL_VAL;
    NEXT();
}

L_OP_CDR: {
    lisp_value v = sp[-1];
    sp[-1] = consp(v) ? CDR(v) : NIL_VAL;
    NEXT();
}

L_OP_CDDR: {
    lisp_value v = sp[-1];
    v = consp(v) ? CDR(v) : NIL_VAL;
    sp[-1] = consp(v) ? CDR(v) : NIL_VAL;
    NEXT();
}

L_OP_CAAR: {
    lisp_value v = sp[-1];
    v = consp(v) ? CAR(v) : NIL_VAL;
    sp[-1] = consp(v) ? CAR(v) : NIL_VAL;
    NEXT();
}

L_OP_CDAR: {
    lisp_value v = sp[-1];
    v = consp(v) ? CAR(v) : NIL_VAL;
    sp[-1] = consp(v) ? CDR(v) : NIL_VAL;
    NEXT();
}

L_OP_SECOND: {
    lisp_value v = sp[-1];
    v = consp(v) ? CDR(v) : NIL_VAL;
    sp[-1] = consp(v) ? CAR(v) : NIL_VAL;
    NEXT();
}

L_OP_THIRD: {
    lisp_value v = sp[-1];
    if (consp(v)) v = CDR(v);
    if (consp(v)) v = CDR(v);
    sp[-1] = consp(v) ? CAR(v) : NIL_VAL;
    NEXT();
}

L_OP_FOURTH: {
    lisp_value v = sp[-1];
    if (consp(v)) v = CDR(v);
    if (consp(v)) v = CDR(v);
    if (consp(v)) v = CDR(v);
    sp[-1] = consp(v) ? CAR(v) : NIL_VAL;
    NEXT();
}

L_OP_FIFTH: {
    lisp_value v = sp[-1];
    for (int i = 0; i < 4 && consp(v); i++)
        v = CDR(v);
    sp[-1] = consp(v) ? CAR(v) : NIL_VAL;
    NEXT();
}

L_OP_CONS: {
    lisp_value d = *--sp;
    lisp_value a = sp[-1];
    SYNC();
    sp[-1] = cons(a, d);
    NEXT();
}

L_OP_LIST: {
    uint32_t n = A_OF(inst);
    sp -= n;
    SYNC();
    lisp_value out = NIL_VAL;
    for (uint32_t i = n; i-- > 0;)
        out = cons(sp[i], out);
    *sp++ = out;
    NEXT();
}

L_OP_NTH: {
    lisp_value lst = *--sp;
    lisp_value idx = sp[-1];
    if (!intp(idx)) {
        SYNC();
        sp[-1] = list_nth(lst, require_int(idx, "nth index"));
        NEXT();
    }
    int64_t n = INTVAL(idx);
    uint32_t it = 0;
    while (n > 0 && consp(lst)) {
        if ((++it & 63u) == 0) { SYNC(); check_steps(); }
        lst = CDR(lst);
        n--;
    }
    sp[-1] = (n == 0 && consp(lst)) ? CAR(lst) : NIL_VAL;
    NEXT();
}

L_OP_NTH_IMM: {
    uint32_t n = A_OF(inst);
    lisp_value lst = sp[-1];
    while (n > 0 && consp(lst)) {
        lst = CDR(lst);
        n--;
    }
    sp[-1] = (n == 0 && consp(lst)) ? CAR(lst) : NIL_VAL;
    NEXT();
}

L_OP_NTHCDR: {
    lisp_value lst = *--sp;
    lisp_value idx = sp[-1];
    SYNC();
    sp[-1] = list_nthcdr(lst, require_int(idx, "nthcdr index"));
    NEXT();
}

L_OP_LAST:
    sp[-1] = list_last(sp[-1]);
    NEXT();

L_OP_LENGTH: {
    lisp_value v = sp[-1];
    if (stringp(v)) {
        SYNC();
        sp[-1] = make_int(PTR(v, lisp_string)->len);
        NEXT();
    }
    int64_t n = 0;
    while (consp(v)) {
        if ((++n & 63) == 0) { SYNC(); check_steps(); }
        v = CDR(v);
    }
    SYNC();
    sp[-1] = make_int(n);
    NEXT();
}

L_OP_GETF: {
    uint32_t argc = A_OF(inst);
    lisp_value deflt = argc == 3 ? *--sp : NIL_VAL;
    lisp_value key = *--sp;
    sp[-1] = vm_getf(sp[-1], key, deflt);
    NEXT();
}

L_OP_GETF_CONST: {
    lisp_value key = consts[B_OF(inst)];
    lisp_value p = sp[-1];
    lisp_value out = NIL_VAL;
    uint32_t it = 0;
    while (consp(p) && consp(CDR(p))) {
        if ((++it & 63u) == 0) { SYNC(); check_steps(); }
        if (CAR(p) == key) {
            out = CAR(CDR(p));
            break;
        }
        p = CDR(CDR(p));
    }
    sp[-1] = out;
    NEXT();
}

L_OP_GETHASH: {
    uint32_t argc = A_OF(inst);
    lisp_value *argp = sp - argc;
    SYNC();
    lisp_value res = builtin_gethash((int)argc, argp);
    sp = argp;
    *sp++ = res;
    NEXT();
}

L_OP_NULL:
    sp[-1] = sp[-1] == NIL_VAL ? T_VAL : NIL_VAL;
    NEXT();

L_OP_CONSP:
    sp[-1] = consp(sp[-1]) ? T_VAL : NIL_VAL;
    NEXT();

L_OP_SYMBOLP:
    sp[-1] = symbolp(sp[-1]) ? T_VAL : NIL_VAL;
    NEXT();

L_OP_LISTP:
    sp[-1] = (sp[-1] == NIL_VAL || consp(sp[-1])) ? T_VAL : NIL_VAL;
    NEXT();

L_OP_STRINGP:
    sp[-1] = stringp(sp[-1]) ? T_VAL : NIL_VAL;
    NEXT();

L_OP_NUMBERP:
    sp[-1] = numberp(sp[-1]) ? T_VAL : NIL_VAL;
    NEXT();

L_OP_INTEGERP:
    sp[-1] = intp(sp[-1]) ? T_VAL : NIL_VAL;
    NEXT();

L_OP_KEYWORDP:
    sp[-1] = (symbolp(sp[-1]) && (SYM(sp[-1])->h.flags & SYM_KEYWORD)) ? T_VAL : NIL_VAL;
    NEXT();

L_OP_FUNCTIONP:
    sp[-1] = functionp(sp[-1]) ? T_VAL : NIL_VAL;
    NEXT();

L_OP_EQ: {
    lisp_value b = *--sp;
    sp[-1] = sp[-1] == b ? T_VAL : NIL_VAL;
    NEXT();
}

L_OP_EQL: {
    lisp_value b = *--sp;
    lisp_value a = sp[-1];
    sp[-1] = (a == b || eql_value(a, b)) ? T_VAL : NIL_VAL;
    NEXT();
}

L_OP_EQUAL: {
    lisp_value b = *--sp;
    lisp_value a = sp[-1];
    if (a == b) {
        sp[-1] = T_VAL;
        NEXT();
    }
    SYNC();
    sp[-1] = equal_value(a, b) ? T_VAL : NIL_VAL;
    NEXT();
}

L_OP_ADD2: {
    lisp_value b = *--sp;
    lisp_value a = sp[-1];
    SYNC();
    if (intp(a) && intp(b))
        sp[-1] = make_int(INTVAL(a) + INTVAL(b));
    else
        sp[-1] = number_add(a, b);
    NEXT();
}

L_OP_SUB2: {
    lisp_value b = *--sp;
    lisp_value a = sp[-1];
    SYNC();
    if (intp(a) && intp(b))
        sp[-1] = make_int(INTVAL(a) - INTVAL(b));
    else
        sp[-1] = number_sub(a, b);
    NEXT();
}

L_OP_MUL2: {
    lisp_value b = *--sp;
    lisp_value a = sp[-1];
    SYNC();
    if (intp(a) && intp(b))
        sp[-1] = make_int(INTVAL(a) * INTVAL(b));
    else
        sp[-1] = number_mul(a, b);
    NEXT();
}

L_OP_ADD1: {
    lisp_value a = sp[-1];
    SYNC();
    if (intp(a))
        sp[-1] = make_int(INTVAL(a) + 1);
    else
        sp[-1] = number_add(a, make_int(1));
    NEXT();
}

L_OP_SUB1: {
    lisp_value a = sp[-1];
    SYNC();
    if (intp(a))
        sp[-1] = make_int(INTVAL(a) - 1);
    else
        sp[-1] = number_sub(a, make_int(1));
    NEXT();
}

L_OP_NEG: {
    lisp_value a = sp[-1];
    SYNC();
    if (intp(a))
        sp[-1] = make_int(-INTVAL(a));
    else
        sp[-1] = number_sub(make_int(0), a);
    NEXT();
}

L_OP_LT2: {
    lisp_value b = *--sp;
    lisp_value a = sp[-1];
    if (intp(a) && intp(b)) {
        sp[-1] = INTVAL(a) < INTVAL(b) ? T_VAL : NIL_VAL;
        NEXT();
    }
    SYNC();
    bool ok = true;
    int cmp = number_compare(a, b, &ok);
    sp[-1] = (ok && cmp < 0) ? T_VAL : NIL_VAL;
    NEXT();
}

L_OP_LE2: {
    lisp_value b = *--sp;
    lisp_value a = sp[-1];
    if (intp(a) && intp(b)) {
        sp[-1] = INTVAL(a) <= INTVAL(b) ? T_VAL : NIL_VAL;
        NEXT();
    }
    SYNC();
    bool ok = true;
    int cmp = number_compare(a, b, &ok);
    sp[-1] = (ok && cmp <= 0) ? T_VAL : NIL_VAL;
    NEXT();
}

L_OP_GT2: {
    lisp_value b = *--sp;
    lisp_value a = sp[-1];
    if (intp(a) && intp(b)) {
        sp[-1] = INTVAL(a) > INTVAL(b) ? T_VAL : NIL_VAL;
        NEXT();
    }
    SYNC();
    bool ok = true;
    int cmp = number_compare(a, b, &ok);
    sp[-1] = (ok && cmp > 0) ? T_VAL : NIL_VAL;
    NEXT();
}

L_OP_GE2: {
    lisp_value b = *--sp;
    lisp_value a = sp[-1];
    if (intp(a) && intp(b)) {
        sp[-1] = INTVAL(a) >= INTVAL(b) ? T_VAL : NIL_VAL;
        NEXT();
    }
    SYNC();
    bool ok = true;
    int cmp = number_compare(a, b, &ok);
    sp[-1] = (ok && cmp >= 0) ? T_VAL : NIL_VAL;
    NEXT();
}

L_OP_NUMEQ2: {
    lisp_value b = *--sp;
    lisp_value a = sp[-1];
    if (intp(a) && intp(b)) {
        sp[-1] = INTVAL(a) == INTVAL(b) ? T_VAL : NIL_VAL;
        NEXT();
    }
    SYNC();
    bool ok = true;
    int cmp = number_compare(a, b, &ok);
    sp[-1] = (ok && cmp == 0) ? T_VAL : NIL_VAL;
    NEXT();
}

L_OP_NUMNE2: {
    lisp_value b = *--sp;
    lisp_value a = sp[-1];
    if (intp(a) && intp(b)) {
        sp[-1] = INTVAL(a) != INTVAL(b) ? T_VAL : NIL_VAL;
        NEXT();
    }
    SYNC();
    bool ok = true;
    int cmp = number_compare(a, b, &ok);
    sp[-1] = (!ok || cmp != 0) ? T_VAL : NIL_VAL;
    NEXT();
}

L_OP_ZEROP: {
    lisp_value a = sp[-1];
    if (intp(a)) {
        sp[-1] = INTVAL(a) == 0 ? T_VAL : NIL_VAL;
        NEXT();
    }
    SYNC();
    sp[-1] = number_to_double(a) == 0.0 ? T_VAL : NIL_VAL;
    NEXT();
}

L_OP_PLUSP: {
    lisp_value a = sp[-1];
    if (intp(a)) {
        sp[-1] = INTVAL(a) > 0 ? T_VAL : NIL_VAL;
        NEXT();
    }
    SYNC();
    sp[-1] = number_to_double(a) > 0.0 ? T_VAL : NIL_VAL;
    NEXT();
}

L_OP_MINUSP: {
    lisp_value a = sp[-1];
    if (intp(a)) {
        sp[-1] = INTVAL(a) < 0 ? T_VAL : NIL_VAL;
        NEXT();
    }
    SYNC();
    sp[-1] = number_to_double(a) < 0.0 ? T_VAL : NIL_VAL;
    NEXT();
}

L_OP_MIN2: {
    lisp_value b = *--sp;
    lisp_value a = sp[-1];
    if (intp(a) && intp(b)) {
        sp[-1] = INTVAL(a) <= INTVAL(b) ? a : b;
        NEXT();
    }
    SYNC();
    bool ok = true;
    sp[-1] = (number_compare(a, b, &ok) <= 0) ? a : b;
    NEXT();
}

L_OP_MAX2: {
    lisp_value b = *--sp;
    lisp_value a = sp[-1];
    if (intp(a) && intp(b)) {
        sp[-1] = INTVAL(a) >= INTVAL(b) ? a : b;
        NEXT();
    }
    SYNC();
    bool ok = true;
    sp[-1] = (number_compare(a, b, &ok) >= 0) ? a : b;
    NEXT();
}

L_OP_MEMBER2: {
    lisp_value lst = *--sp;
    lisp_value item = sp[-1];
    lisp_value out = NIL_VAL;
    uint32_t it = 0;
    while (consp(lst)) {
        if ((++it & 63u) == 0) { SYNC(); check_steps(); }
        if (CAR(lst) == item || eql_value(item, CAR(lst))) {
            out = lst;
            break;
        }
        lst = CDR(lst);
    }
    sp[-1] = out;
    NEXT();
}

L_OP_ASSOC2: {
    lisp_value lst = *--sp;
    lisp_value target = sp[-1];
    lisp_value out = NIL_VAL;
    uint32_t it = 0;
    while (consp(lst)) {
        if ((++it & 63u) == 0) { SYNC(); check_steps(); }
        lisp_value pair = CAR(lst);
        lisp_value key = consp(pair) ? CAR(pair) : pair;
        if (key == target || eql_value(target, key)) {
            out = pair;
            break;
        }
        lst = CDR(lst);
    }
    sp[-1] = out;
    NEXT();
}

L_OP_APPEND2: {
    lisp_value b = *--sp;
    lisp_value a = sp[-1];
    SYNC();
    sp[-1] = list_append2(a, b);
    NEXT();
}

L_OP_STEP:
    SYNC();
    check_steps();
    NEXT();

L_OP_LOOP_STEP_IN: {
    uint32_t slot = A_OF(inst);
    lisp_value lst = base[slot];
    if (!consp(lst)) {
        pc += SB_OF(inst);
        NEXT();
    }
    SYNC();
    check_steps();
    *sp++ = CAR(lst);
    base[slot] = CDR(lst);
    NEXT();
}

L_OP_LOOP_STEP_ON: {
    uint32_t slot = A_OF(inst);
    lisp_value lst = base[slot];
    if (!consp(lst)) {
        pc += SB_OF(inst);
        NEXT();
    }
    SYNC();
    check_steps();
    *sp++ = lst;
    base[slot] = CDR(lst);
    NEXT();
}

L_OP_LOOP_STEP_IN_CDDR: {
    uint32_t slot = A_OF(inst);
    lisp_value lst = base[slot];
    if (!consp(lst)) {
        pc += SB_OF(inst);
        NEXT();
    }
    SYNC();
    check_steps();
    *sp++ = CAR(lst);
    lisp_value d = CDR(lst);
    base[slot] = consp(d) ? CDR(d) : NIL_VAL;
    NEXT();
}

L_OP_LOOP_STEP_ON_CDDR: {
    uint32_t slot = A_OF(inst);
    lisp_value lst = base[slot];
    if (!consp(lst)) {
        pc += SB_OF(inst);
        NEXT();
    }
    SYNC();
    check_steps();
    *sp++ = lst;
    lisp_value d = CDR(lst);
    base[slot] = consp(d) ? CDR(d) : NIL_VAL;
    NEXT();
}

L_OP_LOOP_COLLECT: {
    uint32_t hs = A_OF(inst);
    uint32_t ts = B_OF(inst);
    lisp_value val = *--sp;
    SYNC();
    lisp_value cell = cons(val, NIL_VAL);
    if (nilp(base[hs]))
        base[hs] = cell;
    else
        CDR(base[ts]) = cell;
    base[ts] = cell;
    NEXT();
}

L_OP_LOOP_APPEND: {
    uint32_t hs = A_OF(inst);
    uint32_t ts = B_OF(inst);
    lisp_value lst = *--sp;
    SYNC();
    uint32_t it = 0;
    while (consp(lst)) {
        if ((++it & 63u) == 0) check_steps();
        lisp_value cell = cons(CAR(lst), NIL_VAL);
        if (nilp(base[hs]))
            base[hs] = cell;
        else
            CDR(base[ts]) = cell;
        base[ts] = cell;
        lst = CDR(lst);
    }
    NEXT();
}

L_OP_LOOP_NCONC: {
    uint32_t hs = A_OF(inst);
    uint32_t ts = B_OF(inst);
    lisp_value lst = *--sp;
    if (consp(lst)) {
        if (nilp(base[hs]))
            base[hs] = lst;
        else
            CDR(base[ts]) = lst;
        base[ts] = list_last(lst);
    }
    NEXT();
}

L_OP_LOOP_ACROSS_LEN: {
    lisp_value v = sp[-1];
    SYNC();
    if (stringp(v))
        sp[-1] = make_int(PTR(v, lisp_string)->len);
    else
        sp[-1] = make_int(list_length(v));
    NEXT();
}

L_OP_LOOP_ACROSS_REF: {
    int64_t idx = INTVAL(*--sp);
    lisp_value seq = sp[-1];
    SYNC();
    if (stringp(seq))
        sp[-1] = make_char(string_char_at(seq, (uint32_t)idx));
    else
        sp[-1] = list_nth(seq, idx);
    NEXT();
}

L_OP_CASE_MATCH: {
    lisp_value spec = consts[B_OF(inst)];
    lisp_value key = sp[-1];
    *sp++ = case_match(key, spec) ? T_VAL : NIL_VAL;
    NEXT();
}

L_OP_ECASE_FAIL: {
    lisp_value key = *--sp;
    lisp_value form = consts[B_OF(inst)];
    SYNC();
    char shown[160];
    char whole[400];
    lisp_print(key, true, shown, sizeof(shown));
    lisp_print(form, true, whole, sizeof(whole));
    lisp_error("ECASE %s has no clause for %s", whole, shown);
}

L_OP_CHECK_TYPE: {
    lisp_value val = sp[-1];
    lisp_value type = consts[B_OF(inst)];
    SYNC();
    if (!lisp_type_check(val, type, NULL))
        lisp_error("The value %s is not of type %s", "value", "type");
    sp[-1] = NIL_VAL;
    NEXT();
}

L_OP_RETURN_FROM: {
    lisp_value bname = consts[B_OF(inst)];
    sp--;
    SYNC();
    int cnt = lisp.mv_count > 0 ? lisp.mv_count : 1;
    if (cnt > LISP_MAX_MV)
        cnt = LISP_MAX_MV;
    lisp_value buf[LISP_MAX_MV];
    for (int i = 0; i < cnt; i++)
        buf[i] = lisp.mv[i];
    throw_return(bname, cnt, buf);
}

L_OP_SETF_GETF: {
    lisp_value val = *--sp;
    lisp_value key = *--sp;
    lisp_value plist = sp[-1];
    SYNC();
    lisp_value scan = plist;
    bool found = false;
    uint32_t it = 0;
    while (consp(scan) && consp(CDR(scan))) {
        if ((++it & 63u) == 0) check_steps();
        if (CAR(scan) == key || eql_value(CAR(scan), key)) {
            CAR(CDR(scan)) = heap_store(OBJ(CDR(scan)), val);
            found = true;
            break;
        }
        scan = CDR(CDR(scan));
    }
    sp[-1] = found ? plist : cons(key, cons(val, plist));
    NEXT();
}

L_OP_SETF_GETHASH: {
    lisp_value val = *--sp;
    lisp_value tbl = *--sp;
    lisp_value key = sp[-1];
    SYNC();
    if (TYPE(tbl) != LT_HASHTABLE)
        lisp_error("gethash place is not a hash table");
    struct lisp_hashtable *table = PTR(tbl, lisp_hashtable);
    bool found = false;
    for (lisp_value scan = table->entries; consp(scan); scan = CDR(scan)) {
        lisp_value pair = CAR(scan);
        if (hash_table_match(tbl, CAR(pair), key)) {
            CDR(pair) = heap_store(OBJ(pair), val);
            found = true;
            break;
        }
    }
    if (!found) {
        lisp_value entry = cons(heap_store(table, key), heap_store(table, val));
        table->entries = heap_store(table, cons(entry, table->entries));
        table->count++;
    }
    sp[-1] = val;
    NEXT();
}

L_OP_SETF_CDR: {
    lisp_value val = *--sp;
    lisp_value cell = sp[-1];
    SYNC();
    if (!consp(cell))
        lisp_error("cdr place is not a cons");
    CDR(cell) = heap_store(OBJ(cell), val);
    sp[-1] = val;
    NEXT();
}

L_OP_SETF_NTH: {
    lisp_value val = *--sp;
    lisp_value lst = *--sp;
    lisp_value idx = sp[-1];
    SYNC();
    lisp_value cell = list_nthcdr(lst, require_int(idx, "nth index"));
    if (!consp(cell))
        lisp_error("nth place out of range");
    CAR(cell) = heap_store(OBJ(cell), val);
    sp[-1] = val;
    NEXT();
}

L_OP_UNWIND_THUNK: {
    lisp_value cleanup = *--sp;
    lisp_value body = sp[-1];
    sp--;
    SYNC();
    *sp++ = eval_unwind_thunk(body, cleanup);
    NEXT();
}

L_OP_IGNORE_ERRORS_THUNK: {
    lisp_value body = *--sp;
    SYNC();
    *sp++ = eval_ignore_errors_thunk(body);
    NEXT();
}

L_OP_HANDLER_CASE_THUNK: {
    lisp_value handler = *--sp;
    lisp_value body = *--sp;
    SYNC();
    *sp++ = eval_handler_case_thunk(body, handler);
    NEXT();
}

L_OP_BLOCK_THUNK: {
    lisp_value bname = consts[B_OF(inst)];
    lisp_value body = *--sp;
    SYNC();
    *sp++ = eval_block_thunk(bname, body);
    NEXT();
}

L_OP_DESTRUCTURE:
    NEXT();

L_OP_PUSHNEW_EQ: {
    lisp_value lst = *--sp;
    lisp_value item = sp[-1];
    bool hit = false;
    uint32_t it = 0;
    for (lisp_value p = lst; consp(p); p = CDR(p)) {
        if ((++it & 63u) == 0) { SYNC(); check_steps(); }
        if (CAR(p) == item || eql_value(item, CAR(p))) {
            hit = true;
            break;
        }
    }
    if (hit) {
        sp[-1] = lst;
    } else {
        SYNC();
        sp[-1] = cons(item, lst);
    }
    NEXT();
}

L_OP_CALL_BUILTIN: {
    uint32_t argc = A_OF(inst);
    struct lisp_function *bf = PTR(consts[B_OF(inst)], lisp_function);
    check_steps();
    lisp_value *argp = sp - argc;
    SYNC();
    lisp.mv_count = 0;
    lisp_value res = bf->fn((int)argc, argp);
    if (lisp.mv_count == 0 || lisp.mv[0] != res)
        mv_return1(res);
    sp = argp;
    *sp++ = lisp.mv[0];
    NEXT();
}

L_OP_GET_LOCAL_GETF_CONST: {
    lisp_value p = base[A_OF(inst)];
    lisp_value key = consts[B_OF(inst)];
    lisp_value out = NIL_VAL;
    uint32_t it = 0;
    while (consp(p) && consp(CDR(p))) {
        if ((++it & 63u) == 0) { SYNC(); check_steps(); }
        if (CAR(p) == key) {
            out = CAR(CDR(p));
            break;
        }
        p = CDR(CDR(p));
    }
    *sp++ = out;
    NEXT();
}

L_OP_GET_LOCAL_NTH_IMM: {
    lisp_value lst = base[A_OF(inst)];
    uint32_t n = B_OF(inst);
    while (n > 0 && consp(lst)) {
        lst = CDR(lst);
        n--;
    }
    *sp++ = (n == 0 && consp(lst)) ? CAR(lst) : NIL_VAL;
    NEXT();
}

L_OP_EQ_CONST:
    sp[-1] = sp[-1] == consts[B_OF(inst)] ? T_VAL : NIL_VAL;
    NEXT();

L_OP_STRING_EQ: {
    lisp_value b = *--sp;
    lisp_value a = sp[-1];
    if (a == b && stringp(a)) {
        sp[-1] = T_VAL;
        NEXT();
    }
    if (stringp(a) && stringp(b)) {
        struct lisp_string *sa = PTR(a, lisp_string);
        struct lisp_string *sb = PTR(b, lisp_string);
        sp[-1] = (sa->byte_len == sb->byte_len && memcmp(sa->data, sb->data, sa->byte_len) == 0) ? T_VAL : NIL_VAL;
        NEXT();
    }
    lisp_value args[2] = { a, b };
    SYNC();
    sp[-1] = builtin_string_eq(2, args);
    NEXT();
}

L_OP_COPY_LIST:
    SYNC();
    sp[-1] = list_copy_shallow(sp[-1]);
    NEXT();

L_OP_NREVERSE: {
    lisp_value lst = sp[-1];
    if (stringp(lst)) {
        SYNC();
        sp[-1] = builtin_nreverse(1, &lst);
        NEXT();
    }
    lisp_value prev = NIL_VAL;
    uint32_t it = 0;
    while (consp(lst)) {
        if ((++it & 63u) == 0) { SYNC(); check_steps(); }
        lisp_value next = CDR(lst);
        CDR(lst) = prev;
        prev = lst;
        lst = next;
    }
    sp[-1] = prev;
    NEXT();
}

L_OP_REVERSE: {
    lisp_value lst = sp[-1];
    SYNC();
    sp[-1] = stringp(lst) ? builtin_reverse(1, &lst) : list_reverse_copy(lst);
    NEXT();
}

L_OP_ACONS: {
    lisp_value alist = *--sp;
    lisp_value val = *--sp;
    lisp_value key = sp[-1];
    SYNC();
    sp[-1] = cons(cons(key, val), alist);
    NEXT();
}

#undef SYNC
#undef RELOAD
#undef NEXT
}

lisp_value lisp_eval_cached(lisp_value form, struct lisp_frame *env)
{
    return eval(form, env);
}

void lisp_forget_forms(struct lisp_region *region)
{
    (void)region;
}
