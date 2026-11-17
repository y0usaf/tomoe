#include <pipewire/pipewire.h>
#include <spa/buffer/buffer.h>
#include <spa/buffer/meta.h>
#include <spa/param/buffers.h>
#include <spa/param/video/raw-utils.h>
#include <spa/param/video/raw.h>
#include <spa/pod/builder.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define CURSOR_EDGE 256

enum { MODE_DMABUF, MODE_SHM };

struct consumer {
    struct pw_main_loop *main_loop;
    struct pw_context *context;
    struct pw_core *core;
    struct pw_stream *stream;
    struct spa_hook stream_listener;
    struct spa_hook registry_listener;
    struct spa_hook core_listener;
    struct pw_registry *registry;
    struct spa_source *retry;
    struct spa_source *deadline;
    unsigned target;
    unsigned node;
    unsigned producer_port;
    unsigned consumer_port;
    bool linked;
    int mode;
    int frames;
    bool reported_alloc;
    uint32_t last_format;
    uint32_t last_modifier;
    uint32_t last_width;
    uint32_t last_height;
    uint64_t started;
    uint64_t duration;
    char last_cursor[256];
    bool bitmap;
    struct { int x, y; } probe[2];
    int probes;
};

static uint64_t block_hash(const struct spa_buffer *buffer, int x, int y) {
    if (!buffer || buffer->n_datas == 0) return 0;
    const struct spa_data *data = &buffer->datas[0];
    if (!data->data || !data->chunk || data->chunk->stride <= 0 || x < 0 || y < 0) return 0;
    int32_t stride = data->chunk->stride;
    uint64_t hash = 1469598103934665603ull;
    for (int row = 0; row < 16; row++) {
        size_t offset = (size_t)(y + row) * (size_t)stride + (size_t)x * 4;
        if (offset + 64 > data->maxsize) return 0;
        for (int column = 0; column < 64; column++) {
            hash ^= ((const uint8_t *)data->data)[offset + column];
            hash *= 1099511628211ull;
        }
    }
    return hash;
}

static uint64_t now_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
}

static void link_if_ready(struct consumer *consumer) {
    if (consumer->linked || !consumer->node || !consumer->consumer_port ||
            !consumer->producer_port) return;
    struct pw_properties *properties = pw_properties_new(NULL, NULL);
    pw_properties_setf(properties, PW_KEY_LINK_OUTPUT_NODE, "%u", consumer->target);
    pw_properties_setf(properties, PW_KEY_LINK_OUTPUT_PORT, "%u", consumer->producer_port);
    pw_properties_setf(properties, PW_KEY_LINK_INPUT_NODE, "%u", consumer->node);
    pw_properties_setf(properties, PW_KEY_LINK_INPUT_PORT, "%u", consumer->consumer_port);
    void *link = pw_core_create_object(consumer->core, "link-factory", PW_TYPE_INTERFACE_Link,
        PW_VERSION_LINK, &properties->dict, 0);
    pw_properties_free(properties);
    if (!link) return;
    consumer->linked = true;
    printf("link %u %u\n", consumer->target, consumer->node);
    fflush(stdout);
}

static void registry_global(void *data, uint32_t id, uint32_t permissions, const char *type,
        uint32_t version, const struct spa_dict *props) {
    struct consumer *consumer = data;
    if (!props || !type || strcmp(type, PW_TYPE_INTERFACE_Port) != 0) return;
    const char *direction = spa_dict_lookup(props, PW_KEY_PORT_DIRECTION);
    const char *node = spa_dict_lookup(props, PW_KEY_NODE_ID);
    if (!direction || !node) return;
    unsigned owner = (unsigned)strtoul(node, NULL, 10);
    if (owner == consumer->node && strcmp(direction, "in") == 0 && !consumer->consumer_port) {
        consumer->consumer_port = id;
        link_if_ready(consumer);
    } else if (owner == consumer->target && strcmp(direction, "out") == 0 &&
            !consumer->producer_port) {
        consumer->producer_port = id;
        link_if_ready(consumer);
    }
}

static void registry_global_remove(void *data, uint32_t id) {
}

