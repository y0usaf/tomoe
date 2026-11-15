#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "lisp-internal.h"

struct embedded_source {
    char name[64];
    const char *text;
    size_t length;
};

static struct embedded_source embedded[64];
static int embedded_count;

static struct embedded_source *embedded_find(const char *name)
{
    for (int i = 0; i < embedded_count; i++)
        if (strcmp(embedded[i].name, name) == 0)
            return &embedded[i];
    return NULL;
}

static bool definition_head(lisp_value sym, const char **kind)
{
    if (!symbolp(sym))
        return false;
    const char *name = SYM(sym)->name;
    if (strcmp(name, "DEFUN") == 0 || strcmp(name, "DEFMACRO") == 0 ||
        strcmp(name, "DEFINE-EXTENSION") == 0) {
        *kind = "function";
        return true;
    }
    if (strcmp(name, "DEFPARAMETER") == 0 || strcmp(name, "DEFVAR") == 0) {
        *kind = "variable";
        return true;
    }
    return false;
}

static lisp_value list_append_cell(lisp_value *head, lisp_value *tail, lisp_value value)
{
    lisp_value cell = cons(value, NIL_VAL);
    if (nilp(*head))
        *head = cell;
    else
        CDR(*tail) = cell;
    *tail = cell;
    return cell;
}

static void collect_definitions(lisp_value form, lisp_value *funcs, lisp_value *func_tail,
    lisp_value *vars, lisp_value *var_tail)
{
    if (!consp(form))
        return;
    lisp_value head = CAR(form);
    if (head == VAL(lisp.sym_quote))
        return;
    lisp_value args = CDR(form);
    const char *kind = NULL;
    if (definition_head(head, &kind)) {
        lisp_value name = CAR(args);
        if (symbolp(name)) {
            if (strcmp(kind, "function") == 0)
                list_append_cell(funcs, func_tail, name);
            else
                list_append_cell(vars, var_tail, name);
        }
        return;
    }
    while (consp(form)) {
        collect_definitions(CAR(form), funcs, func_tail, vars, var_tail);
        form = CDR(form);
    }
}

static int analyse_on = 1;

static int load_source_internal(const char *name, const char *text, size_t length,
    uint32_t package, struct lisp_region *region, bool analyse, char *error, size_t error_size)
{
    struct lisp_region *saved_region = lisp.cur_region;
    uint32_t saved_package = lisp.cur_package;
    lisp.cur_region = region;
    lisp.cur_package = package;
    lisp.load_depth++;

    int index = push_ctl(CTL_CATCH);
    if (setjmp(lisp.ctl[index].jb) != 0) {
        lisp.ctl_top = index;
        if (error && error_size) {
            lisp_value message = condition_message(lisp_escape_condition());
            size_t length = 0;
            const char *text = lisp_get_string(message, &length);
            if (text && length)
                snprintf(error, error_size, "%.*s", (int)length, text);
            else
                snprintf(error, error_size, "%s", lisp_error_message());
        }
        lisp.cur_region = saved_region;
        lisp.cur_package = saved_package;
        lisp.load_depth--;
        lisp_clear_location();
        return -1;
    }

    bool allow_backquote = package == PKG_TOMOE;
    struct lisp_reader rd;
    reader_init(&rd, name, text, length, package, allow_backquote);

    int cap = 256;
    lisp_value *forms = heap_alloc(sizeof(lisp_value) * (size_t)cap);
    int *lines = heap_alloc(sizeof(int) * (size_t)cap);
    int count = 0;
    while (!reader_done(&rd)) {
        if (count >= cap) {
            int bigger = cap * 2;
            lisp_value *nf = heap_alloc(sizeof(lisp_value) * (size_t)bigger);
            int *nl = heap_alloc(sizeof(int) * (size_t)bigger);
            memcpy(nf, forms, sizeof(lisp_value) * (size_t)count);
            memcpy(nl, lines, sizeof(int) * (size_t)count);
            forms = nf;
            lines = nl;
            cap = bigger;
        }
        int line = 0;
        forms[count] = reader_next(&rd, false, &line);
        if (rd.pos >= rd.len && reader_done(&rd) && count >= 0) {
        }
        lines[count] = line;
        count++;
    }

    lisp_value funcs = NIL_VAL, func_tail = NIL_VAL;
    lisp_value vars = NIL_VAL, var_tail = NIL_VAL;
    for (int i = 0; i < count; i++)
        collect_definitions(forms[i], &funcs, &func_tail, &vars, &var_tail);

    lisp_value results = NIL_VAL, result_tail = NIL_VAL;
    for (int i = 0; i < count; i++) {
        lisp_set_error_location(name, lines[i]);
        if (analyse && analyse_on)
            lisp_analyse_form(forms[i], funcs, vars);
        lisp_value value = eval(forms[i], NULL);
        lisp_clear_location();
        list_append_cell(&results, &result_tail, value);
    }

    lisp.ctl_top = index;
    lisp.cur_region = saved_region;
    lisp.cur_package = saved_package;
    lisp.load_depth--;
    lisp_clear_location();
    lisp.mv_count = 1;
    lisp.mv[0] = results;
    return 0;
}

static struct lisp_region *new_generation(const char *name, uint32_t package)
{
    struct lisp_region *gen = region_new(REG_GENERATION, name, package);
    gen->next = lisp.generations;
    lisp.generations = gen;
    return gen;
}

