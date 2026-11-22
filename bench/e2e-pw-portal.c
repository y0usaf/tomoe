#define _GNU_SOURCE
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <drm_fourcc.h>
#include <pipewire/pipewire.h>
#include <spa/buffer/buffer.h>
#include <spa/param/format-utils.h>
#include <spa/param/video/format-utils.h>
#include <spa/pod/builder.h>

struct consumer {
    struct pw_main_loop *loop;
    struct pw_stream *stream;
    struct spa_source *timer;
    bool require_modifier;
    bool shm_only;
    bool saw_unfixated;
    bool saw_fixated;
    bool have_modifier;
    uint64_t modifier;
    uint32_t data_type;
    int frames;
    int width;
    int height;
    int target_frames;
    int exit_code;
};

static void on_state_changed(void *userdata, enum pw_stream_state old, enum pw_stream_state state,
        const char *error) {
    struct consumer *c = userdata;
    if (state == PW_STREAM_STATE_ERROR) {
        fprintf(stderr, "e2e-pw-portal: stream error: %s\n", error ? error : "unknown");
        c->exit_code = 1;
        pw_main_loop_quit(c->loop);
    }
}

static void on_param_changed(void *userdata, uint32_t id, const struct spa_pod *param) {
    struct consumer *c = userdata;
    if (!param || id != SPA_PARAM_Format) return;
    struct spa_video_info_raw info;
    memset(&info, 0, sizeof(info));
    if (spa_format_video_raw_parse(param, &info) < 0) return;
    const struct spa_pod_prop *prop = spa_pod_find_prop(param, NULL, SPA_FORMAT_VIDEO_modifier);
    if (prop) {
        c->have_modifier = true;
        c->modifier = info.modifier;
        if (prop->flags & SPA_POD_PROP_FLAG_DONT_FIXATE) {
            c->saw_unfixated = true;
            return;
        }
    } else if (c->require_modifier) {
        return;
    }
    c->saw_fixated = true;
    c->width = (int)info.size.width;
    c->height = (int)info.size.height;
    uint8_t buf[512];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
    uint32_t data_types = (1u << SPA_DATA_DmaBuf) | (1u << SPA_DATA_MemFd);
    const struct spa_pod *params[1];
    params[0] = spa_pod_builder_add_object(&b,
        SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
        SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(2, 1, 8),
        SPA_PARAM_BUFFERS_blocks, SPA_POD_CHOICE_RANGE_Int(1, 1, 4),
        SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int(data_types));
    pw_stream_update_params(c->stream, params, 1);
}

static void on_process(void *userdata) {
    struct consumer *c = userdata;
    struct pw_buffer *b = pw_stream_dequeue_buffer(c->stream);
    if (!b) return;
    if (b->buffer && b->buffer->n_datas > 0) {
        c->data_type = b->buffer->datas[0].type;
    }
    c->frames++;
    pw_stream_queue_buffer(c->stream, b);
    if (c->saw_fixated && c->frames >= c->target_frames) {
        if (c->shm_only && c->data_type != SPA_DATA_MemFd) {
            fprintf(stderr, "e2e-pw-portal: expected SPA_DATA_MemFd (%u), got %u\n",
                (unsigned)SPA_DATA_MemFd, c->data_type);
            c->exit_code = 1;
        } else {
            c->exit_code = 0;
        }
        pw_main_loop_quit(c->loop);
    }
}

static void on_timeout(void *userdata, uint64_t expirations) {
    struct consumer *c = userdata;
    fprintf(stderr,
        "e2e-pw-portal: timeout (require_modifier=%d saw_unfixated=%d saw_fixated=%d "
        "have_modifier=%d modifier=0x%llx frames=%d)\n",
        c->require_modifier, c->saw_unfixated, c->saw_fixated, c->have_modifier,
        (unsigned long long)c->modifier, c->frames);
    c->exit_code = 1;
    pw_main_loop_quit(c->loop);
}

static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .state_changed = on_state_changed,
    .param_changed = on_param_changed,
    .process = on_process,
};

