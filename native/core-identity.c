#include "internal.h"
#include "backend.h"
#include "lisp.h"

#include <stdio.h>

#ifndef TOMOE_CORE_STAMP
#define TOMOE_CORE_STAMP ""
#endif

#ifdef TOMOE_SHARED_CORE
__attribute__((weak)) char __executable_start;
__attribute__((weak)) char etext;
#endif

const char *tomoe_core_stamp(void) {
    return TOMOE_CORE_STAMP[0] ? TOMOE_CORE_STAMP : TOMOE_VERSION;
}

const char *tomoe_core_abi(void) {
    static char text[512];
    size_t sizes[] = {
        sizeof(struct tomoe), sizeof(struct surface), sizeof(struct node),
        sizeof(struct output), sizeof(struct screen), sizeof(struct layer),
        sizeof(struct buffer), sizeof(struct addon), sizeof(struct input_device),
        sizeof(struct box), sizeof(struct fbox), sizeof(struct texture),
        sizeof(struct op), sizeof(struct oplist), sizeof(struct timeline),
        sizeof(struct ring), sizeof(struct pointer_motion), sizeof(struct window_shader),
        sizeof(struct window_mesh), sizeof(struct screen_mode), sizeof(struct cursor_image),
        sizeof(struct profile), sizeof(struct dmabuf_attributes), sizeof(struct format),
        sizeof(struct keymap_slot), sizeof(struct scene_quad), sizeof(struct positioner_rules),
        sizeof(struct texture_options), sizeof(struct rect_options), sizeof(struct read_options),
        sizeof(struct render_counters), sizeof(struct keyboard_modifiers),
        sizeof(struct mesh_options), sizeof(struct window_path_sample), sizeof(struct screen_overlay),
        sizeof(struct lisp_heap_stats), sizeof(struct lisp_builtin_source), sizeof(lisp_value)
    };
    size_t used = (size_t)snprintf(text, sizeof(text), "%d", tomoe_abi_version());
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]) && used < sizeof(text); i++)
        used += (size_t)snprintf(text + used, sizeof(text) - used, "/%zu", sizes[i]);
    return text;
}
