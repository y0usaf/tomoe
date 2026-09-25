#include "internal.h"
#include "ui.h"

void presentation_finish(struct tomoe *s) {
    if (!s || !s->presentation) return;
    ui_set_finish(s->presentation->ui);
    presentation_bindings_finish(s->presentation);
    keyboard_profile_finish(s->presentation->keyboard);
    if (s->presentation->settings) settings_finish(s->presentation->settings);
    free(s->presentation->settings);
    free(s->presentation->outputs);
    free(s->presentation->targets);
    free(s->presentation);
    s->presentation = NULL;
}

void tomoe_present_abort(struct tomoe *s) {
    presentation_finish(s);
}

struct presentation_target *presentation_target_for(struct presentation *plan, uint32_t id) {
    for (size_t i = 0; i < plan->target_count; i++)
        if (plan->targets[i].target.id == id) return &plan->targets[i];
    return NULL;
}

const struct presentation_output *presentation_output_for(const struct presentation *plan,
        struct wlr_output *output) {
    for (size_t i = 0; i < plan->output_count; i++)
        if (plan->outputs[i].output->wlr == output) return &plan->outputs[i];
    return NULL;
}

const struct presentation_output *presentation_output_at(const struct presentation *plan,
        double x, double y) {
    for (size_t i = 0; i < plan->output_count; i++) {
        const struct presentation_output *output = &plan->outputs[i];
        const struct wlr_box *box = &output->box;
        if (x >= box->x && y >= box->y &&
                x < (double)box->x + box->width && y < (double)box->y + box->height)
            return output;
    }
    return NULL;
}

void presentation_protocol_to_screen(const struct presentation *plan, double *x, double *y) {
    double reference = plan->output_count ? plan->outputs[0].scale_120 / 120.0 : 1.0;
    for (size_t i = 0; i < plan->output_count; i++) {
        const struct presentation_output *output = &plan->outputs[i];
        double scale = output->scale_120 / 120.0;
        int lx = pixel_round(output->box.x / reference);
        int ly = pixel_round(output->box.y / reference);
        int width = (int)((int64_t)output->box.width * 120 / output->scale_120);
        int height = (int)((int64_t)output->box.height * 120 / output->scale_120);
        if (*x >= lx && *y >= ly && *x < (double)lx + width && *y < (double)ly + height) {
            *x = output->box.x + (*x - lx) * scale;
            *y = output->box.y + (*y - ly) * scale;
            return;
        }
    }
    *x *= reference;
    *y *= reference;
}

int tomoe_present_begin(struct tomoe *s, int view_x, int view_y,
        double zoom, uint32_t focus, int restack, int outputs_changed,
        int replace_bindings, uint32_t grab_id, int grab_mode) {
    presentation_finish(s);
    if (!isfinite(zoom) || zoom < 1.0 / 16.0 || zoom > 16.0 ||
            grab_mode < 0 || grab_mode > 3) return 0;
    struct presentation *plan = calloc(1, sizeof(*plan));
    if (!plan) return 0;
    wl_list_init(&plan->bindings);
    plan->outputs_changed = outputs_changed != 0;
    plan->replace_bindings = replace_bindings != 0;
    plan->grab_id = grab_mode ? grab_id : 0;
    plan->grab_mode = grab_mode;
    s->presentation = plan;
    plan->output_count = output_active_count(s, plan->outputs_changed);
    if (plan->output_count == SIZE_MAX) {
        presentation_finish(s);
        return 0;
    }
    size_t capacity = (size_t)wl_list_length(&s->windows) + wl_list_length(&s->layers);
    plan->outputs = calloc(plan->output_count ? plan->output_count : 1, sizeof(*plan->outputs));
    plan->targets = calloc(capacity ? capacity : 1, sizeof(*plan->targets));
    if (!plan->outputs || !plan->targets || !presentation_outputs(s, plan)) {
        presentation_finish(s);
        return 0;
    }
    plan->view_x = view_x;
    plan->view_y = view_y;
    plan->view_zoom = zoom;
    plan->focused = focus;
    plan->restack = restack != 0;
    struct wlr_scene_tree *bands[] = { s->layer_tree[0], s->layer_tree[1],
        s->window_tree, s->layer_tree[2], s->fullscreen_tree,
        s->unmanaged_tree, s->layer_tree[3] };
    for (size_t band = 0; band < sizeof(bands) / sizeof(bands[0]); band++) {
        struct wlr_scene_node *node;
        wl_list_for_each(node, &bands[band]->children, link) {
            if (!node->data) continue;
            if (plan->target_count == capacity) {
                presentation_finish(s);
                return 0;
            }
            struct presentation_target *entry = &plan->targets[plan->target_count++];
            entry->node = node;
            entry->target = *(struct target *)node->data;
            entry->layer_x = node->x;
            entry->layer_y = node->y;
            entry->visible = node->enabled;
            entry->desired_visible = node->enabled;
            entry->band = (int)band;
            entry->order = plan->next_order++;
        }
    }
    return 1;
}