int main(int argc, char **argv) {
    uint32_t target_id = PW_ID_ANY;
    int timeout_sec = 8;
    int target_frames = 1;
    bool require_modifier = true;
    bool shm_only = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--node") == 0 && i + 1 < argc) {
            target_id = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--timeout") == 0 && i + 1 < argc) {
            timeout_sec = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            target_frames = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--allow-shm") == 0) {
            require_modifier = false;
        } else if (strcmp(argv[i], "--shm-only") == 0) {
            require_modifier = false;
            shm_only = true;
        }
    }
    if (target_id == PW_ID_ANY) {
        fprintf(stderr, "e2e-pw-portal: missing --node <id>\n");
        return 1;
    }
    pw_init(&argc, &argv);
    struct consumer c;
    memset(&c, 0, sizeof(c));
    c.require_modifier = require_modifier;
    c.shm_only = shm_only;
    c.target_frames = target_frames;
    c.exit_code = 1;
    c.loop = pw_main_loop_new(NULL);
    if (!c.loop) return 1;
    struct pw_properties *props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Video",
        PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_MEDIA_ROLE, "Screen",
        NULL);
    c.stream = pw_stream_new_simple(
        pw_main_loop_get_loop(c.loop),
        "tomoe-e2e-pw-portal",
        props,
        &stream_events,
        &c);
    if (!c.stream) return 1;
    uint8_t buf_mod[1024];
    struct spa_pod_builder bm = SPA_POD_BUILDER_INIT(buf_mod, sizeof(buf_mod));
    struct spa_pod_frame f0, f1;
    spa_pod_builder_push_object(&bm, &f0, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
    spa_pod_builder_add(&bm,
        SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
        SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
        SPA_FORMAT_VIDEO_format, SPA_POD_CHOICE_ENUM_Id(5,
            SPA_VIDEO_FORMAT_BGRx,
            SPA_VIDEO_FORMAT_BGRx,
            SPA_VIDEO_FORMAT_BGRA,
            SPA_VIDEO_FORMAT_RGBx,
            SPA_VIDEO_FORMAT_RGBA),
        0);
    spa_pod_builder_prop(&bm, SPA_FORMAT_VIDEO_modifier,
        SPA_POD_PROP_FLAG_MANDATORY | SPA_POD_PROP_FLAG_DONT_FIXATE);
    spa_pod_builder_push_choice(&bm, &f1, SPA_CHOICE_Enum, 0);
    spa_pod_builder_long(&bm, (int64_t)DRM_FORMAT_MOD_LINEAR);
    spa_pod_builder_long(&bm, (int64_t)DRM_FORMAT_MOD_LINEAR);
    spa_pod_builder_pop(&bm, &f1);
    spa_pod_builder_add(&bm,
        SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(
            &SPA_RECTANGLE(1280, 720), &SPA_RECTANGLE(1, 1), &SPA_RECTANGLE(16384, 16384)),
        SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(
            &SPA_FRACTION(60, 1), &SPA_FRACTION(0, 1), &SPA_FRACTION(1000, 1)),
        0);
    const struct spa_pod *params[2];
    uint32_t n_params = 0;
    if (!shm_only) {
        params[n_params++] = spa_pod_builder_pop(&bm, &f0);
    }
    uint8_t buf_shm[1024];
    if (!require_modifier) {
        struct spa_pod_builder bs = SPA_POD_BUILDER_INIT(buf_shm, sizeof(buf_shm));
        params[n_params++] = spa_pod_builder_add_object(&bs,
            SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
            SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
            SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
            SPA_FORMAT_VIDEO_format, SPA_POD_CHOICE_ENUM_Id(5,
                SPA_VIDEO_FORMAT_BGRx,
                SPA_VIDEO_FORMAT_BGRx,
                SPA_VIDEO_FORMAT_BGRA,
                SPA_VIDEO_FORMAT_RGBx,
                SPA_VIDEO_FORMAT_RGBA),
            SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(
                &SPA_RECTANGLE(1280, 720), &SPA_RECTANGLE(1, 1), &SPA_RECTANGLE(16384, 16384)),
            SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(
                &SPA_FRACTION(60, 1), &SPA_FRACTION(0, 1), &SPA_FRACTION(1000, 1)));
    }
    struct timespec ts = { .tv_sec = timeout_sec, .tv_nsec = 0 };
    c.timer = pw_loop_add_timer(pw_main_loop_get_loop(c.loop), on_timeout, &c);
    pw_loop_update_timer(pw_main_loop_get_loop(c.loop), c.timer, &ts, NULL, false);
    int res = pw_stream_connect(c.stream, PW_DIRECTION_INPUT, target_id,
        PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS, params, n_params);
    if (res < 0) {
        fprintf(stderr, "e2e-pw-portal: pw_stream_connect failed: %d\n", res);
        return 1;
    }
    pw_main_loop_run(c.loop);
    if (c.exit_code == 0) {
        printf("e2e-pw-portal: PASS node=%u size=%dx%d fixated=%d modifier=0x%llx data_type=%u frames=%d\n",
            target_id, c.width, c.height, c.saw_fixated, (unsigned long long)c.modifier,
            c.data_type, c.frames);
    }
    pw_stream_destroy(c.stream);
    pw_main_loop_destroy(c.loop);
    pw_deinit();
    return c.exit_code;
}
