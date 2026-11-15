#include <string.h>

#include "lisp-internal.h"

void actx_init(struct lisp_actx *ctx, lisp_value file_funcs, lisp_value file_vars)
{
    ctx->var_top = 0;
    ctx->func_top = 0;
    ctx->file_funcs = file_funcs;
    ctx->file_vars = file_vars;
}

static bool mentions(lisp_value list, lisp_value sym)
{
    while (consp(list)) {
        if (CAR(list) == sym)
            return true;
        list = CDR(list);
    }
    return false;
}

void analyse_push_var(struct lisp_actx *ctx, lisp_value sym)
{
    if (!symbolp(sym))
        return;
    if (ctx->var_top >= (int)(sizeof(ctx->vars) / sizeof(ctx->vars[0])))
        lisp_error("scope nesting too deep");
    ctx->vars[ctx->var_top++] = sym;
}

void analyse_push_func(struct lisp_actx *ctx, lisp_value sym)
{
    if (!symbolp(sym))
        return;
    if (ctx->func_top >= (int)(sizeof(ctx->funcs) / sizeof(ctx->funcs[0])))
        lisp_error("scope nesting too deep");
    ctx->funcs[ctx->func_top++] = sym;
}

void analyse_push_pattern(struct lisp_actx *ctx, lisp_value pattern)
{
    if (nilp(pattern))
        return;
    if (symbolp(pattern)) {
        analyse_push_var(ctx, pattern);
        return;
    }
    if (!consp(pattern))
        return;
    lisp_value p = pattern;
    while (consp(p)) {
        lisp_value head = CAR(p);
        p = CDR(p);
        if (symbolp(head)) {
            if (head == VAL(lisp.sym_rest) || head == VAL(lisp.sym_body) ||
                head == VAL(lisp.sym_optional) || head == VAL(lisp.sym_key))
                continue;
            analyse_push_var(ctx, head);
            continue;
        }
        analyse_push_pattern(ctx, head);
    }
    if (!nilp(p))
        analyse_push_pattern(ctx, p);
}

static bool var_bound(struct lisp_actx *ctx, lisp_value sym)
{
    if (mentions(ctx->file_vars, sym))
        return true;
    for (int i = 0; i < ctx->var_top; i++)
        if (ctx->vars[i] == sym)
            return true;
    if (sym == VAL(lisp.sym_t) || sym == VAL(lisp.sym_nil))
        return true;
    if (SYM(sym)->h.flags & (SYM_KEYWORD | SYM_CONSTANT))
        return true;
    return SYM(sym)->value != VAL(lisp.sym_unbound);
}

static bool func_bound(struct lisp_actx *ctx, lisp_value sym)
{
    if (mentions(ctx->file_funcs, sym))
        return true;
    for (int i = 0; i < ctx->func_top; i++)
        if (ctx->funcs[i] == sym)
            return true;
    return !nilp(SYM(sym)->function);
}

static void analyse_args(struct lisp_actx *ctx, lisp_value args)
{
    while (consp(args)) {
        analyse_expr(ctx, CAR(args));
        args = CDR(args);
    }
}

static void analyse_lambda_list(struct lisp_actx *ctx, lisp_value params)
{
    lisp_value p = params;
    while (consp(p)) {
        lisp_value item = CAR(p);
        p = CDR(p);
        if (symbolp(item)) {
            if (item == VAL(lisp.sym_rest) || item == VAL(lisp.sym_body) ||
                item == VAL(lisp.sym_optional) || item == VAL(lisp.sym_key) ||
                item == VAL(lisp.sym_aux) || item == VAL(lisp.sym_allow_other_keys) ||
                item == VAL(lisp.sym_whole) || item == VAL(lisp.sym_environment))
                continue;
            analyse_push_var(ctx, item);
            continue;
        }
        if (consp(item)) {
            lisp_value head = CAR(item);
            if (consp(head)) {
                analyse_push_var(ctx, CAR(CDR(head)));
            } else {
                analyse_push_var(ctx, head);
            }
            if (consp(CDR(item)) && consp(CDR(CDR(item))))
                analyse_push_var(ctx, CAR(CDR(CDR(item))));
        }
    }
}