int tomoe_present_window(struct tomoe *s, uint32_t id, int x, int y,
        int width, int height, int visible, int fullscreen, int maximize) {
    struct presentation *plan = s->presentation;
    if (!plan || width < 1 || height < 1) return 0;
    struct presentation_target *entry = presentation_target_for(plan, id);
    if (!entry || !find_window_registered(s, id)) return 1;
    entry->target.x = x;
    entry->target.y = y;
    entry->width = width;
    entry->height = height;
    entry->desired_visible = visible != 0;
    entry->visible = visible != 0;
    entry->fullscreen = fullscreen != 0;
    entry->maximize = maximize != 0;
    entry->staged = true;
    if (plan->restack) entry->order = plan->next_order++;
    return 1;
}

int tomoe_present_window_style(struct tomoe *s, uint32_t id, int radius, int blur,
        int tearing, int64_t focused, int64_t unfocused) {
    struct presentation *plan = s->presentation;
    if (!plan) return 0;
    struct presentation_target *entry = presentation_target_for(plan, id);
    if (entry) entry->target.style = (struct window_style){ radius, blur, tearing, focused, unfocused };
    return 1;
}

int tomoe_present_stack(struct tomoe *s, uint32_t id) {
    struct presentation *plan = s->presentation;
    if (!plan) return 0;
    plan->explicit_stacking = true;
    struct presentation_target *entry = presentation_target_for(plan, id);
    if (!entry || !find_window_registered(s, id)) return 1;
    entry->order = plan->next_order++;
    return 1;
}

static int compare_targets(const void *a, const void *b) {
    const struct presentation_target *left = a, *right = b;
    if (left->band != right->band) return left->band < right->band ? -1 : 1;
    return (left->order > right->order) - (left->order < right->order);
}

bool presentation_prepare(struct tomoe *s) {
    struct presentation *plan = s->presentation;
    if (!plan || !ui_prepare(s) || !layers_prepare_presentation(s, plan)) return false;
    windows_prepare_presentation(s, plan);
    struct presentation_target *focus = presentation_target_for(plan, plan->focused);
    if (!plan->explicit_stacking && (plan->restack || s->focused != plan->focused) &&
            focus && focus->target.kind == TARGET_WINDOW)
        focus->order = plan->next_order++;
    qsort(plan->targets, plan->target_count, sizeof(*plan->targets), compare_targets);
    return true;
}

void presentation_publish(struct tomoe *s) {
    struct presentation *plan = s->presentation;
    s->view_x = plan->view_x;
    s->view_y = plan->view_y;
    s->view_zoom = plan->view_zoom;
    windows_publish_presentation(s, plan);
    if (s->failed) return;
    layers_publish_presentation(s);
    if (s->failed) return;
    ui_publish(s);
    if (!keyboard_profile_publish(s, plan)) {
        fail(s, "keyboard publication failed");
        return;
    }
    presentation_input_publish(s, plan);
    settings_publish(s, plan);
    uint32_t previous_focus = s->focused;
    if (plan->restack || s->focused != plan->focused) tomoe_focus(s, plan->focused);
    if (s->focused == previous_focus) update_keyboard_focus(s);
    for (size_t i = 0; i < plan->target_count; i++)
        wlr_scene_node_raise_to_top(plan->targets[i].node);
    schedule_scene(s);
}

const char *tomoe_present_apply(struct tomoe *s) {
    if (!presentation_prepare(s)) return "Cannot prepare candidate presentation.";
    if (s->presentation->outputs_changed) return tomoe_outputs_apply(s);
    presentation_publish(s);
    return s->failed ? "Presentation publication failed; stopping the compositor." : NULL;
}