lisp_value builtin_load_source(int argc, lisp_value *argv)
{
    if (argc < 3)
        lisp_error("%%LOAD-SOURCE needs a name, text and a package");
    char name[64];
    if (symbolp(argv[0]))
        snprintf(name, sizeof(name), "%s", SYM(argv[0])->name);
    else if (stringp(argv[0]))
        snprintf(name, sizeof(name), "%s", PTR(argv[0], lisp_string)->data);
    else
        lisp_error("%%LOAD-SOURCE needs a source name");
    const char *text = NULL;
    size_t length = 0;
    if (nilp(argv[1])) {
        struct embedded_source *found = embedded_find(name);
        if (!found)
            lisp_error("no embedded source named %s", name);
        text = found->text;
        length = found->length;
    } else {
        text = require_string(argv[1], &length, "source text");
    }
    uint32_t package = PKG_TOMOE_USER;
    if (symbolp(argv[2]))
        package = package_find_or_create(SYM(argv[2])->name);
    else if (stringp(argv[2]))
        package = package_find_or_create(PTR(argv[2], lisp_string)->data);
    else if (!nilp(argv[2]))
        lisp_error("%%LOAD-SOURCE needs a package name");
    struct lisp_region *saved = lisp.cur_region;
    struct lisp_region *gen = new_generation(name, package);
    lisp.cur_region = gen;
    char error[1024] = { 0 };
    int status = load_source_internal(name, text, length, package, gen, true, error, sizeof(error));
    lisp.cur_region = saved;
    if (status != 0)
        lisp_signal_error(error);
    return lisp.mv[0];
}

lisp_value builtin_drop_package(int argc, lisp_value *argv)
{
    if (argc < 1)
        lisp_error("%%DROP-PACKAGE needs a name");
    const char *name = symbolp(argv[0]) ? SYM(argv[0])->name :
                       stringp(argv[0]) ? PTR(argv[0], lisp_string)->data : NULL;
    if (!name)
        lisp_error("%%DROP-PACKAGE needs a name");
    struct lisp_region *gen = lisp.generations;
    while (gen) {
        struct lisp_region *next = gen->next;
        if (strcmp(gen->name, name) == 0) {
            if (gen->pin_count == 0)
                region_destroy(gen);
            else
                gen->dropped = true;
        }
        gen = next;
    }
    uint32_t package = package_find(name);
    if (package != UINT32_MAX)
        package_drop(package);
    return NIL_VAL;
}

lisp_value builtin_export(int argc, lisp_value *argv)
{
    if (argc < 1)
        return NIL_VAL;
    lisp_value list = argv[0];
    while (consp(list)) {
        lisp_value item = CAR(list);
        list = CDR(list);
        if (symbolp(item))
            item = VAL(symbol_intern(PKG_TOMOE, SYM(item)->name, SYM(item)->name_len, false));
        else if (stringp(item))
            item = VAL(symbol_intern(PKG_TOMOE, PTR(item, lisp_string)->data,
                                     PTR(item, lisp_string)->byte_len, false));
        else
            continue;
        SYM(item)->h.flags |= SYM_VISIBLE;
        lisp.packages[PKG_TOMOE].export_count++;
    }
    return NIL_VAL;
}

int lisp_load_builtins(const struct lisp_builtin_source *sources, size_t count,
    char *error, size_t error_size)
{
    if (error && error_size)
        error[0] = '\0';
    embedded_count = 0;
    for (size_t i = 0; i < count && embedded_count < 64; i++) {
        struct embedded_source *slot = &embedded[embedded_count];
        snprintf(slot->name, sizeof(slot->name), "%s", sources[i].name);
        slot->text = sources[i].text;
        slot->length = sources[i].length;
        embedded_count++;
    }
    struct lisp_region *gen = new_generation("<builtins>", PKG_TOMOE);
    struct lisp_region *saved_region = lisp.cur_region;
    uint32_t saved_package = lisp.cur_package;
    lisp.cur_region = gen;
    lisp.cur_package = PKG_TOMOE;
    int index = push_ctl(CTL_TOP);
    int status = 0;
    if (setjmp(lisp.ctl[index].jb) != 0) {
        lisp.ctl_top = index;
        if (error && error_size) {
            lisp_value message = condition_message(lisp_escape_condition());
            size_t length = 0;
            const char *text = lisp_get_string(message, &length);
            if (text && length)
                snprintf(error, error_size, "%.*s", (int)length, text);
            else
                snprintf(error, error_size, "%s", lisp_error_message());
        }
        status = -1;
    } else {
        for (size_t i = 0; i < count; i++) {
            if (strcmp(sources[i].name, "desktop") == 0 ||
                strcmp(sources[i].name, "default-config") == 0)
                continue;
            char message[1024] = { 0 };
            lisp_set_current_package(PKG_TOMOE);
            if (load_source_internal(sources[i].name, sources[i].text, sources[i].length,
                                     PKG_TOMOE, gen, false, message, sizeof(message)) != 0) {
                if (error && error_size)
                    snprintf(error, error_size, "%s", message);
                status = -1;
                break;
            }
            lisp_set_current_package(PKG_TOMOE);
        }
        lisp.ctl_top = index;
    }
    lisp.cur_region = saved_region;
    lisp.cur_package = saved_package;
    lisp_clear_location();
    return status;
}