static const struct pw_registry_events registry_events = {
    PW_VERSION_REGISTRY_EVENTS,
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static void on_core_error(void *data, uint32_t id, int seq, int result, const char *message) {
    fprintf(stderr, "consumer: core error %d: %s\n", result, message ? message : "");
    fflush(stderr);
}

static const struct pw_core_events core_events = {
    PW_VERSION_CORE_EVENTS,
    .error = on_core_error,
};

static void report_cursor(struct consumer *consumer, struct spa_buffer *buffer) {
    struct spa_meta *meta = spa_buffer_find_meta(buffer, SPA_META_Cursor);
    if (!meta || !meta->data || meta->size < sizeof(struct spa_meta_cursor)) return;
    struct spa_meta_cursor *cursor = meta->data;
    char line[256];
    char described[64];
    const char *bitmap = "none";
    if (cursor->bitmap_offset && cursor->bitmap_offset + sizeof(struct spa_meta_bitmap) <=
            meta->size) {
        struct spa_meta_bitmap *map = (struct spa_meta_bitmap *)((uint8_t *)cursor +
            cursor->bitmap_offset);
        if (spa_meta_bitmap_is_valid(map)) {
            snprintf(described, sizeof(described), "%ux%u@%u", map->size.width, map->size.height,
                map->format);
            bitmap = described;
            consumer->bitmap = true;
        }
    }
    snprintf(line, sizeof(line), "cursor visible=%d position=%d,%d hotspot=%d,%d bitmap=%s",
        spa_meta_cursor_is_valid(cursor) ? 1 : 0, cursor->position.x, cursor->position.y,
        cursor->hotspot.x, cursor->hotspot.y, bitmap);
    if (strcmp(line, consumer->last_cursor) == 0) return;
    snprintf(consumer->last_cursor, sizeof(consumer->last_cursor), "%s", line);
    printf("%s\n", line);
    fflush(stdout);
}

static void on_process(void *data) {
    struct consumer *consumer = data;
    struct pw_buffer *buffer = pw_stream_dequeue_buffer(consumer->stream);
    if (!buffer) return;
    struct spa_buffer *spa = buffer->buffer;
    if (spa && spa->n_datas > 0) {
        if (!consumer->reported_alloc) {
            printf("alloc %s\n", spa->datas[0].type == SPA_DATA_DmaBuf ? "dmabuf" : "memfd");
            fflush(stdout);
            consumer->reported_alloc = true;
        }
        report_cursor(consumer, spa);
    }
    if (consumer->probes == 2)
        printf("probe %llx %llx\n",
            (unsigned long long)block_hash(spa, consumer->probe[0].x, consumer->probe[0].y),
            (unsigned long long)block_hash(spa, consumer->probe[1].x, consumer->probe[1].y));
    consumer->frames++;
    fflush(stdout);
    pw_stream_queue_buffer(consumer->stream, buffer);
}

static void on_param_changed(void *data, uint32_t id, const struct spa_pod *param) {
    struct consumer *consumer = data;
    if (id != SPA_PARAM_Format || !param) return;
    struct spa_video_info_raw raw;
    if (spa_format_video_raw_parse(param, &raw) < 0) return;
    bool modifier = (raw.flags & SPA_VIDEO_FLAG_MODIFIER) != 0;
    if (consumer->last_format == raw.format && consumer->last_width == raw.size.width &&
            consumer->last_height == raw.size.height &&
            consumer->last_modifier == (modifier ? 1u : 0u)) return;
    consumer->last_format = raw.format;
    consumer->last_width = raw.size.width;
    consumer->last_height = raw.size.height;
    consumer->last_modifier = modifier ? 1u : 0u;
    if (modifier)
        printf("format %u modifier 0x%llx %ux%u frames %d\n", raw.format,
            (unsigned long long)raw.modifier, raw.size.width, raw.size.height, consumer->frames);
    else
        printf("format %u modifier none %ux%u frames %d\n", raw.format, raw.size.width,
            raw.size.height, consumer->frames);
    fflush(stdout);
}

static void on_state_changed(void *data, enum pw_stream_state old, enum pw_stream_state state,
        const char *error) {
    struct consumer *consumer = data;
    if (state == PW_STREAM_STATE_PAUSED || state == PW_STREAM_STATE_STREAMING) {
        consumer->node = pw_stream_get_node_id(consumer->stream);
        if (!consumer->started) consumer->started = now_ms();
        link_if_ready(consumer);
    } else if (state == PW_STREAM_STATE_ERROR) {
        if (error) fprintf(stderr, "consumer: %s\n", error);
    }
}

static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .state_changed = on_state_changed,
    .param_changed = on_param_changed,
    .process = on_process,
};

static void on_retry(void *data, uint64_t expirations) {
    struct consumer *consumer = data;
    if (!consumer->linked) link_if_ready(consumer);
}

static void on_alarm(int signal_number) {
    printf("frames 0 0 bitmap 0\n");
    fflush(stdout);
    _exit(0);
}