static void analyse_quasiquote(struct lisp_actx *ctx, lisp_value form, int depth)
{
    if (!consp(form))
        return;
    lisp_value head = CAR(form);
    if (head == VAL(lisp.sym_unquote)) {
        if (depth == 1) {
            analyse_expr(ctx, CAR(CDR(form)));
        } else {
            analyse_quasiquote(ctx, CAR(CDR(form)), depth - 1);
        }
        return;
    }
    if (head == VAL(lisp.sym_quasiquote)) {
        analyse_quasiquote(ctx, CAR(CDR(form)), depth + 1);
        return;
    }
    lisp_value p = form;
    while (consp(p)) {
        lisp_value item = CAR(p);
        p = CDR(p);
        if (consp(item) && CAR(item) == VAL(lisp.sym_unquote_splicing) && depth == 1)
            analyse_expr(ctx, CAR(CDR(item)));
        else
            analyse_quasiquote(ctx, item, depth);
    }
    if (!nilp(p))
        analyse_quasiquote(ctx, p, depth);
}

void analyse_expr(struct lisp_actx *ctx, lisp_value form)
{
    if (TYPE(form) == LT_SYMBOL) {
        if (!var_bound(ctx, form))
            lisp_error("undefined variable %s", SYM(form)->name);
        return;
    }
    if (TYPE(form) != LT_CONS)
        return;
    lisp_value head = CAR(form);
    lisp_value args = CDR(form);
    if (TYPE(head) == LT_SYMBOL) {
        bool local = false;
        for (int i = 0; i < ctx->func_top; i++)
            if (ctx->funcs[i] == head)
                local = true;
        if (local) {
            analyse_args(ctx, args);
            return;
        }
        if (SYM(head)->special_id) {
            int id = (int)SYM(head)->special_id;
            switch (id) {
            case SF_QUOTE:
                return;
            case SF_FUNCTION: {
                lisp_value target = CAR(args);
                if (symbolp(target)) {
                    if (!func_bound(ctx, target))
                        lisp_error("undefined function %s", SYM(target)->name);
                } else {
                    analyse_expr(ctx, target);
                }
                return;
            }
            case SF_IF:
            case SF_PROGN:
            case SF_AND:
            case SF_OR:
            case SF_WHEN:
            case SF_UNLESS:
                analyse_args(ctx, args);
                return;
            case SF_LET:
            case SF_LET_STAR: {
                int saved = ctx->var_top;
                lisp_value bindings = CAR(args);
                bool star = id == SF_LET_STAR;
                lisp_value b = bindings;
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
                    if (!nilp(init))
                        analyse_expr(ctx, init);
                    if (star)
                        analyse_push_var(ctx, name);
                    else
                        analyse_push_var(ctx, name);
                }
                analyse_args(ctx, CDR(args));
                ctx->var_top = saved;
                return;
            }
            case SF_SETQ: {
                lisp_value p = args;
                while (consp(p) && consp(CDR(p))) {
                    lisp_value sym = CAR(p);
                    if (!symbolp(sym))
                        lisp_error("bad setq target");
                    if (!var_bound(ctx, sym))
                        lisp_error("undefined variable %s", SYM(sym)->name);
                    analyse_expr(ctx, CAR(CDR(p)));
                    p = CDR(CDR(p));
                }
                return;
            }
            case SF_FLET:
            case SF_LABELS: {
                int saved_funcs = ctx->func_top;
                lisp_value bindings = CAR(args);
                lisp_value b = bindings;
                while (consp(b)) {
                    lisp_value binding = CAR(b);
                    b = CDR(b);
                    analyse_push_func(ctx, CAR(binding));
                }
                b = bindings;
                while (consp(b)) {
                    lisp_value binding = CAR(b);
                    b = CDR(b);
                    int saved_vars = ctx->var_top;
                    analyse_lambda_list(ctx, CAR(CDR(binding)));
                    analyse_args(ctx, CDR(CDR(binding)));
                    ctx->var_top = saved_vars;
                }
                analyse_args(ctx, CDR(args));
                ctx->func_top = saved_funcs;
                return;
            }
            case SF_LAMBDA: {
                int saved_vars = ctx->var_top;
                analyse_lambda_list(ctx, CAR(args));
                analyse_args(ctx, CDR(args));
                ctx->var_top = saved_vars;
                return;
            }
            case SF_BLOCK:
                analyse_args(ctx, CDR(args));
                return;
            case SF_RETURN_FROM:
                if (consp(CDR(args)))
                    analyse_expr(ctx, CAR(CDR(args)));
                return;
            case SF_RETURN:
                if (consp(args))
                    analyse_expr(ctx, CAR(args));
                return;
            case SF_MULTIPLE_VALUE_LIST:
                if (consp(args))
                    analyse_expr(ctx, CAR(args));
                return;
            case SF_MULTIPLE_VALUE_BIND: {
                lisp_value vars = CAR(args);
                if (consp(CDR(args)))
                    analyse_expr(ctx, CAR(CDR(args)));
                int saved = ctx->var_top;
                lisp_value v = vars;
                while (consp(v)) {
                    analyse_push_var(ctx, CAR(v));
                    v = CDR(v);
                }
                analyse_args(ctx, CDR(CDR(args)));
                ctx->var_top = saved;
                return;
            }
            case SF_MULTIPLE_VALUE_SETQ: {
                lisp_value vars = CAR(args);
                lisp_value v = vars;
                while (consp(v)) {
                    lisp_value sym = CAR(v);
                    if (symbolp(sym) && !var_bound(ctx, sym))
                        lisp_error("undefined variable %s", SYM(sym)->name);
                    v = CDR(v);
                }
                if (consp(CDR(args)))
                    analyse_expr(ctx, CAR(CDR(args)));
                return;
            }
            case SF_DECLARE:
            case SF_DECLAIM:
            case SF_IN_PACKAGE:
                return;
            case SF_DEFUN: {
                lisp_value name = CAR(args);
                int saved = ctx->var_top;
                analyse_lambda_list(ctx, CAR(CDR(args)));
                analyse_args(ctx, CDR(CDR(args)));
                ctx->var_top = saved;
                (void)name;
                return;
            }
            case SF_DEFPARAMETER:
            case SF_DEFVAR:
                if (consp(CDR(args)))
                    analyse_expr(ctx, CAR(CDR(args)));
                return;
            case SF_DEFMACRO: {
                int saved = ctx->var_top;
                analyse_lambda_list(ctx, CAR(CDR(args)));
                analyse_args(ctx, CDR(CDR(args)));
                ctx->var_top = saved;
                return;
            }
            case SF_COND: {
                lisp_value clauses = args;
                while (consp(clauses)) {
                    lisp_value clause = CAR(clauses);
                    clauses = CDR(clauses);
                    analyse_args(ctx, clause);
                }
                return;
            }
            case SF_CASE:
            case SF_ECASE: {
                analyse_expr(ctx, CAR(args));
                lisp_value clauses = CDR(args);
                while (consp(clauses)) {
                    lisp_value clause = CAR(clauses);
                    clauses = CDR(clauses);
                    analyse_args(ctx, CDR(clause));
                }
                return;
            }
            case SF_DOLIST:
            case SF_DOTIMES: {
                lisp_value spec = CAR(args);
                if (consp(CDR(spec)))
                    analyse_expr(ctx, CAR(CDR(spec)));
                int saved = ctx->var_top;
                analyse_push_var(ctx, CAR(spec));
                analyse_args(ctx, CDR(args));
                if (consp(CDR(CDR(spec))))
                    analyse_expr(ctx, CAR(CDR(CDR(spec))));
                ctx->var_top = saved;
                return;
            }
            case SF_DESTRUCTURING_BIND: {
                analyse_expr(ctx, CAR(CDR(args)));
                int saved = ctx->var_top;
                analyse_push_pattern(ctx, CAR(args));
                analyse_args(ctx, CDR(CDR(args)));
                ctx->var_top = saved;
                return;
            }
            case SF_IGNORE_ERRORS:
                analyse_args(ctx, args);
                return;
            case SF_HANDLER_CASE: {
                analyse_expr(ctx, CAR(args));
                lisp_value clauses = CDR(args);
                while (consp(clauses)) {
                    lisp_value clause = CAR(clauses);
                    clauses = CDR(clauses);
                    int saved = ctx->var_top;
                    lisp_value var_spec = CAR(CDR(clause));
                    lisp_value var = consp(var_spec) ? CAR(var_spec) : var_spec;
                    analyse_push_var(ctx, var);
                    analyse_args(ctx, CDR(CDR(clause)));
                    ctx->var_top = saved;
                }
                return;
            }
            case SF_UNWIND_PROTECT:
                analyse_expr(ctx, CAR(args));
                analyse_args(ctx, CDR(args));
                return;
            case SF_SETF: {
                lisp_value p = args;
                while (consp(p) && consp(CDR(p))) {
                    analyse_expr(ctx, CAR(p));
                    analyse_expr(ctx, CAR(CDR(p)));
                    p = CDR(CDR(p));
                }
                return;
            }
            case SF_PUSH:
            case SF_PUSHNEW:
            case SF_POP:
            case SF_INCF:
            case SF_DECF:
                analyse_expr(ctx, CAR(args));
                if (consp(CDR(args)))
                    analyse_expr(ctx, CAR(CDR(args)));
                return;
            case SF_ROTATEF:
                analyse_args(ctx, args);
                return;
            case SF_LOOP:
                loop_analyse(ctx, args);
                return;
            case SF_QUASIQUOTE:
                analyse_quasiquote(ctx, CAR(args), 1);
                return;
            case SF_DEFINE_EXTENSION: {
                analyse_expr(ctx, CAR(args));
                lisp_value options = CAR(CDR(args));
                lisp_value opt = options;
                while (consp(opt) && consp(CDR(opt))) {
                    lisp_value key = CAR(opt);
                    lisp_value value = CAR(CDR(opt));
                    if (lisp_eq_keyword(key, "STATE") || lisp_eq_keyword(key, "ADMISSION"))
                        analyse_expr(ctx, value);
                    opt = CDR(CDR(opt));
                }
                int saved = ctx->var_top;
                analyse_lambda_list(ctx, CAR(CDR(CDR(args))));
                analyse_args(ctx, CDR(CDR(CDR(args))));
                ctx->var_top = saved;
                return;
            }
            case SF_EVAL:
                analyse_expr(ctx, CAR(args));
                return;
            case SF_PROG1:
                analyse_args(ctx, args);
                return;
            case SF_CHECK_TYPE:
                analyse_expr(ctx, CAR(args));
                return;
            default:
                analyse_args(ctx, args);
                return;
            }
        }
        if (SYM(head)->h.flags & SYM_MACRO) {
            int argc = 0;
            lisp_value argv[64];
            lisp_value macro_args = args;
            while (consp(macro_args)) {
                if (argc >= 64)
                    lisp_error("too many macro arguments");
                argv[argc++] = CAR(macro_args);
                macro_args = CDR(macro_args);
            }
            lisp_value expansion = apply_function(SYM(head)->function, argc, argv);
            analyse_expr(ctx, expansion);
            return;
        }
        if (func_bound(ctx, head)) {
            analyse_args(ctx, args);
            return;
        }
        lisp_error("undefined function %s", SYM(head)->name);
    }
    analyse_expr(ctx, head);
    analyse_args(ctx, args);
}

