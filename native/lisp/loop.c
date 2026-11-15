#include <string.h>
#include <stdio.h>

#include "lisp-internal.h"

enum loop_for_kind { LF_IN, LF_ON, LF_FROM, LF_EQ };

enum loop_bound_kind { LB_NONE, LB_TO, LB_BELOW, LB_DOWNTO };

struct loop_for {
    int kind;
    lisp_value pattern;
    lisp_value expr;
    lisp_value then_expr;
    lisp_value by;
    lisp_value from_expr;
    lisp_value bound;
    int bound_kind;
    lisp_value state;
    lisp_value step_fn;
};

enum loop_acc_kind { ACC_COLLECT, ACC_APPEND, ACC_NCONC, ACC_SUM, ACC_COUNT, ACC_MAX, ACC_MIN };

struct loop_acc {
    int kind;
    lisp_value var;
    lisp_value value;
    lisp_value tail;
    bool anonymous;
    bool used;
    int slot;
};

enum loop_act_kind {
    ACT_COLLECT,
    ACT_APPEND,
    ACT_NCONC,
    ACT_SUM,
    ACT_COUNT,
    ACT_MAX,
    ACT_MIN,
    ACT_DO,
    ACT_RETURN,
    ACT_THEREIS
};

struct loop_act {
    int kind;
    lisp_value guard;
    bool negate;
    lisp_value expr;
    lisp_value forms;
    int acc;
};

static bool name_is(lisp_value v, const char *name)
{
    return symbolp(v) && strcmp(SYM(v)->name, name) == 0;
}

static bool clause_keyword(lisp_value v)
{
    static const char *names[] = {
        "FOR", "AS", "WITH", "REPEAT", "WHILE", "WHEN", "UNLESS", "COLLECT", "APPEND",
        "NCONC", "SUM", "COUNT", "MAXIMIZE", "MINIMIZE", "DO", "RETURN", "THEREIS",
        "FINALLY", "END", "IN", "ON", "FROM", "TO", "BELOW", "DOWNTO", "BY", "THEN", "INTO"
    };
    if (!symbolp(v))
        return false;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (strcmp(SYM(v)->name, names[i]) == 0)
            return true;
    return false;
}

static int find_acc(struct loop_acc *accs, int na, lisp_value var)
{
    for (int i = 0; i < na; i++)
        if (accs[i].var == var)
            return i;
    return -1;
}

static lisp_value acc_value(struct loop_acc *acc)
{
    switch (acc->kind) {
    case ACC_COLLECT:
        return list_reverse_copy(acc->value);
    default:
        return acc->value;
    }
}

