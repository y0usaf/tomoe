#include <string.h>
#include <stdio.h>

#include "lisp-internal.h"

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
    case ACC_SUM:
    case ACC_COUNT:
        return nilp(acc->value) ? make_int(0) : acc->value;
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

bool loop_parse(lisp_value clauses, struct loop_plan *plan, bool error_on_fail)
{
    memset(plan, 0, sizeof(*plan));
    plan->while_expr = NIL_VAL;
    plan->repeat_expr = NIL_VAL;
    lisp_value tokens[512];
    int n = 0;
    lisp_value p = clauses;
    while (consp(p) && n < 512) {
        tokens[n++] = CAR(p);
        p = CDR(p);
    }
    if (consp(p)) {
        if (error_on_fail)
            lisp_error("LOOP: too many clauses");
        return false;
    }
    int i = 0;
    while (i < n) {
        lisp_value token = tokens[i++];
        if (!symbolp(token)) {
            if (!consp(token)) {
                if (!error_on_fail)
                    return false;
                char shown[160];
                lisp_print(token, true, shown, sizeof(shown));
                lisp_error("LOOP: unsupported clause %s", shown);
            }
            if (plan->nb >= 64) {
                if (error_on_fail)
                    lisp_error("LOOP: too many clauses");
                return false;
            }
            struct loop_act *act = &plan->acts[plan->nb++];
            memset(act, 0, sizeof(*act));
            act->guard = NIL_VAL;
            act->expr = NIL_VAL;
            act->forms = LIST1(token);
            act->kind = ACT_DO;
            continue;
        }
        const char *name = SYM(token)->name;
        if (strcmp(name, "IF") == 0) {
            if (i >= n) {
                if (error_on_fail)
                    lisp_error("LOOP: IF needs a test");
                return false;
            }
            lisp_value guard = tokens[i];
            tokens[i - 1] = VAL(symbol_intern(PKG_TOMOE, "WHEN", 4, false));
            int else_at = -1;
            for (int j = i + 1; j < n; j++)
                if (symbolp(tokens[j]) && strcmp(SYM(tokens[j])->name, "ELSE") == 0) {
                    else_at = j;
                    break;
                }
            if (else_at >= 0) {
                if (n + 1 > 512) {
                    if (error_on_fail)
                        lisp_error("LOOP: too many clauses");
                    return false;
                }
                for (int j = n - 1; j > else_at; j--)
                    tokens[j + 1] = tokens[j];
                n += 1;
                tokens[else_at] = VAL(symbol_intern(PKG_TOMOE, "UNLESS", 6, false));
                tokens[else_at + 1] = guard;
            }
            i--;
            continue;
        }
        if (strcmp(name, "FOR") == 0 || strcmp(name, "AS") == 0) {
            if (plan->nf >= 16) {
                if (error_on_fail)
                    lisp_error("LOOP: too many FOR clauses");
                return false;
            }
            if (i >= n) {
                if (error_on_fail)
                    lisp_error("LOOP: FOR needs a variable");
                return false;
            }
            struct loop_for *clause = &plan->fors[plan->nf];
            memset(clause, 0, sizeof(*clause));
            clause->pattern = tokens[i++];
            clause->state = NIL_VAL;
            clause->from_expr = NIL_VAL;
            clause->bound = NIL_VAL;
            clause->by = NIL_VAL;
            clause->then_expr = NIL_VAL;
            clause->expr = NIL_VAL;
            clause->step_fn = NIL_VAL;
            clause->index = 0;
            int kind = -1;
            if (i < n && name_is(tokens[i], "IN"))
                kind = LF_IN;
            else if (i < n && name_is(tokens[i], "ON"))
                kind = LF_ON;
            else if (i < n && name_is(tokens[i], "ACROSS"))
                kind = LF_ACROSS;
            else if (i < n && name_is(tokens[i], "="))
                kind = LF_EQ;
            else if (i < n && name_is(tokens[i], "FROM"))
                kind = LF_FROM;
            else if (i < n && (name_is(tokens[i], "BELOW") || name_is(tokens[i], "TO") ||
                               name_is(tokens[i], "DOWNTO")))
                kind = LF_FROM;
            else {
                if (!error_on_fail)
                    return false;
                char shown[600];
                size_t used = 0;
                shown[0] = '\0';
                for (int j = 0; j < n && used < sizeof(shown) - 1; j++)
                    used += (size_t)lisp_print(tokens[j], true, shown + used, sizeof(shown) - used);
                lisp_error("LOOP: bad FOR clause in %s", shown);
            }
            clause->kind = kind;
            if (kind == LF_IN || kind == LF_ON || kind == LF_ACROSS) {
                i++;
                if (i >= n) {
                    if (error_on_fail)
                        lisp_error("LOOP: missing list");
                    return false;
                }
                clause->expr = tokens[i++];
            } else if (kind == LF_EQ) {
                i++;
                if (i >= n) {
                    if (error_on_fail)
                        lisp_error("LOOP: missing initial value");
                    return false;
                }
                clause->expr = tokens[i++];
                if (i < n && name_is(tokens[i], "THEN")) {
                    i++;
                    if (i >= n) {
                        if (error_on_fail)
                            lisp_error("LOOP: missing THEN value");
                        return false;
                    }
                    clause->then_expr = tokens[i++];
                }
            } else {
                clause->from_expr = make_int(0);
                if (name_is(tokens[i], "FROM")) {
                    i++;
                    if (i >= n) {
                        if (error_on_fail)
                            lisp_error("LOOP: missing start");
                        return false;
                    }
                    clause->from_expr = tokens[i++];
                }
                if (i < n && name_is(tokens[i], "TO")) {
                    clause->bound_kind = LB_TO;
                    i++;
                    if (i >= n) {
                        if (error_on_fail)
                            lisp_error("LOOP: missing bound");
                        return false;
                    }
                    clause->bound = tokens[i++];
                } else if (i < n && name_is(tokens[i], "BELOW")) {
                    clause->bound_kind = LB_BELOW;
                    i++;
                    if (i >= n) {
                        if (error_on_fail)
                            lisp_error("LOOP: missing bound");
                        return false;
                    }
                    clause->bound = tokens[i++];
                } else if (i < n && name_is(tokens[i], "DOWNTO")) {
                    clause->bound_kind = LB_DOWNTO;
                    i++;
                    if (i >= n) {
                        if (error_on_fail)
                            lisp_error("LOOP: missing bound");
                        return false;
                    }
                    clause->bound = tokens[i++];
                }
            }
            if (i < n && name_is(tokens[i], "BY")) {
                i++;
                if (i >= n) {
                    if (error_on_fail)
                        lisp_error("LOOP: missing BY value");
                    return false;
                }
                clause->by = tokens[i++];
            }
            plan->nf++;
            continue;
        }
        if (strcmp(name, "WITH") == 0) {
            if (plan->nw >= 32) {
                if (error_on_fail)
                    lisp_error("LOOP: too many WITH clauses");
                return false;
            }
            if (i >= n) {
                if (error_on_fail)
                    lisp_error("LOOP: WITH needs a variable");
                return false;
            }
            plan->with_vars[plan->nw] = tokens[i++];
            plan->with_inits[plan->nw] = NIL_VAL;
            if (i < n && name_is(tokens[i], "=")) {
                i++;
                if (i >= n) {
                    if (error_on_fail)
                        lisp_error("LOOP: missing WITH value");
                    return false;
                }
                plan->with_inits[plan->nw] = tokens[i++];
            }
            plan->nw++;
            continue;
        }
        if (strcmp(name, "REPEAT") == 0) {
            if (i >= n) {
                if (error_on_fail)
                    lisp_error("LOOP: REPEAT needs a count");
                return false;
            }
            plan->repeat_expr = tokens[i++];
            continue;
        }
        if (strcmp(name, "WHILE") == 0) {
            if (i >= n) {
                if (error_on_fail)
                    lisp_error("LOOP: WHILE needs a test");
                return false;
            }
            if (plan->nf > 0)
                plan->while_after_for = true;
            plan->while_expr = tokens[i++];
            continue;
        }
        if (strcmp(name, "WHEN") == 0 || strcmp(name, "UNLESS") == 0) {
            bool negate = strcmp(name, "UNLESS") == 0;
            if (i >= n) {
                if (error_on_fail)
                    lisp_error("LOOP: WHEN needs a test");
                return false;
            }
            lisp_value guard = tokens[i++];
            if (i >= n) {
                if (!error_on_fail)
                    return false;
                char shown[600];
                size_t used = 0;
                shown[0] = '\0';
                for (int j = 0; j < n && used < sizeof(shown) - 1; j++)
                    used += (size_t)lisp_print(tokens[j], true, shown + used, sizeof(shown) - used);
                lisp_error("LOOP: WHEN needs a clause in %s", shown);
            }
            lisp_value sub = tokens[i++];
            if (!symbolp(sub)) {
                if (!error_on_fail)
                    return false;
                char shown[600];
                size_t used = 0;
                shown[0] = '\0';
                for (int j = 0; j < n && used < sizeof(shown) - 1; j++)
                    used += (size_t)lisp_print(tokens[j], true, shown + used, sizeof(shown) - used);
                lisp_error("LOOP: WHEN needs a clause, saw %s in %s", "value", shown);
            }
            const char *subname = SYM(sub)->name;
            if (plan->nb >= 64) {
                if (error_on_fail)
                    lisp_error("LOOP: too many clauses");
                return false;
            }
            struct loop_act *act = &plan->acts[plan->nb++];
            memset(act, 0, sizeof(*act));
            act->guard = guard;
            act->negate = negate;
            act->expr = NIL_VAL;
            act->forms = NIL_VAL;
            if (strcmp(subname, "COLLECT") == 0 || strcmp(subname, "APPEND") == 0 ||
                strcmp(subname, "NCONC") == 0 || strcmp(subname, "SUM") == 0 ||
                strcmp(subname, "COUNT") == 0 || strcmp(subname, "MAXIMIZE") == 0 ||
                strcmp(subname, "MINIMIZE") == 0) {
                if (i >= n) {
                    if (error_on_fail)
                        lisp_error("LOOP: missing expression");
                    return false;
                }
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
                    if (i >= n) {
                        if (error_on_fail)
                            lisp_error("LOOP: missing INTO variable");
                        return false;
                    }
                    var = tokens[i++];
                }
                int found = var ? find_acc(plan->accs, plan->na, var) : -1;
                if (found < 0) {
                    if (plan->na >= 16) {
                        if (error_on_fail)
                            lisp_error("LOOP: too many accumulators");
                        return false;
                    }
                    found = plan->na++;
                    plan->accs[found].kind = kind;
                    plan->accs[found].var = var;
                    plan->accs[found].value = NIL_VAL;
                    plan->accs[found].tail = NIL_VAL;
                    plan->accs[found].anonymous = nilp(var);
                    plan->accs[found].used = false;
                    plan->accs[found].slot = -1;
                }
                act->acc = found;
            } else if (strcmp(subname, "DO") == 0 || strcmp(subname, "RETURN") == 0 ||
                       strcmp(subname, "THEREIS") == 0) {
                if (i >= n) {
                    if (error_on_fail)
                        lisp_error("LOOP: missing form");
                    return false;
                }
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
                if (error_on_fail)
                    lisp_error("LOOP: unsupported clause %s after WHEN", subname);
                return false;
            }
            continue;
        }
        if (strcmp(name, "COLLECT") == 0 || strcmp(name, "APPEND") == 0 ||
            strcmp(name, "NCONC") == 0 || strcmp(name, "SUM") == 0 ||
            strcmp(name, "COUNT") == 0 || strcmp(name, "MAXIMIZE") == 0 ||
            strcmp(name, "MINIMIZE") == 0) {
            if (plan->nb >= 64) {
                if (error_on_fail)
                    lisp_error("LOOP: too many clauses");
                return false;
            }
            if (i >= n) {
                if (error_on_fail)
                    lisp_error("LOOP: missing expression");
                return false;
            }
            struct loop_act *act = &plan->acts[plan->nb++];
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
                if (i >= n) {
                    if (error_on_fail)
                        lisp_error("LOOP: missing INTO variable");
                    return false;
                }
                var = tokens[i++];
            }
            int found = var ? find_acc(plan->accs, plan->na, var) : -1;
            if (found < 0) {
                if (plan->na >= 16) {
                    if (error_on_fail)
                        lisp_error("LOOP: too many accumulators");
                    return false;
                }
                found = plan->na++;
                plan->accs[found].kind = kind;
                plan->accs[found].var = var;
                plan->accs[found].value = NIL_VAL;
                plan->accs[found].tail = NIL_VAL;
                plan->accs[found].anonymous = nilp(var);
                plan->accs[found].used = false;
                plan->accs[found].slot = -1;
            }
            act->acc = found;
            continue;
        }
        if (strcmp(name, "DO") == 0) {
            if (i >= n) {
                if (error_on_fail)
                    lisp_error("LOOP: DO needs a form");
                return false;
            }
            if (plan->nb >= 64) {
                if (error_on_fail)
                    lisp_error("LOOP: too many clauses");
                return false;
            }
            struct loop_act *act = &plan->acts[plan->nb++];
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
        if (strcmp(name, "RETURN") == 0 || strcmp(name, "THEREIS") == 0 ||
            strcmp(name, "ALWAYS") == 0) {
            if (i >= n) {
                if (error_on_fail)
                    lisp_error("LOOP: missing expression");
                return false;
            }
            if (plan->nb >= 64) {
                if (error_on_fail)
                    lisp_error("LOOP: too many clauses");
                return false;
            }
            struct loop_act *act = &plan->acts[plan->nb++];
            memset(act, 0, sizeof(*act));
            act->guard = NIL_VAL;
            act->expr = NIL_VAL;
            act->forms = NIL_VAL;
            act->kind = strcmp(name, "RETURN") == 0 ? ACT_RETURN :
                        strcmp(name, "ALWAYS") == 0 ? ACT_ALWAYS : ACT_THEREIS;
            act->expr = tokens[i++];
            continue;
        }
        if (strcmp(name, "FINALLY") == 0) {
            while (i < n) {
                lisp_value form = tokens[i++];
                if (name_is(form, "END"))
                    continue;
                if (plan->nfin < 32)
                    plan->finally_forms[plan->nfin++] = form;
            }
            break;
        }
        if (strcmp(name, "END") == 0)
            continue;
        if (!error_on_fail)
            return false;
        char shown[600];
        size_t used = 0;
        shown[0] = '\0';
        for (int j = i - 1; j < n && used < sizeof(shown) - 1; j++)
            used += (size_t)lisp_print(tokens[j], true, shown + used, sizeof(shown) - used);
        lisp_error("LOOP: unsupported clause %s in %s", name, shown);
    }
    for (int k = 0; k < plan->nb; k++)
        if (plan->acts[k].kind == ACT_ALWAYS)
            plan->always_used = true;
    return true;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wclobbered"