void loop_analyse(struct lisp_actx *ctx, lisp_value clauses)
{
    int n = 0;
    lisp_value tokens[512];
    lisp_value p = clauses;
    while (consp(p) && n < 512) {
        tokens[n++] = CAR(p);
        p = CDR(p);
    }
    for (int i = 0; i < n; i++) {
        lisp_value token = tokens[i];
        if (!symbolp(token))
            continue;
        const char *name = SYM(token)->name;
        if (strcmp(name, "FOR") == 0 || strcmp(name, "AS") == 0) {
            if (i + 1 < n)
                analyse_push_pattern(ctx, tokens[++i]);
        } else if (strcmp(name, "WITH") == 0) {
            if (i + 1 < n)
                analyse_push_pattern(ctx, tokens[++i]);
        } else if (strcmp(name, "INTO") == 0) {
            if (i + 1 < n)
                analyse_push_var(ctx, tokens[++i]);
        }
    }
    for (int i = 0; i < n; i++) {
        lisp_value token = tokens[i];
        if (!consp(token)) {
            if (symbolp(token)) {
                const char *name = SYM(token)->name;
                if (strcmp(name, "COLLECT") == 0 || strcmp(name, "APPEND") == 0 ||
                    strcmp(name, "NCONC") == 0 || strcmp(name, "SUM") == 0 ||
                    strcmp(name, "COUNT") == 0 || strcmp(name, "MAXIMIZE") == 0 ||
                    strcmp(name, "MINIMIZE") == 0 || strcmp(name, "WHILE") == 0 ||
                    strcmp(name, "REPEAT") == 0 || strcmp(name, "THEREIS") == 0 ||
                    strcmp(name, "RETURN") == 0 || strcmp(name, "WHEN") == 0 ||
                    strcmp(name, "UNLESS") == 0) {
                    if (i + 1 < n && consp(tokens[i + 1]))
                        analyse_expr(ctx, tokens[i + 1]);
                } else if (strcmp(name, "FOR") == 0 || strcmp(name, "AS") == 0) {
                    if (i + 1 < n)
                        i++;
                    if (i + 1 < n && consp(tokens[i + 1]))
                        analyse_expr(ctx, tokens[i + 1]);
                } else if (strcmp(name, "WITH") == 0) {
                    if (i + 1 < n)
                        i++;
                    if (i + 1 < n && symbolp(tokens[i + 1]) &&
                        strcmp(SYM(tokens[i + 1])->name, "=") == 0)
                        i++;
                    if (i + 1 < n && consp(tokens[i + 1]))
                        analyse_expr(ctx, tokens[i + 1]);
                }
            }
            continue;
        }
        analyse_expr(ctx, token);
    }
}

void lisp_analyse_form(lisp_value form, lisp_value file_functions, lisp_value file_variables)
{
    struct lisp_actx ctx;
    actx_init(&ctx, file_functions, file_variables);
    analyse_expr(&ctx, form);
}