static lisp_value loop_num_op(int kind, lisp_value a, lisp_value b)
{
    switch (kind) {
    case ACC_SUM:
        return number_add(a, b);
    case ACC_COUNT:
        return number_add(a, make_int(1));
    default:
        return a;
    }
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wclobbered"
lisp_value loop_eval(lisp_value clauses, struct lisp_frame *env, const char *file)
{
    (void)file;
    lisp_value tokens[512];
    int n = 0;
    lisp_value p = clauses;
    while (consp(p) && n < 512) {
        tokens[n++] = CAR(p);
        p = CDR(p);
    }
    struct loop_for fors[16];
    struct loop_acc accs[16];
    struct loop_act acts[64];
    lisp_value with_vars[32];
    lisp_value with_inits[32];
    lisp_value finally_forms[32];
    int nf = 0, na = 0, nb = 0, nw = 0, nfin = 0;
    lisp_value while_expr = NIL_VAL;
    lisp_value repeat_expr = NIL_VAL;
    int i = 0;

    while (i < n) {
        lisp_value token = tokens[i++];
        if (!symbolp(token))
            lisp_error("LOOP: unsupported clause");
        const char *name = SYM(token)->name;
        if (strcmp(name, "FOR") == 0 || strcmp(name, "AS") == 0) {
            if (nf >= 16)
                lisp_error("LOOP: too many FOR clauses");
            if (i >= n)
                lisp_error("LOOP: FOR needs a variable");
            struct loop_for *clause = &fors[nf];
            memset(clause, 0, sizeof(*clause));
            clause->pattern = tokens[i++];
            clause->state = NIL_VAL;
            clause->from_expr = NIL_VAL;
            clause->bound = NIL_VAL;
            clause->by = NIL_VAL;
            clause->then_expr = NIL_VAL;
            clause->expr = NIL_VAL;
            clause->step_fn = NIL_VAL;
            int kind = -1;
            if (i < n && name_is(tokens[i], "IN"))
                kind = LF_IN;
            else if (i < n && name_is(tokens[i], "ON"))
                kind = LF_ON;
            else if (i < n && name_is(tokens[i], "="))
                kind = LF_EQ;
            else if (i < n && name_is(tokens[i], "FROM"))
                kind = LF_FROM;
            else if (i < n && (name_is(tokens[i], "BELOW") || name_is(tokens[i], "TO") ||
                               name_is(tokens[i], "DOWNTO")))
                kind = LF_FROM;
            else
                lisp_error("LOOP: bad FOR clause");
            clause->kind = kind;
            if (kind == LF_IN || kind == LF_ON) {
                i++;
                if (i >= n)
                    lisp_error("LOOP: missing list");
                clause->expr = tokens[i++];
            } else if (kind == LF_EQ) {
                i++;
                if (i >= n)
                    lisp_error("LOOP: missing initial value");
                clause->expr = tokens[i++];
                if (i < n && name_is(tokens[i], "THEN")) {
                    i++;
                    if (i >= n)
                        lisp_error("LOOP: missing THEN value");
                    clause->then_expr = tokens[i++];
                }
            } else {
                clause->from_expr = make_int(0);
                if (name_is(tokens[i], "FROM")) {
                    i++;
                    if (i >= n)
                        lisp_error("LOOP: missing start");
                    clause->from_expr = tokens[i++];
                }
                if (i < n && name_is(tokens[i], "TO")) {
                    clause->bound_kind = LB_TO;
                    i++;
                    if (i >= n)
                        lisp_error("LOOP: missing bound");
                    clause->bound = tokens[i++];
                } else if (i < n && name_is(tokens[i], "BELOW")) {
                    clause->bound_kind = LB_BELOW;
                    i++;
                    if (i >= n)
                        lisp_error("LOOP: missing bound");
                    clause->bound = tokens[i++];
                } else if (i < n && name_is(tokens[i], "DOWNTO")) {
                    clause->bound_kind = LB_DOWNTO;
                    i++;
                    if (i >= n)
                        lisp_error("LOOP: missing bound");
                    clause->bound = tokens[i++];
                }
            }
            if (i < n && name_is(tokens[i], "BY")) {
                i++;
                if (i >= n)
                    lisp_error("LOOP: missing BY value");
                clause->by = tokens[i++];
            }
            nf++;
            continue;
        }
        if (strcmp(name, "WITH") == 0) {
            if (nw >= 32)
                lisp_error("LOOP: too many WITH clauses");
            if (i >= n)
                lisp_error("LOOP: WITH needs a variable");
            with_vars[nw] = tokens[i++];
            with_inits[nw] = NIL_VAL;
            if (i < n && name_is(tokens[i], "=")) {
                i++;
                if (i >= n)
                    lisp_error("LOOP: missing WITH value");
                with_inits[nw] = tokens[i++];
            }
            nw++;
            continue;
        }
        if (strcmp(name, "REPEAT") == 0) {
            if (i >= n)
                lisp_error("LOOP: REPEAT needs a count");
            repeat_expr = tokens[i++];
            continue;
        }
        if (strcmp(name, "WHILE") == 0) {
            if (i >= n)
                lisp_error("LOOP: WHILE needs a test");
            while_expr = tokens[i++];
            continue;
        }
        if (strcmp(name, "WHEN") == 0 || strcmp(name, "UNLESS") == 0) {
            bool negate = strcmp(name, "UNLESS") == 0;
            if (i >= n)
                lisp_error("LOOP: WHEN needs a test");
            lisp_value guard = tokens[i++];
            if (i >= n)
                lisp_error("LOOP: WHEN needs a clause");
            lisp_value sub = tokens[i++];
            if (!symbolp(sub))
                lisp_error("LOOP: WHEN needs a clause");
            const char *subname = SYM(sub)->name;
            if (nb >= 64)
                lisp_error("LOOP: too many clauses");
            struct loop_act *act = &acts[nb++];
            memset(act, 0, sizeof(*act));
            act->guard = NIL_VAL;
            act->expr = NIL_VAL;
            act->forms = NIL_VAL;
            act->guard = guard;
            act->negate = negate;
            if (strcmp(subname, "COLLECT") == 0 || strcmp(subname, "APPEND") == 0 ||
                strcmp(subname, "NCONC") == 0 || strcmp(subname, "SUM") == 0 ||
                strcmp(subname, "COUNT") == 0 || strcmp(subname, "MAXIMIZE") == 0 ||
                strcmp(subname, "MINIMIZE") == 0) {
                if (i >= n)
                    lisp_error("LOOP: missing expression");
                act->expr = tokens[i++];
                int kind = ACC_COLLECT;
                int actkind = ACT_COLLECT;
                if (strcmp(subname, "APPEND") == 0) {
                    kind = ACC_APPEND;
                    actkind = ACT_APPEND;
                } else if (strcmp(subname, "NCONC") == 0) {
                    kind = ACC_NCONC;
                    actkind = ACT_NCONC;
                } else if (strcmp(subname, "SUM") == 0) {
                    kind = ACC_SUM;
                    actkind = ACT_SUM;
                } else if (strcmp(subname, "COUNT") == 0) {
                    kind = ACC_COUNT;
                    actkind = ACT_COUNT;
                } else if (strcmp(subname, "MAXIMIZE") == 0) {
                    kind = ACC_MAX;
                    actkind = ACT_MAX;
                } else if (strcmp(subname, "MINIMIZE") == 0) {
                    kind = ACC_MIN;
                    actkind = ACT_MIN;
                }
                act->kind = actkind;
                lisp_value var = NIL_VAL;
                if (i < n && name_is(tokens[i], "INTO")) {
                    i++;
                    if (i >= n)
                        lisp_error("LOOP: missing INTO variable");
                    var = tokens[i++];
                }
                int found = var ? find_acc(accs, na, var) : -1;
                if (found < 0) {
                    if (na >= 16)
                        lisp_error("LOOP: too many accumulators");
                    found = na++;
                    accs[found].kind = kind;
                    accs[found].var = var;
                    accs[found].value = NIL_VAL;
                    accs[found].tail = NIL_VAL;
                    accs[found].anonymous = nilp(var);
                    accs[found].used = false;
                    accs[found].slot = -1;
                }
                act->acc = found;
            } else if (strcmp(subname, "DO") == 0 || strcmp(subname, "RETURN") == 0 ||
                       strcmp(subname, "THEREIS") == 0) {
                if (i >= n)
                    lisp_error("LOOP: missing form");
                lisp_value first = tokens[i++];
                if (strcmp(subname, "DO") == 0) {
                    act->kind = ACT_DO;
                    lisp_value forms = LIST1(first);
                    lisp_value tail = forms;
                    while (i < n && consp(tokens[i]) && !clause_keyword(tokens[i])) {
                        CDR(tail) = LIST1(tokens[i++]);
                        tail = CDR(tail);
                    }
                    act->forms = forms;
                } else {
                    act->kind = strcmp(subname, "RETURN") == 0 ? ACT_RETURN : ACT_THEREIS;
                    act->expr = first;
                }
            } else {
                lisp_error("LOOP: unsupported clause after WHEN");
            }
            continue;
        }
        if (strcmp(name, "COLLECT") == 0 || strcmp(name, "APPEND") == 0 ||
            strcmp(name, "NCONC") == 0 || strcmp(name, "SUM") == 0 ||
            strcmp(name, "COUNT") == 0 || strcmp(name, "MAXIMIZE") == 0 ||
            strcmp(name, "MINIMIZE") == 0) {
            if (nb >= 64)
                lisp_error("LOOP: too many clauses");
            if (i >= n)
                lisp_error("LOOP: missing expression");
            struct loop_act *act = &acts[nb++];
            memset(act, 0, sizeof(*act));
            act->guard = NIL_VAL;
            act->expr = NIL_VAL;
            act->forms = NIL_VAL;
            int kind = ACC_COLLECT;
            int actkind = ACT_COLLECT;
            if (strcmp(name, "APPEND") == 0) {
                kind = ACC_APPEND;
                actkind = ACT_APPEND;
            } else if (strcmp(name, "NCONC") == 0) {
                kind = ACC_NCONC;
                actkind = ACT_NCONC;
            } else if (strcmp(name, "SUM") == 0) {
                kind = ACC_SUM;
                actkind = ACT_SUM;
            } else if (strcmp(name, "COUNT") == 0) {
                kind = ACC_COUNT;
                actkind = ACT_COUNT;
            } else if (strcmp(name, "MAXIMIZE") == 0) {
                kind = ACC_MAX;
                actkind = ACT_MAX;
            } else if (strcmp(name, "MINIMIZE") == 0) {
                kind = ACC_MIN;
                actkind = ACT_MIN;
            }
            act->kind = actkind;
            act->expr = tokens[i++];
            lisp_value var = NIL_VAL;
            if (i < n && name_is(tokens[i], "INTO")) {
                i++;
                if (i >= n)
                    lisp_error("LOOP: missing INTO variable");
                var = tokens[i++];
            }
            int found = var ? find_acc(accs, na, var) : -1;
            if (found < 0) {
                if (na >= 16)
                    lisp_error("LOOP: too many accumulators");
                found = na++;
                accs[found].kind = kind;
                accs[found].var = var;
                accs[found].value = NIL_VAL;
                accs[found].tail = NIL_VAL;
                accs[found].anonymous = nilp(var);
                accs[found].used = false;
                accs[found].slot = -1;
            }
            act->acc = found;
            continue;
        }
        if (strcmp(name, "DO") == 0) {
            if (i >= n)
                lisp_error("LOOP: DO needs a form");
            if (nb >= 64)
                lisp_error("LOOP: too many clauses");
            struct loop_act *act = &acts[nb++];
            memset(act, 0, sizeof(*act));
            act->guard = NIL_VAL;
            act->expr = NIL_VAL;
            act->forms = NIL_VAL;
            act->kind = ACT_DO;
            lisp_value forms = LIST1(tokens[i++]);
            lisp_value tail = forms;
            while (i < n && consp(tokens[i]) && !clause_keyword(tokens[i])) {
                CDR(tail) = LIST1(tokens[i++]);
                tail = CDR(tail);
            }
            act->forms = forms;
            continue;
        }
        if (strcmp(name, "RETURN") == 0 || strcmp(name, "THEREIS") == 0) {
            if (i >= n)
                lisp_error("LOOP: missing expression");
            if (nb >= 64)
                lisp_error("LOOP: too many clauses");
            struct loop_act *act = &acts[nb++];
            memset(act, 0, sizeof(*act));
            act->guard = NIL_VAL;
            act->expr = NIL_VAL;
            act->forms = NIL_VAL;
            act->kind = strcmp(name, "RETURN") == 0 ? ACT_RETURN : ACT_THEREIS;
            act->expr = tokens[i++];
            continue;
        }
        if (strcmp(name, "FINALLY") == 0) {
            while (i < n) {
                lisp_value form = tokens[i++];
                if (name_is(form, "END"))
                    continue;
                if (nfin < 32)
                    finally_forms[nfin++] = form;
            }
            break;
        }
        if (strcmp(name, "END") == 0)
            continue;
        lisp_error("LOOP: unsupported clause %s", name);
    }

    uint32_t frame_slots = (uint32_t)(nw + na);
    struct lisp_frame *frame = frame_new(env, frame_slots + 1);
    uint32_t slot = 0;
    for (int k = 0; k < nw; k++) {
        lisp_value value = nilp(with_inits[k]) ? NIL_VAL : eval(with_inits[k], frame);
        frame_bind(frame, slot++, with_vars[k], value, BIND_VAR);
    }
    for (int k = 0; k < na; k++) {
        accs[k].slot = (int)slot;
        frame_bind(frame, slot++, accs[k].var, NIL_VAL, BIND_VAR);
    }
    if (na > 16)
        lisp_error("LOOP: too many accumulators");

    for (int k = 0; k < nf; k++) {
        struct loop_for *clause = &fors[k];
        clause->state = NIL_VAL;
        if (clause->kind == LF_IN || clause->kind == LF_ON) {
            clause->state = eval(clause->expr, frame);
            if (!nilp(clause->by))
                clause->step_fn = eval(clause->by, frame);
        } else if (clause->kind == LF_EQ) {
            clause->state = NIL_VAL;
        } else if (clause->kind == LF_FROM) {
            clause->state = eval(clause->from_expr, frame);
            if (!nilp(clause->bound))
                clause->bound = eval(clause->bound, frame);
            clause->by = nilp(clause->by) ? make_int(1) : eval(clause->by, frame);
        }
    }
    int64_t remaining = -1;
    if (!nilp(repeat_expr))
        remaining = require_int(eval(repeat_expr, frame), "repeat count");

    int ctl = push_ctl(CTL_BLOCK);
    lisp.ctl[ctl].name = NIL_VAL;
    if (setjmp(lisp.ctl[ctl].jb) != 0) {
        lisp.ctl_top = ctl;
        int count = lisp_escape_count() > 0 ? lisp_escape_count() : 1;
        const lisp_value *values = lisp_escape_values_ptr();
        lisp.mv_count = count;
        for (int k = 0; k < count; k++)
            lisp.mv[k] = k < lisp_escape_count() ? values[k] : NIL_VAL;
        return lisp.mv[0];
    }

    lisp_value returned = NIL_VAL;
    bool returning = false;
    bool first_iteration = true;
    struct lisp_frame *last_env = frame;

    for (;;) {
        check_steps();
        if (remaining == 0)
            break;
        bool done = false;
        for (int k = 0; k < nf; k++) {
            struct loop_for *clause = &fors[k];
            if ((clause->kind == LF_IN || clause->kind == LF_ON) && !consp(clause->state)) {
                done = true;
                break;
            }
            if (clause->kind == LF_FROM && clause->bound_kind != LB_NONE) {
                bool ok = true;
                int cmp = number_compare(clause->state, clause->bound, &ok);
                if (!ok)
                    lisp_error("LOOP: bad bound");
                if (clause->bound_kind == LB_TO && cmp > 0)
                    done = true;
                else if (clause->bound_kind == LB_BELOW && cmp >= 0)
                    done = true;
                else if (clause->bound_kind == LB_DOWNTO && cmp < 0)
                    done = true;
                if (done)
                    break;
            }
        }
        if (done)
            break;
        struct lisp_frame *iter = frame;
        if (!nilp(while_expr)) {
            lisp_value test = eval(while_expr, iter);
            if (nilp(test))
                break;
        }
        for (int k = 0; k < nf; k++) {
            struct loop_for *clause = &fors[k];
            lisp_value value = clause->state;
            if (clause->kind == LF_IN)
                value = consp(clause->state) ? CAR(clause->state) : NIL_VAL;
            else if (clause->kind == LF_ON)
                value = clause->state;
            uint32_t slots = pattern_slot_count(clause->pattern);
            if (clause->kind == LF_EQ) {
                struct lisp_frame *then_env = iter;
                if (!first_iteration && !nilp(clause->then_expr)) {
                    struct lisp_frame *previous = frame_new(iter, slots + 1);
                    destructure_bind(previous, clause->pattern, clause->state);
                    then_env = previous;
                }
                if (first_iteration)
                    value = eval(clause->expr, iter);
                else if (!nilp(clause->then_expr))
                    value = eval(clause->then_expr, then_env);
                else
                    value = eval(clause->expr, then_env);
                clause->state = value;
            }
            struct lisp_frame *binding = frame_new(iter, slots + 1);
            destructure_bind(binding, clause->pattern, value);
            iter = binding;
        }
        last_env = iter;
        for (int k = 0; k < nb; k++) {
            struct loop_act *act = &acts[k];
            if (!nilp(act->guard)) {
                lisp_value test = eval(act->guard, iter);
                if (act->negate ? truthy(test) : nilp(test))
                    continue;
            }
            switch (act->kind) {
            case ACT_DO:
                eval_progn(act->forms, iter);
                break;
            case ACT_RETURN:
                returned = eval(act->expr, iter);
                returning = true;
                break;
            case ACT_THEREIS: {
                lisp_value value = eval(act->expr, iter);
                if (truthy(value)) {
                    returned = value;
                    returning = true;
                }
                break;
            }
            default: {
                struct loop_acc *acc = &accs[act->acc];
                lisp_value value = eval(act->expr, iter);
                acc->used = true;
                switch (act->kind) {
                case ACT_COLLECT:
                    acc->value = cons(value, acc->value);
                    break;
                case ACT_APPEND:
                    acc->value = list_append2(acc->value, value);
                    break;
                case ACT_NCONC:
                    if (nilp(value))
                        break;
                    if (nilp(acc->value))
                        acc->value = value;
                    else
                        CDR(acc->tail) = value;
                    acc->tail = list_last(value);
                    break;
                case ACT_SUM:
                case ACT_COUNT:
                    if (nilp(acc->value))
                        acc->value = make_int(0);
                    acc->value = loop_num_op(acc->kind == ACT_SUM ? ACC_SUM : ACC_COUNT,
                                             acc->value, value);
                    break;
                case ACT_MAX:
                case ACT_MIN: {
                    bool ok = true;
                    if (nilp(acc->value))
                        acc->value = value;
                    else {
                        int cmp = number_compare(value, acc->value, &ok);
                        if (!ok)
                            lisp_error("LOOP: bad number");
                        if ((act->kind == ACT_MAX && cmp > 0) || (act->kind == ACT_MIN && cmp < 0))
                            acc->value = value;
                    }
                    break;
                }
                default:
                    break;
                }
                if (!nilp(acc->var))
                    frame_bind(frame, (uint32_t)acc->slot, acc->var, acc_value(acc), BIND_VAR);
                break;
            }
            }
            if (returning)
                break;
        }
        if (returning)
            break;
        for (int k = 0; k < nf; k++) {
            struct loop_for *clause = &fors[k];
            if (clause->kind == LF_IN || clause->kind == LF_ON) {
                if (!nilp(clause->step_fn))
                    clause->state = apply_function(clause->step_fn, 1, &clause->state);
                else
                    clause->state = consp(clause->state) ? CDR(clause->state) : NIL_VAL;
            } else if (clause->kind == LF_FROM) {
                clause->state = clause->bound_kind == LB_DOWNTO
                    ? number_sub(clause->state, clause->by)
                    : number_add(clause->state, clause->by);
            }
        }
        if (remaining > 0)
            remaining--;
        first_iteration = false;
    }
    int anonymous = -1, anonymous_count = 0;
    for (int k = 0; k < na; k++) {
        if (accs[k].anonymous) {
            anonymous_count++;
            anonymous = k;
        }
    }
    lisp_value result = NIL_VAL;
    if (returning) {
        result = returned;
    } else {
        if (anonymous_count == 1 && accs[anonymous].used)
            result = acc_value(&accs[anonymous]);
        else if (anonymous_count == 1)
            result = nilp(accs[anonymous].value) && accs[anonymous].kind != ACC_SUM &&
                             accs[anonymous].kind != ACC_COUNT ? NIL_VAL : acc_value(&accs[anonymous]);
        for (int k = 0; k < nfin; k++)
            eval(finally_forms[k], last_env);
    }
    lisp.ctl_top = ctl;
    mv_return1(result);
    return result;
}
#pragma GCC diagnostic pop