static void on_deadline(void *data, uint64_t expirations) {
    struct consumer *consumer = data;
    uint64_t elapsed = consumer->started ? now_ms() - consumer->started : 0;
    printf("frames %d %llu bitmap %d\n", consumer->frames, (unsigned long long)elapsed,
        consumer->bitmap ? 1 : 0);
    fflush(stdout);
    pw_main_loop_quit(consumer->main_loop);
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s <node-id> <dmabuf|shm> <duration-ms> [x y x y]\n", argv[0]);
        return 2;
    }
    struct consumer consumer = {0};
    consumer.target = (unsigned)strtoul(argv[1], NULL, 10);
    consumer.mode = strcmp(argv[2], "shm") == 0 ? MODE_SHM : MODE_DMABUF;
    consumer.duration = (uint64_t)strtoul(argv[3], NULL, 10);
    if (argc >= 8) {
        consumer.probe[0].x = (int)strtoul(argv[4], NULL, 10);
        consumer.probe[0].y = (int)strtoul(argv[5], NULL, 10);
        consumer.probe[1].x = (int)strtoul(argv[6], NULL, 10);
        consumer.probe[1].y = (int)strtoul(argv[7], NULL, 10);
        consumer.probes = 2;
    }
    signal(SIGALRM, on_alarm);
    pw_init(NULL, NULL);
    consumer.main_loop = pw_main_loop_new(NULL);
    consumer.context = pw_context_new(pw_main_loop_get_loop(consumer.main_loop), NULL, 0);
    consumer.core = pw_context_connect(consumer.context, NULL, 0);
    if (!consumer.core) {
        fprintf(stderr, "consumer: cannot reach PipeWire\n");
        return 1;
    }
    pw_core_add_listener(consumer.core, &consumer.core_listener, &core_events, &consumer);
    consumer.registry = pw_core_get_registry(consumer.core, PW_VERSION_REGISTRY, 0);
    pw_registry_add_listener(consumer.registry, &consumer.registry_listener, &registry_events,
        &consumer);
    struct pw_properties *properties = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Video",
        PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_MEDIA_ROLE, "Screen",
        PW_KEY_NODE_NAME, "portal-test-consumer",
        NULL);
    consumer.stream = pw_stream_new(consumer.core, "portal-test-consumer", properties);
    pw_stream_add_listener(consumer.stream, &consumer.stream_listener, &stream_events, &consumer);
    uint8_t buffer[2048];
    struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    const struct spa_pod *params[5];
    int count = 0;
    struct spa_rectangle size = SPA_RECTANGLE(1, 1);
    struct spa_rectangle limit = SPA_RECTANGLE(8192, 8192);
    struct spa_fraction rate = SPA_FRACTION(0, 1);
    struct spa_fraction limit_rate = SPA_FRACTION(240, 1);
    if (consumer.mode == MODE_DMABUF) {
        params[count++] = spa_pod_builder_add_object(&builder,
            SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
            SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
            SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
            SPA_FORMAT_VIDEO_format, SPA_POD_CHOICE_ENUM_Id(3, SPA_VIDEO_FORMAT_BGRx,
                SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_RGBx),
            SPA_FORMAT_VIDEO_modifier,
            SPA_POD_CHOICE_RANGE_Long(0, 0, (long long)0x00ffffffffffffffULL),
            SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(&size, &size, &limit),
            SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(&rate, &rate, &limit_rate));
    }
    params[count++] = spa_pod_builder_add_object(&builder,
        SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
        SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
        SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
        SPA_FORMAT_VIDEO_format, SPA_POD_CHOICE_ENUM_Id(3, SPA_VIDEO_FORMAT_BGRx,
            SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_RGBx),
        SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(&size, &size, &limit),
        SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(&rate, &rate, &limit_rate));
    params[count++] = spa_pod_builder_add_object(&builder,
        SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
        SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Cursor),
        SPA_PARAM_META_size, SPA_POD_Int((int)(sizeof(struct spa_meta_cursor) +
            sizeof(struct spa_meta_bitmap) + CURSOR_EDGE * CURSOR_EDGE * 4)));
    uint32_t types = consumer.mode == MODE_DMABUF
        ? (1u << SPA_DATA_DmaBuf) | (1u << SPA_DATA_MemFd) : (1u << SPA_DATA_MemFd);
    params[count++] = spa_pod_builder_add_object(&builder,
        SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
        SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(8, 2, 16),
        SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int((int)types));
    if (pw_stream_connect(consumer.stream, PW_DIRECTION_INPUT, PW_ID_ANY,
            PW_STREAM_FLAG_MAP_BUFFERS, params, (uint32_t)count) < 0) {
        fprintf(stderr, "consumer: cannot open the stream\n");
        return 1;
    }
    consumer.retry = pw_loop_add_timer(pw_main_loop_get_loop(consumer.main_loop), on_retry,
        &consumer);
    struct timespec retry = { .tv_sec = 0, .tv_nsec = 200000000L };
    pw_loop_update_timer(pw_main_loop_get_loop(consumer.main_loop), consumer.retry, &retry, &retry,
        false);
    consumer.deadline = pw_loop_add_timer(pw_main_loop_get_loop(consumer.main_loop), on_deadline,
        &consumer);
    long long total = (long long)(consumer.duration + 3000) * 1000000LL;
    struct timespec deadline = { .tv_sec = (time_t)(total / 1000000000LL),
        .tv_nsec = (long)(total % 1000000000LL) };
    pw_loop_update_timer(pw_main_loop_get_loop(consumer.main_loop), consumer.deadline, &deadline,
        &deadline, false);
    alarm((unsigned)((consumer.duration + 8000) / 1000));
    pw_main_loop_run(consumer.main_loop);
    pw_stream_destroy(consumer.stream);
    pw_core_disconnect(consumer.core);
    pw_context_destroy(consumer.context);
    pw_main_loop_destroy(consumer.main_loop);
    pw_deinit();
    return 0;
}
