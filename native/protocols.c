#include "internal.h"
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_linux_drm_syncobj_v1.h>

void drag_icons_refresh(struct tomoe *s) {
    struct output *o = output_at_physical(s, s->pointer_x, s->pointer_y);
    s->drag_icon.x = pixel_round(s->pointer_x);
    s->drag_icon.y = pixel_round(s->pointer_y);
    s->drag_icon.scale = o ? snapped_scale(o->wlr->scale) : reference_scale(s);
    if (!wl_list_empty(&s->drag_icon_tree->children)) schedule_scene(s);
}
static bool syncobj_listen(struct tomoe *s) {
    if (!s->renderer->features.timeline || !s->backend->features.timeline) return true;
    int fd = wlr_renderer_get_drm_fd(s->renderer);
    return fd < 0 || wlr_linux_drm_syncobj_manager_v1_create(s->display, 1, fd);
}

bool protocols_listen(struct tomoe *s) {
    s->presentation_time = wlr_presentation_create(s->display, s->backend, 2);
    if (!s->presentation_time || !idle_listen(s) || !gamma_listen(s) ||
            !decoration_listen(s) || !foreign_listen(s) || !tearing_listen(s) ||
            !pointer_protocols_listen(s)) return false;
    s->drag_icon.kind = TARGET_ICON;
    return syncobj_listen(s);
}