lisp_value loop_eval(lisp_value clauses, struct lisp_frame *env, const char *file)
{
    (void)file;
    struct loop_plan plan;
    loop_parse(clauses, &plan, true);
    struct loop_for *fors = plan.fors;
    struct loop_acc *accs = plan.accs;
    struct loop_act *acts = plan.acts;
    int nf = plan.nf, na = plan.na, nb = plan.nb, nw = plan.nw, nfin = plan.nfin;
    lisp_value while_expr = plan.while_expr;
    lisp_value repeat_expr = plan.repeat_expr;
    bool while_after_for = plan.while_after_for;
    bool always_used = plan.always_used;

    uint32_t frame_slots = (uint32_t)(nw + na);
    struct lisp_frame *frame = frame_new(env, frame_slots + 1);
    uint32_t slot = 0;
    for (int k = 0; k < nw; k++) {
        lisp_value value = nilp(plan.with_inits[k]) ? NIL_VAL : eval(plan.with_inits[k], frame);
        frame_bind(frame, slot++, plan.with_vars[k], value, BIND_VAR);
    }
    for (int k = 0; k < na; k++) {
        accs[k].slot = (int)slot;
        frame_bind(frame, slot++, accs[k].var, NIL_VAL, BIND_VAR);
    }

    for (int k = 0; k < nf; k++) {
        struct loop_for *clause = &fors[k];
        clause->state = NIL_VAL;
        if (clause->kind == LF_IN || clause->kind == LF_ON || clause->kind == LF_ACROSS) {
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
            if (clause->kind == LF_ACROSS) {
                if (stringp(clause->state)) {
                    if (clause->index >= (int64_t)PTR(clause->state, lisp_string)->len)
                        done = true;
                } else if (!consp(clause->state)) {
                    done = true;
                }
                if (done)
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
        if (!nilp(while_expr) && !while_after_for) {
            lisp_value test = lisp_eval_cached(while_expr, iter);
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
            else if (clause->kind == LF_ACROSS) {
                if (stringp(clause->state))
                    value = make_char(string_char_at(clause->state, (uint32_t)clause->index));
                else
                    value = consp(clause->state) ? CAR(clause->state) : NIL_VAL;
            }
            uint32_t pslots = pattern_slot_count(clause->pattern);
            if (clause->kind == LF_EQ) {
                struct lisp_frame *then_env = iter;
                if (!first_iteration && !nilp(clause->then_expr)) {
                    struct lisp_frame *previous = frame_new(iter, pslots + 1);
                    destructure_bind(previous, clause->pattern, clause->state);
                    then_env = previous;
                }
                if (first_iteration)
                    value = lisp_eval_cached(clause->expr, iter);
                else if (!nilp(clause->then_expr))
                    value = lisp_eval_cached(clause->then_expr, then_env);
                else
                    value = lisp_eval_cached(clause->expr, then_env);
                clause->state = value;
            }
            struct lisp_frame *next = frame_new(iter, pslots + 1);
            destructure_bind(next, clause->pattern, value);
            iter = next;
        }
        last_env = iter;
        if (!nilp(while_expr) && while_after_for) {
            lisp_value test = lisp_eval_cached(while_expr, iter);
            if (nilp(test))
                break;
        }
        for (int k = 0; k < nb; k++) {
            struct loop_act *act = &acts[k];
            if (!nilp(act->guard)) {
                lisp_value test = lisp_eval_cached(act->guard, iter);
                if (act->negate ? truthy(test) : nilp(test))
                    continue;
            }
            switch (act->kind) {
            case ACT_DO: {
                for (lisp_value form = act->forms; consp(form); form = CDR(form))
                    lisp_eval_cached(CAR(form), iter);
                break;
            }
            case ACT_RETURN:
                returned = lisp_eval_cached(act->expr, iter);
                returning = true;
                break;
            case ACT_THEREIS: {
                lisp_value value = lisp_eval_cached(act->expr, iter);
                if (truthy(value)) {
                    returned = value;
                    returning = true;
                }
                break;
            }
            case ACT_ALWAYS: {
                lisp_value value = lisp_eval_cached(act->expr, iter);
                if (nilp(value)) {
                    returned = NIL_VAL;
                    returning = true;
                }
                break;
            }
            default: {
                struct loop_acc *acc = &accs[act->acc];
                lisp_value value = lisp_eval_cached(act->expr, iter);
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
                        CDR(acc->tail) = heap_store(OBJ(acc->tail), value);
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
            if (clause->kind == LF_IN || clause->kind == LF_ON || clause->kind == LF_ACROSS) {
                if (!nilp(clause->step_fn)) {
                    clause->state = apply_function(clause->step_fn, 1, &clause->state);
                } else if (clause->kind == LF_ACROSS && stringp(clause->state)) {
                    clause->index++;
                } else {
                    clause->state = consp(clause->state) ? CDR(clause->state) : NIL_VAL;
                }
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
    } else if (always_used) {
        result = T_VAL;
        for (int k = 0; k < nfin; k++)
            eval(plan.finally_forms[k], last_env);
    } else {
        if (anonymous_count == 1)
            result = acc_value(&accs[anonymous]);
        for (int k = 0; k < nfin; k++)
            eval(plan.finally_forms[k], last_env);
    }
    lisp.ctl_top = ctl;
    mv_return1(result);
    return result;
}
#pragma GCC diagnostic pop
