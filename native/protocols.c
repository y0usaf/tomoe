#include "internal.h"

void drag_icons_refresh(struct tomoe *s) {
    struct output *o = output_at_physical(s, s->pointer_x, s->pointer_y);
    s->drag_icon.x = pixel_round(s->pointer_x);
    s->drag_icon.y = pixel_round(s->pointer_y);
    s->drag_icon.scale = o ? snapped_scale(o->screen->scale) : reference_scale(s);
    if (!wl_list_empty(&s->drag_icon_tree->children)) schedule_scene(s);
}
bool protocols_listen(struct tomoe *s) {
    if (!idle_listen(s) || !gamma_listen(s) || !power_listen(s) ||
            !decoration_listen(s) || !foreign_listen(s) || !tearing_listen(s) ||
            !pointer_protocols_listen(s)) return false;
    s->drag_icon.kind = TARGET_ICON;
    return true;
}
