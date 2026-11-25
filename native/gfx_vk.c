#include "gfx.h"
#include "drm.h"
#include "upload.h"
#include <dirent.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <vulkan/vulkan.h>
#include "shaders/scene.vert.h"
#include "shaders/scene.frag.h"
#include "shaders/blur.vert.h"
#include "shaders/down.frag.h"
#include "shaders/up.frag.h"
#include "shaders/down.comp.h"
#include "shaders/up.comp.h"
#include "shaders/lut.frag.h"
#include "shaders/ext.frag.h"
#include "shaders/glass.frag.h"
#include "shader-vk.h"

enum {
    BLENDS = 3,
    TARGET_FORMATS = 8,
    POOL_SETS = 64,
    CHUNK_MIN = 256 << 10,
    CHUNK_SPARE_MAX = 2,
    SCENE_RESERVE = 256,
    BATCH_LIMIT = 32,
    LEVEL_POOL = 8,
    YUV_FORMATS = 3,
    YUV_POOL_SETS = 32,
};

struct vk;

struct vk_image {
    struct addon addon;
    struct wl_list link;
    struct vk *v;
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view, sample;
    VkDescriptorSet set, storage_set;
    VkDescriptorPool pool, storage_pool;
    VkFormat format;
    VkImageUsageFlags usage;
    uint32_t drm_format;
    int width, height;
    bool foreign, target, owned, used;
    uint64_t batch, written, copied, sync_rw;
    int fd, yuv;
};

struct vk_texture {
    struct texture base;
    struct vk *v;
    struct vk_image *image;
    struct buffer *buffer;
    uint32_t format;
    bool alpha, external;
    uint64_t bytes;
};

struct grave {
    struct grave *next;
    uint64_t serial;
    struct chunk *chunk;
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view, sample;
    VkDescriptorSet set, storage_set;
    VkDescriptorPool pool, storage_pool;
    VkShaderModule module;
    VkPipeline pipelines[TARGET_FORMATS];
};

struct chunk {
    struct chunk *next;
    VkDescriptorSet ubo;
    VkBuffer buffer;
    VkDeviceMemory memory;
    unsigned char *map;
    VkDeviceSize size, used;
};

struct batch {
    VkCommandBuffer pre, main;
    VkFence fence;
    VkSemaphore signal;
    VkSemaphore *waits;
    VkSemaphoreSubmitInfo *wait_infos;
    int wait_count, wait_made;
    struct chunk *chunks;
    VkDeviceSize staged;
    struct vk_image **acquired;
    size_t acquired_len, acquired_cap;
    VkQueryPool queries;
    int64_t submit_ns;
    uint64_t serial;
    bool busy, has_ts, is_frame;
};

struct scene_instance {
    float pos[8], local[8], tex[8];
    float color[4], tint[4], params[4], extra[4];
    float shape[4];
};

struct scene_item {
    struct scene_instance in;
    struct vk_image *image;
    int filter, blend;
    int32_t scissor[4];
};

struct scene {
    struct scene_item *items;
    size_t count, cap;
    struct pass *owner;
};

struct ext_push {
    float tint[4];
    float size[2];
    float alpha, opaque, clip, radius;
};

struct yuv_path {
    bool supported, midpoint, linear;
    uint64_t modifiers[32];
    int modifier_count;
    VkSamplerYcbcrConversion conversion;
    VkSampler sampler;
    VkDescriptorSetLayout set_layout;
    VkPipelineLayout layout;
    VkDescriptorPool *pools;
    size_t pool_count;
    VkFormat targets[TARGET_FORMATS];
    VkPipeline pipelines[TARGET_FORMATS];
};

struct blur_push {
    float half_pixel[2];
    float offset, grain, vibrancy;
};

struct blur_comp_push {
    float half_pixel[2];
    float uv_step[2];
    float uv_origin[2];
    float uv_min[2];
    float uv_max[2];
    int32_t rect_off[2];
    int32_t rect_end[2];
    float offset, grain, vibrancy;
};

struct glass_push {
    float box[4];
    float resolution[2];
    float bend;
};

struct gfx_prog {
    int builtin;
    struct gfx_prog *next;
    struct vk *v;
    uint64_t hash;
    uint32_t *code;
    size_t words;
    int refs;
    uint32_t reads;
    VkShaderModule frag;
    VkFormat formats[TARGET_FORMATS];
    VkPipeline pipelines[TARGET_FORMATS];
};

struct user_uniforms {
    float mouse[4], marks_box[4], beneath_box[4], frost_box[4];
    float resolution[3];
    float time;
    int32_t frame, trail_count, window_count, reserved;
    float trail[POINTER_TRAIL][4];
    float data[SHADER_DATA / 4][4];
    float windows[SHADER_WINDOWS][4];
    float selection[4][4];
};

_Static_assert(sizeof(struct user_uniforms) == 2080, "std140 TomoeUniforms is 2080 bytes");

struct target_pipelines {
    VkFormat format;
    VkPipeline blend[BLENDS];
    VkPipeline lut, glass;
};

struct vk {
    struct render base;
    VkInstance instance;
    VkPhysicalDevice phys;
    VkDevice dev;
    VkQueue queue;
    uint32_t family;
    VkPhysicalDeviceMemoryProperties memory;
    VkDeviceSize atom;
    uint32_t max_extent;
    VkPipelineCache pipeline_cache;
    char *pipeline_cache_path;
    size_t pipeline_cache_bytes;
    uint32_t vendor_id, device_id;
    uint8_t cache_uuid[VK_UUID_SIZE];
    PFN_vkGetMemoryFdPropertiesKHR memory_fd_properties;
    PFN_vkGetMemoryFdKHR memory_fd;
    PFN_vkGetImageDrmFormatModifierPropertiesEXT image_modifier;
    PFN_vkGetSemaphoreFdKHR semaphore_fd;
    PFN_vkImportSemaphoreFdKHR import_semaphore;
    bool sync_fd;
    VkCommandPool command_pool;
    struct batch *batches;
    size_t batch_count;
    struct batch *open;
    uint64_t serial, done;
    uint64_t latches;
    uint64_t chunks_made, chunk_bytes, images_made, fence_waits, levels_made, imports_made;
    struct grave *graves;
    struct chunk *spare;
    VkDescriptorSetLayout image_layout, sampler_layout, storage_layout;
    VkDescriptorPool sampler_pool;
    VkDescriptorPool *pools, *storage_pools;
    size_t pool_count, storage_pool_count;
    VkSampler samplers[2];
    VkDescriptorSet sampler_sets[2];
    VkPipelineLayout layout;
    VkShaderModule vert, frag, blur_vert, down_frag, up_frag, down_comp_mod, up_comp_mod,
        lut_frag, ext_frag, glass_frag;
    bool ycbcr, queue_compute, compute_blur;
    struct yuv_path yuv[YUV_FORMATS];
    VkPipelineLayout blur_layout, blur_comp_layout, lut_layout, glass_layout, user_layout;
    VkDescriptorSetLayout ubo_layout;
    VkDescriptorPool ubo_pool;
    VkDeviceSize ubo_align;
    struct gfx_prog *user_progs;
    bool lost;
    uint64_t submits, lose_at;
    struct gfx_prog glass_program;
    VkPipeline down, up, down_comp, up_comp;
    struct target_pipelines targets[TARGET_FORMATS];
    struct wl_list images;
    struct vk_image *dummy;
    struct vk_image *target, *bound;
    VkRect2D render_area;
    struct vk_image *pool[LEVEL_POOL];
    int pooled;
    struct {
        struct chunk *chunk;
        uint64_t serial[UPLOAD_FENCES], limit[UPLOAD_FENCES];
        int fences;
        bool failed, unfenced;
    } staging;
    bool rendering;
    bool timestamps, in_target;
    double ts_period;
    uint32_t ts_bits;
    FILE *ts_log;
    struct scene scene;
};

static uint64_t serials;
static struct vk *current_vk;

static void level_pool_drain(struct vk *v);
static bool wait_fd(struct vk *v, int fd);
struct vk_texture;
struct ext_push;
static void external_quad(struct pass *pass, struct vk_texture *t, const float pos[8],
    const float local[8], const float texcoords[8], const struct region *clip,
    const struct ext_push *push);

enum { LEVEL_FORMAT = VK_FORMAT_B8G8R8A8_UNORM };

static inline struct vk *V(struct render *r) {
    return (struct vk *)r;
}

static const struct pixel {
    uint32_t drm;
    VkFormat vk;
    bool alpha;
} pixels[] = {
    { DRM_FORMAT_ARGB8888, VK_FORMAT_B8G8R8A8_UNORM, true },
    { DRM_FORMAT_XRGB8888, VK_FORMAT_B8G8R8A8_UNORM, false },
    { DRM_FORMAT_ABGR8888, VK_FORMAT_R8G8B8A8_UNORM, true },
    { DRM_FORMAT_XBGR8888, VK_FORMAT_R8G8B8A8_UNORM, false },
    { DRM_FORMAT_ARGB2101010, VK_FORMAT_A2R10G10B10_UNORM_PACK32, true },
    { DRM_FORMAT_XRGB2101010, VK_FORMAT_A2R10G10B10_UNORM_PACK32, false },
    { DRM_FORMAT_ABGR2101010, VK_FORMAT_A2B10G10R10_UNORM_PACK32, true },
    { DRM_FORMAT_XBGR2101010, VK_FORMAT_A2B10G10R10_UNORM_PACK32, false },
    { DRM_FORMAT_ABGR16161616F, VK_FORMAT_R16G16B16A16_SFLOAT, true },
    { DRM_FORMAT_XBGR16161616F, VK_FORMAT_R16G16B16A16_SFLOAT, false },
    { DRM_FORMAT_RGB565, VK_FORMAT_R5G6B5_UNORM_PACK16, false },
};

static const struct yuv_format {
    uint32_t drm;
    VkFormat vk;
} yuv_formats[YUV_FORMATS] = {
    { DRM_FORMAT_NV12, VK_FORMAT_G8_B8R8_2PLANE_420_UNORM },
    { DRM_FORMAT_P010, VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16 },
    { DRM_FORMAT_YUV420, VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM },
};

static int yuv_for(uint32_t drm) {
    for (int i = 0; i < YUV_FORMATS; i++)
        if (yuv_formats[i].drm == drm) return i;
    return -1;
}

static const struct pixel *pixel_for(uint32_t drm) {
    for (size_t i = 0; i < sizeof(pixels) / sizeof(pixels[0]); i++)
        if (pixels[i].drm == drm) return &pixels[i];
    return NULL;
}

static bool shm_format(uint32_t drm) {
    return drm == DRM_FORMAT_ARGB8888 || drm == DRM_FORMAT_XRGB8888 ||
        drm == DRM_FORMAT_ABGR8888 || drm == DRM_FORMAT_XBGR8888;
}

static bool format_alpha(uint32_t format) {
    const struct pixel *p = pixel_for(format);
    return p && p->alpha;
}

static int memory_type(struct vk *v, uint32_t bits, VkMemoryPropertyFlags need,
        VkMemoryPropertyFlags want) {
    int fallback = -1;
    for (uint32_t i = 0; i < v->memory.memoryTypeCount; i++) {
        if (!(bits & (1u << i))) continue;
        VkMemoryPropertyFlags flags = v->memory.memoryTypes[i].propertyFlags;
        if ((flags & need) != need) continue;
        if ((flags & want) == want) return (int)i;
        if (fallback < 0) fallback = (int)i;
    }
    return fallback;
}

static void stage_barrier(VkCommandBuffer cb, VkPipelineStageFlags2 src_stage,
        VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access) {
    VkMemoryBarrier2 memory = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask = src_stage,
        .srcAccessMask = src_access,
        .dstStageMask = dst_stage,
        .dstAccessMask = dst_access,
    };
    VkDependencyInfo info = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &memory,
    };
    vkCmdPipelineBarrier2(cb, &info);
}

static void memory_barrier(struct vk *v, VkCommandBuffer cb,
        VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
        VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access) {
    (void)v;
    stage_barrier(cb, src_stage, src_access, dst_stage, dst_access);
}

static void full_barrier(struct vk *v, VkCommandBuffer cb) {
    memory_barrier(v, cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
}

static void image_sync(VkCommandBuffer cb, VkImage image,
        VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
        VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access,
        VkImageLayout from, VkImageLayout to, uint32_t src_family, uint32_t dst_family) {
    VkImageMemoryBarrier2 barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = src_stage,
        .srcAccessMask = src_access,
        .dstStageMask = dst_stage,
        .dstAccessMask = dst_access,
        .oldLayout = from,
        .newLayout = to,
        .srcQueueFamilyIndex = src_family,
        .dstQueueFamilyIndex = dst_family,
        .image = image,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    VkDependencyInfo info = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &barrier,
    };
    vkCmdPipelineBarrier2(cb, &info);
}

static void image_barrier(VkCommandBuffer cb, VkImage image, VkImageLayout from, VkImageLayout to,
        uint32_t src_family, uint32_t dst_family) {
    image_sync(cb, image, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
        from, to, src_family, dst_family);
}

static void chunk_free(struct vk *v, struct chunk *c) {
    if (c->ubo) vkFreeDescriptorSets(v->dev, v->ubo_pool, 1, &c->ubo);
    if (c->map) vkUnmapMemory(v->dev, c->memory);
    vkDestroyBuffer(v->dev, c->buffer, NULL);
    vkFreeMemory(v->dev, c->memory, NULL);
    free(c);
}

static struct chunk *chunk_new(struct vk *v, VkDeviceSize size, VkBufferUsageFlags usage,
        VkMemoryPropertyFlags want) {
    struct chunk *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    VkBufferCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    if (vkCreateBuffer(v->dev, &info, NULL, &c->buffer) != VK_SUCCESS) {
        free(c);
        return NULL;
    }
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(v->dev, c->buffer, &req);
    int type = memory_type(v, req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, want);
    VkMemoryAllocateInfo alloc = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size,
        .memoryTypeIndex = (uint32_t)type,
    };
    void *map = NULL;
    if (type < 0 || vkAllocateMemory(v->dev, &alloc, NULL, &c->memory) != VK_SUCCESS) {
        vkDestroyBuffer(v->dev, c->buffer, NULL);
        free(c);
        return NULL;
    }
    if (vkBindBufferMemory(v->dev, c->buffer, c->memory, 0) != VK_SUCCESS ||
            vkMapMemory(v->dev, c->memory, 0, VK_WHOLE_SIZE, 0, &map) != VK_SUCCESS) {
        chunk_free(v, c);
        return NULL;
    }
    c->map = map;
    c->size = size;
    v->chunks_made++;
    v->chunk_bytes += size;
    return c;
}

static void bury_now(struct vk *v, struct grave *g) {
    if (g->chunk) chunk_free(v, g->chunk);
    for (int i = 0; i < TARGET_FORMATS; i++)
        if (g->pipelines[i]) vkDestroyPipeline(v->dev, g->pipelines[i], NULL);
    if (g->module) vkDestroyShaderModule(v->dev, g->module, NULL);
    if (g->set) vkFreeDescriptorSets(v->dev, g->pool, 1, &g->set);
    if (g->storage_set) vkFreeDescriptorSets(v->dev, g->storage_pool, 1, &g->storage_set);
    if (g->sample && g->sample != g->view) vkDestroyImageView(v->dev, g->sample, NULL);
    if (g->view) vkDestroyImageView(v->dev, g->view, NULL);
    if (g->image) vkDestroyImage(v->dev, g->image, NULL);
    if (g->memory) vkFreeMemory(v->dev, g->memory, NULL);
}

static void graves_sweep(struct vk *v, bool all) {
    struct grave **link = &v->graves;
    while (*link) {
        struct grave *g = *link;
        if (!all && g->serial > v->done) {
            link = &g->next;
            continue;
        }
        *link = g->next;
        bury_now(v, g);
        free(g);
    }
}

static int64_t ts_now_ns(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000000000LL + now.tv_nsec;
}

static void batch_recycle(struct vk *v, struct batch *b) {
    size_t spare_count = 0;
    for (struct chunk *s = v->spare; s; s = s->next) spare_count++;
    while (b->chunks) {
        struct chunk *c = b->chunks;
        b->chunks = c->next;
        if (c->size > CHUNK_MIN * 2 || spare_count >= CHUNK_SPARE_MAX) {
            chunk_free(v, c);
            continue;
        }
        c->used = 0;
        c->next = v->spare;
        v->spare = c;
        spare_count++;
    }
    b->acquired_len = 0;
    b->wait_count = 0;
    b->staged = 0;
    b->has_ts = false;
    b->is_frame = false;
    b->submit_ns = 0;
}

static void device_lost(struct vk *v, const char *where) {
    if (v->lost) return;
    v->lost = true;
    tomoe_log(LOG_ERROR, "tomoe: the Vulkan device was lost (%s); Tomoe exits so its supervisor "
        "can start it on a fresh device", where);
    loop_request_fail("the Vulkan device was lost");
}

static VkResult fence_wait(struct vk *v, VkFence fence) {
    if (v->lost) return VK_ERROR_DEVICE_LOST;
    VkResult result = vkWaitForFences(v->dev, 1, &fence, VK_TRUE, UINT64_MAX);
    if (result == VK_ERROR_DEVICE_LOST) device_lost(v, "a fence wait");
    return result;
}

static void retire(struct vk *v, bool wait) {
    for (size_t i = 0; i < v->batch_count; i++) {
        struct batch *b = &v->batches[i];
        if (!b->busy) continue;
        if (v->lost) {
            b->busy = false;
            continue;
        }
        if (wait) fence_wait(v, b->fence);
        VkResult status = v->lost ? VK_ERROR_DEVICE_LOST : vkGetFenceStatus(v->dev, b->fence);
        if (status == VK_ERROR_DEVICE_LOST) {
            device_lost(v, "a fence status");
            b->busy = false;
            continue;
        }
        if (status != VK_SUCCESS) continue;
        if (b->has_ts && b->is_frame && b->queries && !v->lost) {
            uint64_t ts[2] = { 0, 0 };
            if (vkGetQueryPoolResults(v->dev, b->queries, 0, 2, sizeof(ts), ts, sizeof(ts[0]),
                    VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS) {
                uint64_t mask = v->ts_bits >= 64 ? UINT64_MAX : ((1ULL << v->ts_bits) - 1);
                uint64_t ticks = (ts[1] - ts[0]) & mask;
                uint64_t ns = (uint64_t)llround((double)ticks * v->ts_period);
                v->base.gpu_frames++;
                v->base.gpu_ns += ns;
                if (v->ts_log)
                    fprintf(v->ts_log, "%" PRId64 " %" PRIu64 "\n",
                        b->submit_ns ? b->submit_ns : ts_now_ns(), ns);
            }
        }
        vkResetFences(v->dev, 1, &b->fence);
        b->busy = false;
        if (b->serial > v->done) v->done = b->serial;
        batch_recycle(v, b);
    }
    graves_sweep(v, false);
}

static bool batch_init(struct vk *v, struct batch *b) {
    *b = (struct batch){0};
    VkCommandBuffer buffers[2];
    VkCommandBufferAllocateInfo alloc = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = v->command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 2,
    };
    if (vkAllocateCommandBuffers(v->dev, &alloc, buffers) != VK_SUCCESS) return false;
    b->pre = buffers[0];
    b->main = buffers[1];
    VkFenceCreateInfo fence = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    if (vkCreateFence(v->dev, &fence, NULL, &b->fence) != VK_SUCCESS) {
        vkFreeCommandBuffers(v->dev, v->command_pool, 2, buffers);
        return false;
    }
    if (v->sync_fd) {
        VkExportSemaphoreCreateInfo exported = {
            .sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
            .handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
        };
        VkSemaphoreCreateInfo info = {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
            .pNext = &exported,
        };
        if (vkCreateSemaphore(v->dev, &info, NULL, &b->signal) != VK_SUCCESS)
            b->signal = VK_NULL_HANDLE;
    }
    if (v->timestamps) {
        VkQueryPoolCreateInfo qp = {
            .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
            .queryType = VK_QUERY_TYPE_TIMESTAMP,
            .queryCount = 2,
        };
        if (vkCreateQueryPool(v->dev, &qp, NULL, &b->queries) != VK_SUCCESS)
            b->queries = VK_NULL_HANDLE;
    }
    return true;
}

static void batch_finish(struct vk *v, struct batch *b) {
    batch_recycle(v, b);
    if (b->queries) vkDestroyQueryPool(v->dev, b->queries, NULL);
    VkCommandBuffer buffers[2] = { b->pre, b->main };
    vkFreeCommandBuffers(v->dev, v->command_pool, 2, buffers);
    vkDestroyFence(v->dev, b->fence, NULL);
    if (b->signal) vkDestroySemaphore(v->dev, b->signal, NULL);
    for (int i = 0; i < b->wait_made; i++) vkDestroySemaphore(v->dev, b->waits[i], NULL);
    free(b->waits);
    free(b->wait_infos);
    free(b->acquired);
}

static struct batch *batch_free(struct vk *v) {
    for (size_t i = 0; i < v->batch_count; i++)
        if (!v->batches[i].busy) return &v->batches[i];
    if (v->batch_count < BATCH_LIMIT) {
        struct batch *grown = realloc(v->batches, (v->batch_count + 1) * sizeof(*grown));
        if (!grown) return NULL;
        v->batches = grown;
        if (!batch_init(v, &v->batches[v->batch_count])) return NULL;
        return &v->batches[v->batch_count++];
    }
    uint64_t oldest = UINT64_MAX;
    struct batch *pick = NULL;
    for (size_t i = 0; i < v->batch_count; i++) {
        if (v->batches[i].serial < oldest) {
            oldest = v->batches[i].serial;
            pick = &v->batches[i];
        }
    }
    fence_wait(v, pick->fence);
    v->fence_waits++;
    retire(v, false);
    return pick->busy ? NULL : pick;
}

static struct batch *batch(struct vk *v) {
    if (v->open) return v->open;
    retire(v, false);
    struct batch *b = batch_free(v);
    if (!b) {
        tomoe_log(LOG_ERROR, "tomoe: vulkan cannot open a command batch");
        return NULL;
    }
    VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vkResetCommandBuffer(b->pre, 0);
    vkResetCommandBuffer(b->main, 0);
    if (vkBeginCommandBuffer(b->pre, &begin) != VK_SUCCESS ||
            vkBeginCommandBuffer(b->main, &begin) != VK_SUCCESS) return NULL;
    b->serial = ++v->serial;
    b->is_frame = v->in_target;
    v->open = b;
    if (v->timestamps && b->queries) {
        vkCmdResetQueryPool(b->pre, b->queries, 0, 2);
        vkCmdWriteTimestamp2(b->pre, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, b->queries, 0);
        b->has_ts = true;
    }
    return b;
}

static void bury(struct vk *v, struct grave g) {
    g.serial = v->open ? v->open->serial : v->serial;
    if (g.serial <= v->done) {
        bury_now(v, &g);
        return;
    }
    struct grave *copy = malloc(sizeof(*copy));
    if (!copy) {
        vkDeviceWaitIdle(v->dev);
        bury_now(v, &g);
        return;
    }
    *copy = g;
    copy->next = v->graves;
    v->graves = copy;
}

static void *stage_alloc_in(struct vk *v, VkDeviceSize size, VkDeviceSize align,
        struct chunk **chunk, VkDeviceSize *offset) {
    struct batch *b = batch(v);
    if (!b) return NULL;
    size = (size + 15) & ~(VkDeviceSize)15;
    struct chunk *c = b->chunks;
    VkDeviceSize at = c ? (c->used + align - 1) / align * align : 0;
    if (!c || at > c->size || c->size - at < size) {
        struct chunk **link = &v->spare;
        c = NULL;
        while (*link) {
            if ((*link)->size >= size) {
                c = *link;
                *link = c->next;
                break;
            }
            link = &(*link)->next;
        }
        if (!c) {
            VkDeviceSize want = size <= CHUNK_MIN ? CHUNK_MIN :
                ((size + CHUNK_MIN - 1) / CHUNK_MIN) * CHUNK_MIN;
            c = chunk_new(v, want, VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, 0);
            if (!c) {
                tomoe_log(LOG_ERROR, "tomoe: vulkan cannot allocate %llu staging bytes",
                    (unsigned long long)want);
                return NULL;
            }
        }
        c->used = 0;
        c->next = b->chunks;
        b->chunks = c;
        at = 0;
    }
    *chunk = c;
    *offset = at;
    b->staged += size + (at - c->used);
    c->used = at + size;
    return c->map + at;
}

static void *stage_alloc(struct vk *v, VkDeviceSize size, VkBuffer *buffer, VkDeviceSize *offset) {
    struct chunk *c;
    void *at = stage_alloc_in(v, size, 16, &c, offset);
    if (at) *buffer = c->buffer;
    return at;
}

static void rendering_close(struct vk *v) {
    if (!v->rendering) return;
    vkCmdEndRendering(v->open->main);
    v->rendering = false;
    v->bound = NULL;
}

static void rendering_end(struct vk *v) {
    if (!v->rendering) return;
    VkCommandBuffer cb = v->open->main;
    rendering_close(v);
    memory_barrier(v, cb,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT |
            VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT |
            VK_PIPELINE_STAGE_2_CLEAR_BIT,
        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT |
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_TRANSFER_READ_BIT |
            VK_ACCESS_2_TRANSFER_WRITE_BIT);
}

static bool image_use_mode(struct vk *v, struct vk_image *image, bool write) {
    struct batch *b = batch(v);
    if (!b) return false;
    if (!image->foreign) return true;
    if (image->batch == b->serial) {
        if (write && !image->owned && image->fd >= 0 && v->sync_fd && image->sync_rw != b->serial) {
            struct dma_buf_export_sync_file exported = { .flags = DMA_BUF_SYNC_RW, .fd = -1 };
            if (ioctl(image->fd, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &exported) == 0 && exported.fd >= 0)
                wait_fd(v, exported.fd);
            image->sync_rw = b->serial;
        }
        return true;
    }
    if (b->acquired_len == b->acquired_cap) {
        size_t cap = b->acquired_cap ? b->acquired_cap * 2 : 16;
        struct vk_image **grown = realloc(b->acquired, cap * sizeof(*grown));
        if (!grown) return false;
        b->acquired = grown;
        b->acquired_cap = cap;
    }
    b->acquired[b->acquired_len++] = image;
    image->batch = b->serial;
    VkImageLayout from = image->used ? VK_IMAGE_LAYOUT_GENERAL :
        image->owned ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL;
    image_barrier(b->pre, image->image, from, VK_IMAGE_LAYOUT_GENERAL,
        VK_QUEUE_FAMILY_FOREIGN_EXT, v->family);
    image->used = true;
    if (!image->owned && image->fd >= 0 && v->sync_fd) {
        struct dma_buf_export_sync_file exported = {
            .flags = write ? DMA_BUF_SYNC_RW : DMA_BUF_SYNC_READ,
            .fd = -1,
        };
        if (ioctl(image->fd, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &exported) == 0 && exported.fd >= 0)
            wait_fd(v, exported.fd);
        if (write) image->sync_rw = b->serial;
    }
    return true;
}

static bool image_use(struct vk *v, struct vk_image *image) {
    return image_use_mode(v, image, false);
}

static bool image_use_write(struct vk *v, struct vk_image *image) {
    return image_use_mode(v, image, true);
}

static void release_foreign(struct vk *v, struct batch *b) {
    for (size_t i = 0; i < b->acquired_len; i++) {
        struct vk_image *image = b->acquired[i];
        if (!image) continue;
        image_barrier(b->main, image->image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
            v->family, VK_QUEUE_FAMILY_FOREIGN_EXT);
    }
}

static int submit(struct vk *v, bool export) {
    if (!v->open && !export) return -1;
    struct batch *b = batch(v);
    if (!b) return -1;
    rendering_end(v);
    release_foreign(v, b);
    if (b->has_ts && b->queries) {
        vkCmdWriteTimestamp2(b->main, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, b->queries, 1);
        b->submit_ns = ts_now_ns();
    }
    v->open = NULL;
    if (vkEndCommandBuffer(b->pre) != VK_SUCCESS || vkEndCommandBuffer(b->main) != VK_SUCCESS) {
        tomoe_log(LOG_ERROR, "tomoe: vulkan cannot end a command buffer");
        batch_recycle(v, b);
        return -1;
    }
    VkSemaphoreSubmitInfo *waits = b->wait_infos;
    for (int i = 0; i < b->wait_count; i++)
        waits[i] = (VkSemaphoreSubmitInfo){
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .semaphore = b->waits[i],
            .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        };
    VkCommandBufferSubmitInfo buffers[2] = {
        { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = b->pre },
        { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = b->main },
    };
    bool signal = export && b->signal && v->sync_fd;
    VkSemaphoreSubmitInfo signals = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = b->signal,
        .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
    };
    VkSubmitInfo2 info = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .waitSemaphoreInfoCount = (uint32_t)b->wait_count,
        .pWaitSemaphoreInfos = waits,
        .commandBufferInfoCount = 2,
        .pCommandBufferInfos = buffers,
        .signalSemaphoreInfoCount = signal ? 1 : 0,
        .pSignalSemaphoreInfos = &signals,
    };
    VkResult result = v->lost ? VK_ERROR_DEVICE_LOST : vkQueueSubmit2(v->queue, 1, &info, b->fence);
    if (result == VK_SUCCESS && v->lose_at && ++v->submits >= v->lose_at) {
        vkQueueWaitIdle(v->queue);
        result = VK_ERROR_DEVICE_LOST;
    }
    if (result == VK_ERROR_DEVICE_LOST) {
        device_lost(v, "a queue submit");
        batch_recycle(v, b);
        return -1;
    }
    if (result != VK_SUCCESS) {
        tomoe_log(LOG_ERROR, "tomoe: vulkan submit failed (%d)", (int)result);
        vkDeviceWaitIdle(v->dev);
        batch_recycle(v, b);
        return -1;
    }
    b->busy = true;
    if (!signal) return -1;
    int fd = -1;
    VkSemaphoreGetFdInfoKHR get = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
        .semaphore = b->signal,
        .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
    };
    if (v->semaphore_fd(v->dev, &get, &fd) != VK_SUCCESS) {
        tomoe_log(LOG_ERROR, "tomoe: vulkan cannot export a render fence");
        vkWaitForFences(v->dev, 1, &b->fence, VK_TRUE, UINT64_MAX);
        vkDestroySemaphore(v->dev, b->signal, NULL);
        VkExportSemaphoreCreateInfo exported = {
            .sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
            .handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
        };
        VkSemaphoreCreateInfo info = {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
            .pNext = &exported,
        };
        if (vkCreateSemaphore(v->dev, &info, NULL, &b->signal) != VK_SUCCESS)
            b->signal = VK_NULL_HANDLE;
        return -1;
    }
    for (size_t i = 0; fd >= 0 && i < b->acquired_len; i++) {
        struct vk_image *image = b->acquired[i];
        if (!image || image->fd < 0) continue;
        struct dma_buf_import_sync_file imported = {
            .flags = image->written == b->serial ? DMA_BUF_SYNC_WRITE : DMA_BUF_SYNC_READ,
            .fd = fd,
        };
        ioctl(image->fd, DMA_BUF_IOCTL_IMPORT_SYNC_FILE, &imported);
    }
    return fd;
}

static void finish(struct vk *v) {
    submit(v, false);
    retire(v, true);
}

enum { IDLE_SUBMIT_BYTES = 64 << 20 };

static void idle_submit(struct vk *v) {
    if (v->open && v->open->staged > IDLE_SUBMIT_BYTES) submit(v, false);
}

static VkDescriptorSet descriptor_new(struct vk *v, VkImageView view, VkDescriptorPool *from) {
    VkDescriptorSet set = VK_NULL_HANDLE;
    for (size_t i = v->pool_count; i > 0; i--) {
        VkDescriptorSetAllocateInfo alloc = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = v->pools[i - 1],
            .descriptorSetCount = 1,
            .pSetLayouts = &v->image_layout,
        };
        if (vkAllocateDescriptorSets(v->dev, &alloc, &set) == VK_SUCCESS) {
            *from = v->pools[i - 1];
            break;
        }
        set = VK_NULL_HANDLE;
    }
    if (!set) {
        VkDescriptorPoolSize size = { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, POOL_SETS };
        VkDescriptorPoolCreateInfo info = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
            .maxSets = POOL_SETS,
            .poolSizeCount = 1,
            .pPoolSizes = &size,
        };
        VkDescriptorPool pool;
        VkDescriptorPool *grown = realloc(v->pools, (v->pool_count + 1) * sizeof(*grown));
        if (!grown) return VK_NULL_HANDLE;
        v->pools = grown;
        if (vkCreateDescriptorPool(v->dev, &info, NULL, &pool) != VK_SUCCESS) return VK_NULL_HANDLE;
        v->pools[v->pool_count++] = pool;
        VkDescriptorSetAllocateInfo alloc = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = pool,
            .descriptorSetCount = 1,
            .pSetLayouts = &v->image_layout,
        };
        if (vkAllocateDescriptorSets(v->dev, &alloc, &set) != VK_SUCCESS) return VK_NULL_HANDLE;
        *from = pool;
    }
    VkDescriptorImageInfo image = { .imageView = view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL };
    VkWriteDescriptorSet write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = set,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
        .pImageInfo = &image,
    };
    vkUpdateDescriptorSets(v->dev, 1, &write, 0, NULL);
    return set;
}

static VkDescriptorSet storage_descriptor_new(struct vk *v, VkImageView view,
        VkDescriptorPool *from) {
    VkDescriptorSet set = VK_NULL_HANDLE;
    for (size_t i = v->storage_pool_count; i > 0; i--) {
        VkDescriptorSetAllocateInfo alloc = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = v->storage_pools[i - 1],
            .descriptorSetCount = 1,
            .pSetLayouts = &v->storage_layout,
        };
        if (vkAllocateDescriptorSets(v->dev, &alloc, &set) == VK_SUCCESS) {
            *from = v->storage_pools[i - 1];
            break;
        }
        set = VK_NULL_HANDLE;
    }
    if (!set) {
        VkDescriptorPoolSize size = { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, POOL_SETS };
        VkDescriptorPoolCreateInfo info = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
            .maxSets = POOL_SETS,
            .poolSizeCount = 1,
            .pPoolSizes = &size,
        };
        VkDescriptorPool pool;
        VkDescriptorPool *grown = realloc(v->storage_pools,
            (v->storage_pool_count + 1) * sizeof(*grown));
        if (!grown) return VK_NULL_HANDLE;
        v->storage_pools = grown;
        if (vkCreateDescriptorPool(v->dev, &info, NULL, &pool) != VK_SUCCESS)
            return VK_NULL_HANDLE;
        v->storage_pools[v->storage_pool_count++] = pool;
        VkDescriptorSetAllocateInfo alloc = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = pool,
            .descriptorSetCount = 1,
            .pSetLayouts = &v->storage_layout,
        };
        if (vkAllocateDescriptorSets(v->dev, &alloc, &set) != VK_SUCCESS) return VK_NULL_HANDLE;
        *from = pool;
    }
    VkDescriptorImageInfo image = { .imageView = view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL };
    VkWriteDescriptorSet write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = set,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        .pImageInfo = &image,
    };
    vkUpdateDescriptorSets(v->dev, 1, &write, 0, NULL);
    return set;
}

static bool yuv_view(struct vk *v, struct vk_image *image) {
    struct yuv_path *y = &v->yuv[image->yuv];
    VkSamplerYcbcrConversionInfo conversion = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO,
        .conversion = y->conversion,
    };
    VkImageViewCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = &conversion,
        .image = image->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = image->format,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    if (vkCreateImageView(v->dev, &info, NULL, &image->view) != VK_SUCCESS) return false;
    image->sample = image->view;
    VkDescriptorSet set = VK_NULL_HANDLE;
    for (size_t i = y->pool_count; i > 0 && !set; i--) {
        VkDescriptorSetAllocateInfo alloc = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = y->pools[i - 1],
            .descriptorSetCount = 1,
            .pSetLayouts = &y->set_layout,
        };
        if (vkAllocateDescriptorSets(v->dev, &alloc, &set) == VK_SUCCESS) image->pool = y->pools[i - 1];
        else set = VK_NULL_HANDLE;
    }
    if (!set) {
        VkDescriptorPoolSize size = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, YUV_POOL_SETS * 3 };
        VkDescriptorPoolCreateInfo pool_info = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
            .maxSets = YUV_POOL_SETS,
            .poolSizeCount = 1,
            .pPoolSizes = &size,
        };
        VkDescriptorPool *grown = realloc(y->pools, (y->pool_count + 1) * sizeof(*grown));
        if (!grown) return false;
        y->pools = grown;
        VkDescriptorPool pool;
        if (vkCreateDescriptorPool(v->dev, &pool_info, NULL, &pool) != VK_SUCCESS) return false;
        y->pools[y->pool_count++] = pool;
        VkDescriptorSetAllocateInfo alloc = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = pool,
            .descriptorSetCount = 1,
            .pSetLayouts = &y->set_layout,
        };
        if (vkAllocateDescriptorSets(v->dev, &alloc, &set) != VK_SUCCESS) return false;
        image->pool = pool;
    }
    VkDescriptorImageInfo described = { .imageView = image->view,
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL };
    VkWriteDescriptorSet write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = set,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo = &described,
    };
    vkUpdateDescriptorSets(v->dev, 1, &write, 0, NULL);
    image->set = set;
    return true;
}

static bool image_view(struct vk *v, struct vk_image *image, bool opaque) {
    if (image->yuv >= 0) return yuv_view(v, image);
    VkImageViewCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = image->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = image->format,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    if (vkCreateImageView(v->dev, &info, NULL, &image->view) != VK_SUCCESS) return false;
    image->sample = image->view;
    if (opaque) {
        info.components.a = VK_COMPONENT_SWIZZLE_ONE;
        if (vkCreateImageView(v->dev, &info, NULL, &image->sample) != VK_SUCCESS) {
            image->sample = image->view;
            return false;
        }
    }
    if ((image->usage & VK_IMAGE_USAGE_STORAGE_BIT) && v->storage_layout) {
        image->storage_set = storage_descriptor_new(v, image->view, &image->storage_pool);
        if (!image->storage_set) return false;
    }
    if (!(image->usage & VK_IMAGE_USAGE_SAMPLED_BIT)) return true;
    image->set = descriptor_new(v, image->sample, &image->pool);
    return image->set != VK_NULL_HANDLE;
}

static void image_free(struct vk_image *image) {
    struct vk *v = image->v;
    if (image->fd >= 0) close(image->fd);
    if (v->bound == image) rendering_end(v);
    bury(v, (struct grave){ .image = image->image, .memory = image->memory, .view = image->view,
        .sample = image->sample, .set = image->set, .pool = image->pool,
        .storage_set = image->storage_set, .storage_pool = image->storage_pool });
    if (v->target == image) v->target = NULL;
    for (size_t i = 0; v->open && i < v->open->acquired_len; i++)
        if (v->open->acquired[i] == image) v->open->acquired[i] = NULL;
    free(image);
}

static struct vk_image *image_new(struct vk *v, VkFormat format, int width, int height,
        VkImageUsageFlags usage, bool opaque) {
    if (width < 1 || height < 1 || (uint32_t)width > v->max_extent ||
            (uint32_t)height > v->max_extent) {
        tomoe_log(LOG_ERROR, "tomoe: vulkan refuses a %dx%d image", width, height);
        return NULL;
    }
    struct batch *b = batch(v);
    struct vk_image *image = b ? calloc(1, sizeof(*image)) : NULL;
    if (!image) return NULL;
    image->v = v;
    image->format = format;
    image->usage = usage;
    image->width = width;
    image->height = height;
    image->fd = -1;
    image->yuv = -1;
    wl_list_init(&image->link);
    VkImageCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = { (uint32_t)width, (uint32_t)height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (vkCreateImage(v->dev, &info, NULL, &image->image) != VK_SUCCESS) {
        free(image);
        return NULL;
    }
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(v->dev, image->image, &req);
    int type = memory_type(v, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    if (type < 0) type = memory_type(v, req.memoryTypeBits, 0, 0);
    VkMemoryAllocateInfo alloc = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size,
        .memoryTypeIndex = (uint32_t)type,
    };
    if (type < 0 || vkAllocateMemory(v->dev, &alloc, NULL, &image->memory) != VK_SUCCESS ||
            vkBindImageMemory(v->dev, image->image, image->memory, 0) != VK_SUCCESS ||
            !image_view(v, image, opaque)) {
        tomoe_log(LOG_ERROR, "tomoe: vulkan cannot create a %dx%d image", width, height);
        image_free(image);
        return NULL;
    }
    v->images_made++;
    VkPipelineStageFlags2 dst_stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_COPY_BIT |
        VK_PIPELINE_STAGE_2_BLIT_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT;
    VkAccessFlags2 dst_access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT |
        VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
        VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
    if (usage & VK_IMAGE_USAGE_STORAGE_BIT) {
        dst_stage |= VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        dst_access |= VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    }
    if (usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) {
        image_sync(b->pre, image->image, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
            VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
            VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED);
        VkClearColorValue zero = {{ 0, 0, 0, 0 }};
        VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCmdClearColorImage(b->pre, image->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
        image_sync(b->pre, image->image, VK_PIPELINE_STAGE_2_CLEAR_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT, dst_stage, dst_access,
            VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
            VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED);
    } else {
        image_sync(b->pre, image->image, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
            dst_stage, dst_access, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
            VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED);
    }
    return image;
}

static void import_destroy(struct vk_image *image) {
    addon_finish(&image->addon);
    wl_list_remove(&image->link);
    image_free(image);
}

static void import_addon_destroy(struct addon *addon) {
    struct vk_image *image = wl_container_of(addon, image, addon);
    import_destroy(image);
}

static bool same_file(int a, int b) {
    struct stat sa, sb;
    return a == b || (fstat(a, &sa) == 0 && fstat(b, &sb) == 0 && sa.st_ino == sb.st_ino &&
        sa.st_dev == sb.st_dev);
}

static VkImageUsageFlags import_usage(struct vk *v, uint32_t format, uint64_t modifier) {
    if (format_set_has(&v->base.render_formats, format, modifier))
        return VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    return VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
}

static struct vk_image *import(struct vk *v, struct buffer *buffer) {
    struct addon *addon = addon_find(&buffer->addons, &v->base, import_addon_destroy);
    if (addon) {
        struct vk_image *image = wl_container_of(addon, image, addon);
        return image;
    }
    struct dmabuf_attributes d;
    if (!buffer_get_dmabuf(buffer, &d)) return NULL;
    const struct pixel *p = pixel_for(d.format);
    int yuv = yuv_for(d.format);
    if (yuv >= 0 && !v->yuv[yuv].conversion) yuv = -1;
    if ((!p && yuv < 0) || d.modifier == DRM_FORMAT_MOD_INVALID || d.n_planes < 1 ||
            d.n_planes > DMABUF_MAX_PLANES) {
        tomoe_log(LOG_ERROR, "tomoe: vulkan cannot import format 0x%08x modifier 0x%016llx",
            d.format, (unsigned long long)d.modifier);
        return NULL;
    }
    if (d.width < 1 || d.height < 1 || (uint32_t)d.width > v->max_extent ||
            (uint32_t)d.height > v->max_extent) {
        tomoe_log(LOG_ERROR, "tomoe: vulkan refuses a %dx%d dmabuf", d.width, d.height);
        return NULL;
    }
    for (int i = 1; i < d.n_planes; i++) {
        if (same_file(d.fd[0], d.fd[i])) continue;
        tomoe_log(LOG_ERROR, "tomoe: vulkan cannot import a disjoint dmabuf");
        return NULL;
    }
    struct vk_image *image = calloc(1, sizeof(*image));
    if (!image) return NULL;
    image->v = v;
    image->format = yuv >= 0 ? yuv_formats[yuv].vk : p->vk;
    image->yuv = yuv;
    image->drm_format = d.format;
    image->width = d.width;
    image->height = d.height;
    image->foreign = true;
    image->fd = -1;
    image->usage = yuv >= 0 ? VK_IMAGE_USAGE_SAMPLED_BIT : import_usage(v, d.format, d.modifier);
    image->target = image->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    image->owned = render_owns(buffer);
    VkSubresourceLayout layouts[DMABUF_MAX_PLANES] = {0};
    for (int i = 0; i < d.n_planes; i++) {
        layouts[i].offset = d.offset[i];
        layouts[i].rowPitch = d.stride[i];
    }
    VkImageDrmFormatModifierExplicitCreateInfoEXT modifier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
        .drmFormatModifier = d.modifier,
        .drmFormatModifierPlaneCount = (uint32_t)d.n_planes,
        .pPlaneLayouts = layouts,
    };
    VkExternalMemoryImageCreateInfo external = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .pNext = &modifier,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    VkImageCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &external,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = image->format,
        .extent = { (uint32_t)d.width, (uint32_t)d.height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
        .usage = image->usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkResult result = vkCreateImage(v->dev, &info, NULL, &image->image);
    if (result != VK_SUCCESS) {
        tomoe_log(LOG_ERROR, "tomoe: vulkan dmabuf image failed (%d): %dx%d format 0x%08x "
            "modifier 0x%016llx", (int)result, d.width, d.height, d.format,
            (unsigned long long)d.modifier);
        free(image);
        return NULL;
    }
    VkMemoryFdPropertiesKHR fd_props = { .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR };
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(v->dev, image->image, &req);
    int fd = -1, type = -1;
    if (v->memory_fd_properties(v->dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, d.fd[0],
            &fd_props) == VK_SUCCESS)
        type = memory_type(v, req.memoryTypeBits & fd_props.memoryTypeBits, 0, 0);
    if (type >= 0) fd = fcntl(d.fd[0], F_DUPFD_CLOEXEC, 0);
    VkMemoryDedicatedAllocateInfo dedicated = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .image = image->image,
    };
    VkImportMemoryFdInfoKHR imported = {
        .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
        .pNext = &dedicated,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
        .fd = fd,
    };
    VkMemoryAllocateInfo alloc = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &imported,
        .allocationSize = req.size,
        .memoryTypeIndex = (uint32_t)type,
    };
    if (fd < 0 || vkAllocateMemory(v->dev, &alloc, NULL, &image->memory) != VK_SUCCESS) {
        if (fd >= 0) close(fd);
        tomoe_log(LOG_ERROR, "tomoe: vulkan dmabuf memory import failed: %dx%d format 0x%08x",
            d.width, d.height, d.format);
        vkDestroyImage(v->dev, image->image, NULL);
        free(image);
        return NULL;
    }
    if (vkBindImageMemory(v->dev, image->image, image->memory, 0) != VK_SUCCESS ||
            !image_view(v, image, false)) {
        tomoe_log(LOG_ERROR, "tomoe: vulkan cannot bind a dmabuf image");
        bury_now(v, &(struct grave){ .image = image->image, .memory = image->memory,
            .view = image->view, .sample = image->sample, .set = image->set, .pool = image->pool });
        free(image);
        return NULL;
    }
    v->imports_made++;
    image->fd = fcntl(d.fd[0], F_DUPFD_CLOEXEC, 0);
    addon_init(&image->addon, &buffer->addons, &v->base, import_addon_destroy);
    wl_list_insert(&v->images, &image->link);
    return image;
}

static struct vk_texture *texture_of(struct texture *base) {
    struct vk_texture *t = wl_container_of(base, t, base);
    return t;
}

static void copy_barrier(struct vk *v, VkCommandBuffer cb) {
    memory_barrier(v, cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COPY_BIT |
            VK_PIPELINE_STAGE_2_CLEAR_BIT,
        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT |
            VK_ACCESS_2_TRANSFER_WRITE_BIT);
}

static bool upload_rects(struct vk_texture *t, const void *data, size_t stride,
        const struct rect *rects, int count) {
    struct vk *v = t->v;
    struct batch *b = batch(v);
    if (!b) return false;
    rendering_end(v);
    bool copied = false;
    for (int i = 0; i < count; i++) {
        int x = rects[i].x1 < 0 ? 0 : rects[i].x1, y = rects[i].y1 < 0 ? 0 : rects[i].y1;
        int x2 = rects[i].x2 > (int)t->base.width ? (int)t->base.width : rects[i].x2;
        int y2 = rects[i].y2 > (int)t->base.height ? (int)t->base.height : rects[i].y2;
        if (x2 <= x || y2 <= y) continue;
        size_t row = (size_t)(x2 - x) * 4, rows = (size_t)(y2 - y);
        VkBuffer buffer;
        VkDeviceSize offset;
        unsigned char *dst = stage_alloc(v, row * rows, &buffer, &offset);
        if (!dst) return false;
        const unsigned char *src = (const unsigned char *)data + (size_t)y * stride + (size_t)x * 4;
        if (stride == row) memcpy(dst, src, row * rows);
        else for (size_t j = 0; j < rows; j++) memcpy(dst + j * row, src + j * stride, row);
        VkBufferImageCopy copy = {
            .bufferOffset = offset,
            .bufferRowLength = (uint32_t)(x2 - x),
            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .imageOffset = { x, y, 0 },
            .imageExtent = { (uint32_t)(x2 - x), (uint32_t)rows, 1 },
        };
        vkCmdCopyBufferToImage(b->main, buffer, t->image->image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
        t->v->base.uploads++;
        t->v->base.upload_bytes += row * rows;
        copied = true;
    }
    if (copied) copy_barrier(v, b->main);
    return true;
}

static void staging_poll(struct vk *v, bool wait) {
    struct render *r = &v->base;
    if (!v->staging.fences) return;
    if (wait && v->open && v->staging.serial[0] >= v->open->serial) submit(v, false);
    retire(v, false);
    while (v->staging.fences > 0) {
        if (v->staging.serial[0] > v->done) {
            if (!wait) break;
            retire(v, true);
            if (v->staging.serial[0] > v->done) break;
        }
        wait = false;
        if (r->upload) r->upload->gpu_done = v->staging.limit[0];
        v->staging.fences--;
        memmove(v->staging.serial, v->staging.serial + 1, sizeof(uint64_t) * (size_t)v->staging.fences);
        memmove(v->staging.limit, v->staging.limit + 1, sizeof(uint64_t) * (size_t)v->staging.fences);
    }
}

static void staging_settle(struct vk *v) {
    submit(v, false);
    vkDeviceWaitIdle(v->dev);
    retire(v, true);
    v->staging.fences = 0;
    v->staging.unfenced = false;
    if (v->base.upload) v->base.upload->gpu_done = v->base.upload->alloc;
}

static void staging_free(struct vk *v) {
    staging_poll(v, true);
    if (v->staging.fences || v->staging.unfenced) staging_settle(v);
    if (!v->staging.chunk) return;
    chunk_free(v, v->staging.chunk);
    v->staging.chunk = NULL;
    if (v->base.upload) upload_set_staging(v->base.upload, NULL, 0);
}

static void staging_retire(struct vk *v) {
    if (!v->staging.chunk) return;
    staging_poll(v, false);
    struct chunk *old = v->staging.chunk;
    v->staging.chunk = NULL;
    bool busy = v->staging.fences > 0 || v->staging.unfenced;
    v->staging.fences = 0;
    v->staging.unfenced = false;
    if (v->base.upload) upload_set_staging(v->base.upload, NULL, 0);
    if (busy) bury(v, (struct grave){ .chunk = old });
    else chunk_free(v, old);
}

static bool staging_alloc(struct vk *v, uint64_t size) {
    v->staging.chunk = chunk_new(v, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 0);
    if (!v->staging.chunk) {
        tomoe_log(LOG_ERROR, "tomoe: cannot map a %llu MiB upload staging buffer",
            (unsigned long long)(size >> 20));
        return false;
    }
    upload_set_staging(v->base.upload, v->staging.chunk->map, size);
    return true;
}

static void staging_resize(struct vk *v) {
    struct upload *u = v->base.upload;
    if (!u || !v->staging.chunk || !upload_idle(u)) return;
    uint64_t want = upload_wanted(u);
    if (!want) return;
    staging_retire(v);
    u->stats.resizes++;
    staging_alloc(v, want);
}

static struct upload *staging(struct vk *v) {
    struct render *r = &v->base;
    if (!r->upload) return NULL;
    if (!v->staging.chunk && !v->staging.failed &&
            !staging_alloc(v, upload_wanted(r->upload))) v->staging.failed = true;
    else
        staging_resize(v);
    return v->staging.chunk ? r->upload : NULL;
}

static bool latch_jobs(struct vk *v, bool ready);

static void reclaim(void *data) {
    struct vk *v = data;
    if (v->base.upload && upload_pending(v->base.upload)) latch_jobs(v, true);
    if (v->staging.fences) {
        if (v->open && !v->target && v->staging.serial[0] >= v->open->serial) submit(v, false);
        staging_poll(v, false);
    }
}

static bool latch_jobs(struct vk *v, bool ready) {
    struct render *r = &v->base;
    struct upload *u = r->upload;
    if (!u || (!upload_pending(u) && (ready || (!v->staging.unfenced && !u->frame_bytes && !u->calm)))) return false;
    uint64_t start = timing_enabled ? timing_now() : 0;
    staging_poll(v, false);
    bool used = false;
    uint64_t seq = ++v->latches;
    struct upload_job *job;
    while ((job = upload_latch_next(u, ready))) {
        struct vk_texture *t = wl_container_of(job->texture, t, base);
        struct batch *b = v->staging.chunk ? batch(v) : NULL;
        if (!b) continue;
        rendering_end(v);
        VkDeviceSize at = (VkDeviceSize)(job->src - u->base);
        VkBufferImageCopy copies[UPLOAD_RECTS];
        uint32_t n = 0;
        for (int i = 0; i < job->count && i < UPLOAD_RECTS; i++) {
            const struct rect *rect = &job->rects[i];
            int width = rect->x2 - rect->x1, height = rect->y2 - rect->y1;
            if (width > 0 && height > 0)
                copies[n++] = (VkBufferImageCopy){
                    .bufferOffset = at,
                    .bufferRowLength = (uint32_t)width,
                    .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                    .imageOffset = { rect->x1, rect->y1, 0 },
                    .imageExtent = { (uint32_t)width, (uint32_t)height, 1 },
                };
            at += (VkDeviceSize)width * (VkDeviceSize)height * 4;
        }
        if (!n) continue;
        if (t->image->copied == seq)
            memory_barrier(v, b->main, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT);
        t->image->copied = seq;
        vkCmdCopyBufferToImage(b->main, v->staging.chunk->buffer, t->image->image,
            VK_IMAGE_LAYOUT_GENERAL, n, copies);
        r->gl_calls++;
        used = true;
    }
    if (used && v->open) copy_barrier(v, v->open->main);
    bool fenced = false;
    if (ready && used && !v->target && v->open) {
        uint64_t limit = upload_pending(u) ? u->jobs[u->latch % UPLOAD_JOBS].start : u->alloc;
        bool merged = v->staging.fences == UPLOAD_FENCES;
        int at = merged ? UPLOAD_FENCES - 1 : v->staging.fences++;
        if (at > 0 && v->staging.limit[at - 1] > limit) limit = v->staging.limit[at - 1];
        if (merged && v->staging.limit[at] > limit) limit = v->staging.limit[at];
        v->staging.serial[at] = v->open->serial;
        v->staging.limit[at] = limit;
        used = false;
        fenced = true;
    }
    if (ready) {
        v->staging.unfenced |= used;
        upload_latch_end(u, false);
        if (timing_enabled) timing_add(TIMING_UPLOAD_LATCH, timing_now() - start);
        return fenced;
    }
    used |= v->staging.unfenced;
    v->staging.unfenced = false;
    if (used) {
        if (v->staging.fences == UPLOAD_FENCES) staging_poll(v, false);
        struct batch *b = batch(v);
        uint64_t serial = b ? b->serial : v->serial;
        if (v->staging.fences == UPLOAD_FENCES) {
            v->staging.serial[UPLOAD_FENCES - 1] = serial;
            v->staging.limit[UPLOAD_FENCES - 1] = u->alloc;
        } else {
            v->staging.serial[v->staging.fences] = serial;
            v->staging.limit[v->staging.fences++] = u->alloc;
        }
    } else if (v->staging.fences) {
        v->staging.limit[v->staging.fences - 1] = u->alloc;
    } else {
        u->gpu_done = u->alloc;
    }
    upload_latch_end(u, true);
    if (timing_enabled) timing_add(TIMING_UPLOAD_LATCH, timing_now() - start);
    return used;
}

static void latch(struct vk *v) {
    latch_jobs(v, false);
}

void gfx_upload_latch(struct render *r) {
    struct vk *v = V(r);
    if (latch_jobs(v, true) && v->open && !v->target) submit(v, false);
    else idle_submit(v);
    staging_resize(v);
}

static bool stage(struct vk_texture *t, struct buffer *buffer, const struct region *damage) {
    struct vk *v = t->v;
    struct upload *u = staging(v);
    int count = 0;
    region_rectangles(damage, &count);
    if (!u || count <= 0) return false;
    size_t offset, stride;
    uint32_t format;
    struct wl_shm_view *view = buffer_shm_view(buffer, &offset, &format, &stride);
    if (view) {
        bool ok = format == t->format && stride % 4 == 0 && buffer->width == (int)t->base.width &&
            buffer->height == (int)t->base.height;
        if (ok && upload_full(u)) latch(v);
        if (ok && upload_stage(u, &t->base, buffer, view, offset, stride, damage, reclaim, v))
            return true;
        wl_shm_view_unref(view);
    }
    u->stats.sync++;
    latch(v);
    return false;
}

struct texture *texture_new(struct render *r, uint32_t format, uint32_t width, uint32_t height) {
    struct vk *v = V(r);
    const struct pixel *p = pixel_for(format);
    if (!p || !shm_format(format) || !width || !height) {
        tomoe_log(LOG_ERROR, "tomoe: unsupported pixel buffer format 0x%08x", format);
        return NULL;
    }
    struct vk_texture *t = calloc(1, sizeof(*t));
    if (!t) return NULL;
    t->image = image_new(v, p->vk, (int)width, (int)height, VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, false);
    if (!t->image) {
        free(t);
        return NULL;
    }
    t->base = (struct texture){ width, height, ++serials };
    t->v = v;
    t->format = format;
    t->alpha = p->alpha;
    t->bytes = (uint64_t)width * height * 4;
    r->textures++;
    r->texture_bytes += t->bytes;
    return &t->base;
}

bool texture_clear(struct texture *base) {
    struct vk_texture *t = texture_of(base);
    if (!t || t->buffer) return false;
    struct vk *v = t->v;
    struct batch *b = batch(v);
    if (!b) return false;
    rendering_end(v);
    VkClearColorValue zero = {{ 0, 0, 0, 0 }};
    VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdClearColorImage(b->main, t->image->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
    full_barrier(v, b->main);
    base->serial = ++serials;
    idle_submit(v);
    return true;
}

bool texture_upload(struct texture *base, const void *data, uint32_t format,
        uint32_t stride, const struct rect *rects, int count) {
    if (!base || !data || count <= 0 || stride % 4 != 0) return false;
    struct vk_texture *t = texture_of(base);
    if (t->buffer || format != t->format) return false;
    bool ok = upload_rects(t, data, stride, rects, count);
    if (ok) base->serial = ++serials;
    idle_submit(t->v);
    return ok;
}

struct texture *texture_from_pixels(struct render *r, uint32_t format, uint32_t stride,
        uint32_t width, uint32_t height, const void *data) {
    struct texture *texture = texture_new(r, format, width, height);
    if (!texture) return NULL;
    struct rect full = { 0, 0, (int32_t)width, (int32_t)height };
    if (!texture_upload(texture, data, format, stride, &full, 1)) {
        texture_destroy(texture);
        return NULL;
    }
    return texture;
}

struct texture *texture_from_buffer(struct render *r, struct buffer *buffer) {
    struct vk *v = V(r);
    struct dmabuf_attributes dmabuf;
    if (!buffer_get_dmabuf(buffer, &dmabuf)) {
        void *data;
        uint32_t format;
        size_t stride;
        size_t offset;
        struct wl_shm_view *view = r->upload ? buffer_shm_view(buffer, &offset, &format, &stride) : NULL;
        if (view) {
            wl_shm_view_unref(view);
            struct texture *texture = texture_new(r, format, (uint32_t)buffer->width,
                (uint32_t)buffer->height);
            if (!texture) return NULL;
            struct region full;
            region_init_rect(&full, 0, 0, (uint32_t)buffer->width, (uint32_t)buffer->height);
            bool staged = stage(texture_of(texture), buffer, &full);
            region_fini(&full);
            if (staged) return texture;
            texture_destroy(texture);
        }
        if (!buffer_begin_access(buffer, BUFFER_READ, &data, &format, &stride)) return NULL;
        struct texture *texture = texture_from_pixels(r, format, (uint32_t)stride,
            (uint32_t)buffer->width, (uint32_t)buffer->height, data);
        buffer_end_access(buffer);
        return texture;
    }
    struct vk_image *image = import(v, buffer);
    if (!image || !(image->usage & VK_IMAGE_USAGE_SAMPLED_BIT)) return NULL;
    struct vk_texture *t = calloc(1, sizeof(*t));
    if (!t) return NULL;
    t->base = (struct texture){ buffer->width, buffer->height, ++serials };
    t->v = v;
    t->image = image;
    t->format = dmabuf.format;
    t->alpha = format_alpha(dmabuf.format);
    t->external = image->yuv >= 0;
    t->buffer = buffer_lock(buffer);
    idle_submit(v);
    return &t->base;
}

bool texture_update(struct texture *base, struct buffer *buffer, const struct region *damage) {
    struct vk_texture *t = texture_of(base);
    if (t->buffer) return false;
    if (stage(t, buffer, damage)) {
        base->serial = ++serials;
        return true;
    }
    void *data;
    uint32_t format;
    size_t stride;
    if (!buffer_begin_access(buffer, BUFFER_READ, &data, &format, &stride)) return false;
    bool ok = format == t->format && stride % 4 == 0 && buffer->width == (int)base->width &&
        buffer->height == (int)base->height;
    if (ok) {
        int count = 0;
        const struct rect *rects = region_rectangles(damage, &count);
        ok = upload_rects(t, data, stride, rects, count);
        if (ok) base->serial = ++serials;
    }
    buffer_end_access(buffer);
    idle_submit(t->v);
    return ok;
}

bool texture_read_pixels(struct texture *base, const struct read_options *options) {
    struct vk_texture *t = texture_of(base);
    struct vk *v = t->v;
    latch(v);
    bool want_bgra = options->format == DRM_FORMAT_ARGB8888 ||
        options->format == DRM_FORMAT_XRGB8888;
    bool want_rgba = options->format == DRM_FORMAT_ABGR8888 ||
        options->format == DRM_FORMAT_XBGR8888;
    struct vk_image *image = t->image;
    if ((!want_bgra && !want_rgba) || !image || !(image->usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT))
        return false;
    bool have_bgra = image->format == VK_FORMAT_B8G8R8A8_UNORM;
    if (!have_bgra && image->format != VK_FORMAT_R8G8B8A8_UNORM) return false;
    struct box src = box_empty(&options->src_box) ?
        (struct box){ 0, 0, base->width, base->height } : options->src_box;
    if (src.x < 0 || src.y < 0 || src.width <= 0 || src.height <= 0 ||
            src.x + src.width > image->width || src.y + src.height > image->height) return false;
    VkDeviceSize bytes = (VkDeviceSize)src.width * (VkDeviceSize)src.height * 4;
    struct chunk *readback = chunk_new(v, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    if (!readback) return false;
    struct batch *b = batch(v);
    if (!b || !image_use(v, image)) {
        chunk_free(v, readback);
        return false;
    }
    rendering_end(v);
    VkBufferImageCopy copy = {
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageOffset = { src.x, src.y, 0 },
        .imageExtent = { (uint32_t)src.width, (uint32_t)src.height, 1 },
    };
    vkCmdCopyImageToBuffer(b->main, image->image, VK_IMAGE_LAYOUT_GENERAL, readback->buffer, 1, &copy);
    VkMemoryBarrier2 host = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
        .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT,
    };
    VkDependencyInfo dependency = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &host,
    };
    vkCmdPipelineBarrier2(b->main, &dependency);
    struct vk_image *target = v->target;
    finish(v);
    v->target = target;
    VkMappedMemoryRange range = {
        .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory = readback->memory,
        .size = VK_WHOLE_SIZE,
    };
    vkInvalidateMappedMemoryRanges(v->dev, 1, &range);
    bool swap = have_bgra != want_bgra;
    size_t row = (size_t)src.width * 4;
    for (int y = 0; y < src.height; y++) {
        const unsigned char *from = readback->map + (size_t)y * row;
        unsigned char *to = (unsigned char *)options->data + (size_t)y * options->stride;
        if (!swap) {
            memcpy(to, from, row);
            continue;
        }
        for (size_t x = 0; x < row; x += 4) {
            to[x] = from[x + 2];
            to[x + 1] = from[x + 1];
            to[x + 2] = from[x];
            to[x + 3] = from[x + 3];
        }
    }
    chunk_free(v, readback);
    return true;
}

void texture_destroy(struct texture *base) {
    if (!base) return;
    struct vk_texture *t = texture_of(base);
    struct vk *v = t->v;
    upload_cancel(v->base.upload, base);
    if (t->buffer) {
        buffer_unlock(t->buffer);
    } else {
        image_free(t->image);
        v->base.textures--;
        v->base.texture_bytes -= t->bytes;
    }
    free(t);
    idle_submit(v);
}

bool render_wait(struct render *r, struct timeline *timeline, uint64_t point) {
    struct vk *v = V(r);
    if (!v->sync_fd) return false;
    int fd = timeline_export_sync_file(timeline, point);
    if (fd < 0) return false;
    return wait_fd(v, fd);
}

static bool wait_fd(struct vk *v, int fd) {
    struct batch *b = batch(v);
    if (!b) {
        close(fd);
        return false;
    }
    if (b->wait_count == b->wait_made) {
        size_t cap = (size_t)b->wait_made + 1;
        VkSemaphore *grown = realloc(b->waits, cap * sizeof(*grown));
        if (grown) b->waits = grown;
        VkSemaphoreSubmitInfo *infos = grown ? realloc(b->wait_infos, cap * sizeof(*infos)) : NULL;
        if (infos) b->wait_infos = infos;
        if (!grown || !infos) {
            close(fd);
            return false;
        }
        VkSemaphoreCreateInfo info = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        if (vkCreateSemaphore(v->dev, &info, NULL, &b->waits[b->wait_made]) != VK_SUCCESS) {
            close(fd);
            return false;
        }
        b->wait_made++;
    }
    VkImportSemaphoreFdInfoKHR import_info = {
        .sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR,
        .semaphore = b->waits[b->wait_count],
        .flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT,
        .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
        .fd = fd,
    };
    if (v->import_semaphore(v->dev, &import_info) != VK_SUCCESS) {
        close(fd);
        return false;
    }
    b->wait_count++;
    return true;
}

static VkPipeline pipeline_make(struct vk *v, VkFormat format, int blend, VkShaderModule vert,
        VkShaderModule frag, VkPipelineLayout layout, bool instanced) {
    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vert, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = frag, .pName = "main" },
    };
    VkVertexInputBindingDescription binding = {
        .binding = 0,
        .stride = sizeof(struct scene_instance),
        .inputRate = VK_VERTEX_INPUT_RATE_INSTANCE,
    };
    VkVertexInputAttributeDescription attributes[11];
    for (uint32_t i = 0; i < 11; i++)
        attributes[i] = (VkVertexInputAttributeDescription){
            .location = i, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT,
            .offset = i * 16 };
    VkPipelineVertexInputStateCreateInfo input = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = instanced ? 1 : 0,
        .pVertexBindingDescriptions = &binding,
        .vertexAttributeDescriptionCount = instanced ? 11 : 0,
        .pVertexAttributeDescriptions = attributes,
    };
    VkPipelineInputAssemblyStateCreateInfo assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = instanced ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST :
            VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
    };
    VkPipelineViewportStateCreateInfo viewport = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1,
    };
    VkPipelineRasterizationStateCreateInfo raster = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .lineWidth = 1,
    };
    VkPipelineMultisampleStateCreateInfo multisample = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    VkPipelineColorBlendAttachmentState attachment = {
        .blendEnable = blend ? VK_TRUE : VK_FALSE,
        .srcColorBlendFactor = blend == 2 ? VK_BLEND_FACTOR_ZERO : VK_BLEND_FACTOR_ONE,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = blend == 2 ? VK_BLEND_FACTOR_ZERO : VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .alphaBlendOp = VK_BLEND_OP_ADD,
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
    VkPipelineColorBlendStateCreateInfo blending = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &attachment,
    };
    VkDynamicState states[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamic = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2,
        .pDynamicStates = states,
    };
    VkPipelineRenderingCreateInfo rendering = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &format,
    };
    VkGraphicsPipelineCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = &rendering,
        .stageCount = 2,
        .pStages = stages,
        .pVertexInputState = &input,
        .pInputAssemblyState = &assembly,
        .pViewportState = &viewport,
        .pRasterizationState = &raster,
        .pMultisampleState = &multisample,
        .pColorBlendState = &blending,
        .pDynamicState = &dynamic,
        .layout = layout,
    };
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(v->dev, v->pipeline_cache, 1, &info, NULL, &pipeline) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    return pipeline;
}

static struct target_pipelines *pipelines_find(struct vk *v, VkFormat format);

static struct target_pipelines *pipelines_for(struct vk *v, VkFormat format) {
    struct target_pipelines *t = pipelines_find(v, format);
    if (!t) tomoe_log(LOG_ERROR, "tomoe: vulkan has no scene pipeline for format %d", (int)format);
    return t;
}

static struct target_pipelines *pipelines_find(struct vk *v, VkFormat format) {
    for (int i = 0; i < TARGET_FORMATS; i++)
        if (v->targets[i].format == format && v->targets[i].blend[0]) return &v->targets[i];
    for (int i = 0; i < TARGET_FORMATS; i++) {
        struct target_pipelines *t = &v->targets[i];
        if (t->blend[0]) continue;
        t->format = format;
        for (int b = 0; b < BLENDS; b++) {
            t->blend[b] = pipeline_make(v, format, b, v->vert, v->frag, v->layout, true);
            if (t->blend[b]) continue;
            tomoe_log(LOG_ERROR, "tomoe: vulkan cannot build the scene pipeline");
            for (int k = 0; k < b; k++) vkDestroyPipeline(v->dev, t->blend[k], NULL);
            *t = (struct target_pipelines){0};
            return NULL;
        }
        if (v->lut_layout)
            t->lut = pipeline_make(v, format, 1, v->vert, v->lut_frag, v->lut_layout, true);
        if (v->glass_layout)
            t->glass = pipeline_make(v, format, 1, v->vert, v->glass_frag, v->glass_layout, true);
        return t;
    }
    return NULL;
}

static VkRect2D clamp_area(const struct vk_image *target, VkRect2D area) {
    int32_t x1 = area.offset.x < 0 ? 0 : area.offset.x;
    int32_t y1 = area.offset.y < 0 ? 0 : area.offset.y;
    int64_t x2 = (int64_t)area.offset.x + (int64_t)area.extent.width;
    int64_t y2 = (int64_t)area.offset.y + (int64_t)area.extent.height;
    if (x2 > target->width) x2 = target->width;
    if (y2 > target->height) y2 = target->height;
    if (x2 <= x1 || y2 <= y1) return (VkRect2D){ { 0, 0 }, { 0, 0 } };
    return (VkRect2D){ { x1, y1 }, { (uint32_t)(x2 - x1), (uint32_t)(y2 - y1) } };
}

static bool area_inside(VkRect2D inner, VkRect2D outer) {
    return inner.offset.x >= outer.offset.x && inner.offset.y >= outer.offset.y &&
        (int64_t)inner.offset.x + inner.extent.width <= (int64_t)outer.offset.x + outer.extent.width &&
        (int64_t)inner.offset.y + inner.extent.height <= (int64_t)outer.offset.y + outer.extent.height;
}

static VkRect2D pass_area(const struct pass *pass, const struct vk_image *target) {
    VkRect2D full = { { 0, 0 }, { (uint32_t)target->width, (uint32_t)target->height } };
    if (!pass || !pass->clip) return full;
    const struct rect *e = region_extents((struct region *)pass->clip);
    if (!e || e->x2 <= e->x1 || e->y2 <= e->y1) return full;
    return clamp_area(target, (VkRect2D){ { e->x1, e->y1 },
        { (uint32_t)(e->x2 - e->x1), (uint32_t)(e->y2 - e->y1) } });
}

static bool rendering_begin_area(struct vk *v, struct vk_image *target, VkRect2D area,
        VkAttachmentLoadOp load_op, const float clear[4]) {
    if (!target) return false;
    area = clamp_area(target, area);
    if (!area.extent.width || !area.extent.height) return false;
    if (v->rendering && v->bound == target) {
        if (area_inside(area, v->render_area)) return true;
        load_op = VK_ATTACHMENT_LOAD_OP_LOAD;
        clear = NULL;
    }
    rendering_end(v);
    struct batch *b = batch(v);
    if (!b || !image_use_write(v, target)) return false;
    VkRenderingAttachmentInfo color = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = target->view,
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        .loadOp = load_op,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
    };
    if (load_op == VK_ATTACHMENT_LOAD_OP_CLEAR && clear)
        memcpy(color.clearValue.color.float32, clear, sizeof(color.clearValue.color.float32));
    VkRenderingInfo info = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = area,
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &color,
    };
    vkCmdBeginRendering(b->main, &info);
    target->written = b->serial;
    VkViewport viewport = { 0, 0, (float)target->width, (float)target->height, 0, 1 };
    vkCmdSetViewport(b->main, 0, 1, &viewport);
    v->rendering = true;
    v->bound = target;
    v->render_area = area;
    v->base.gl_calls += 2;
    return true;
}

static bool rendering_begin_op(struct vk *v, struct vk_image *target, VkAttachmentLoadOp load_op,
        const float clear[4]) {
    if (!target) return false;
    VkRect2D full = { { 0, 0 }, { (uint32_t)target->width, (uint32_t)target->height } };
    return rendering_begin_area(v, target, full, load_op, clear);
}

static bool rendering_begin(struct vk *v, struct vk_image *target) {
    return rendering_begin_op(v, target, VK_ATTACHMENT_LOAD_OP_LOAD, NULL);
}

static struct vk_image *level_image(const struct gfx_level *level) {
    return level ? (struct vk_image *)(uintptr_t)level->token : NULL;
}

static void scene_emit(struct pass *pass, const struct scene_quad *quad,
        const struct rect *scissor, uint64_t pixels) {
    struct render *r = pass->r;
    struct scene *s = &V(r)->scene;
    if (s->count == s->cap && s->cap && s->owner == pass) {
        pass_flush(pass);
        s->owner = pass;
    }
    if (s->count == s->cap) {
        size_t cap = s->cap ? s->cap * 2 : 64;
        struct scene_item *grown = realloc(s->items, cap * sizeof(*grown));
        if (!grown) return;
        s->items = grown;
        s->cap = cap;
    }
    struct scene_item *item = &s->items[s->count];
    memset(item, 0, sizeof(*item));
    struct scene_instance *in = &item->in;
    memcpy(in->pos, quad->pos, sizeof(in->pos));
    memcpy(in->local, quad->local, sizeof(in->local));
    memcpy(in->tex, quad->tex, sizeof(in->tex));
    memcpy(in->color, quad->color, sizeof(in->color));
    memcpy(in->tint, quad->tint, sizeof(in->tint));
    in->params[0] = quad->alpha;
    in->params[1] = quad->opaque ? 1 : 0;
    in->params[2] = quad->radius;
    in->params[3] = quad->shader_clip ? 1 : 0;
    in->extra[0] = quad->width;
    in->extra[1] = quad->range;
    in->extra[2] = quad->power;
    bool sdf = quad->kind != SCENE_RECT && quad->kind != SCENE_TEXTURE;
    in->extra[3] = sdf ? (float)(quad->kind - SCENE_BORDER) : (float)quad->kind;
    in->shape[0] = quad->size[0];
    in->shape[1] = quad->size[1];
    in->shape[2] = sdf ? 1 : 0;
    if (scissor) {
        item->scissor[0] = scissor->x1;
        item->scissor[1] = scissor->y1;
        item->scissor[2] = scissor->x2;
        item->scissor[3] = scissor->y2;
    }
    item->image = quad->texture ? texture_of(quad->texture)->image : level_image(quad->level);
    item->filter = quad->filter == SCENE_FILTER_NEAREST ? 1 : 0;
    item->blend = quad->blend;
    s->count++;
    r->fragments += pixels;
}

void pass_scene(struct pass *pass, const struct scene_quad *quad) {
    if (quad->kind == SCENE_TEXTURE && !quad->texture && !level_image(quad->level)) return;
    const struct region *clip = quad->clip ? quad->clip : pass->clip;
    if (!V(pass->r)->scene.count) V(pass->r)->scene.owner = pass;
    struct region both;
    if (quad->clip && pass->clip) {
        region_init(&both);
        region_intersect(&both, quad->clip, pass->clip);
        clip = &both;
    }
    if (!clip) scene_emit(pass, quad, NULL, quad_pixels(pass, quad->pos, NULL));
    else {
        int count = 0;
        const struct rect *rects = region_rectangles((struct region *)clip, &count);
        for (int i = 0; i < count; i++) {
            uint64_t pixels = quad_pixels(pass, quad->pos, &rects[i]);
            if (pixels) scene_emit(pass, quad, &rects[i], pixels);
        }
    }
    if (clip == &both) region_fini(&both);
}

struct scene_run {
    size_t first, count;
    struct vk_image *image;
    int filter, blend;
    int32_t scissor[4];
};

static void run_draw(struct vk *v, VkCommandBuffer cb, struct target_pipelines *pipes,
        const struct scene_run *run, int *blend, VkDescriptorSet *set, int *filter,
        VkRect2D *last_scissor) {
    struct render *r = &v->base;
    if (*blend != run->blend) {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipes->blend[run->blend]);
        *blend = run->blend;
        r->gl_calls++;
    }
    VkDescriptorSet want = run->image ? run->image->set : v->dummy->set;
    if (!want) return;
    if (*set != want) {
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, v->layout, 0, 1, &want, 0, NULL);
        *set = want;
        r->gl_calls++;
    }
    if (*filter != run->filter) {
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, v->layout, 1, 1,
            &v->sampler_sets[run->filter], 0, NULL);
        *filter = run->filter;
        r->gl_calls++;
    }
    VkRect2D scissor = { { 0, 0 }, { (uint32_t)v->target->width, (uint32_t)v->target->height } };
    if (run->scissor[2] > run->scissor[0]) {
        int32_t x1 = run->scissor[0] < 0 ? 0 : run->scissor[0];
        int32_t y1 = run->scissor[1] < 0 ? 0 : run->scissor[1];
        int32_t x2 = run->scissor[2] > v->target->width ? v->target->width : run->scissor[2];
        int32_t y2 = run->scissor[3] > v->target->height ? v->target->height : run->scissor[3];
        if (x2 <= x1 || y2 <= y1) return;
        scissor = (VkRect2D){ { x1, y1 }, { (uint32_t)(x2 - x1), (uint32_t)(y2 - y1) } };
    }
    if (memcmp(last_scissor, &scissor, sizeof(scissor))) {
        vkCmdSetScissor(cb, 0, 1, &scissor);
        *last_scissor = scissor;
        r->gl_calls++;
    }
    vkCmdDraw(cb, 6, (uint32_t)run->count, 0, (uint32_t)run->first);
    r->gl_calls++;
    r->runs++;
    r->quads += run->count;
}

static bool item_covers_target(const struct scene_item *item, const struct vk_image *target) {
    if (item->blend != 0 || item->in.shape[2] >= 0.5f || item->in.params[3] >= 0.5f)
        return false;
    if (item->scissor[2] > item->scissor[0] &&
            (item->scissor[0] > 0 || item->scissor[1] > 0 ||
            item->scissor[2] < target->width || item->scissor[3] < target->height))
        return false;
    const float *p = item->in.pos;
    return p[0] <= -1.0f && p[1] <= -1.0f && p[2] >= 1.0f && p[3] <= -1.0f &&
        p[4] <= -1.0f && p[5] >= 1.0f && p[6] >= 1.0f && p[7] >= 1.0f &&
        p[0] == p[4] && p[2] == p[6] && p[1] == p[3] && p[5] == p[7];
}

void pass_flush(struct pass *pass) {
    struct render *r = pass->r;
    struct vk *v = V(r);
    struct scene *s = &v->scene;
    if (!s->count || s->owner != pass) return;
    s->owner = NULL;
    size_t count = s->count;
    s->count = 0;
    if (!v->target) return;
    struct target_pipelines *pipes = pipelines_for(v, v->target->format);
    if (!pipes || !v->dummy) return;
    VkAttachmentLoadOp load_op = VK_ATTACHMENT_LOAD_OP_LOAD;
    const float *clear = NULL;
    size_t start = 0;
    if (!(v->rendering && v->bound == v->target) && item_covers_target(&s->items[0], v->target)) {
        if (s->items[0].in.extra[3] < 0.5f) {
            if (s->items[0].in.params[0] == 1.0f && s->items[0].in.params[2] == 0.0f) {
                load_op = VK_ATTACHMENT_LOAD_OP_CLEAR;
                clear = s->items[0].in.color;
                start = 1;
            } else {
                load_op = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            }
        } else if (s->items[0].image && s->items[0].image->set) {
            load_op = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        }
    }
    const struct scene_item *items = s->items + start;
    size_t n = count - start;
    if (!n) {
        rendering_begin_op(v, v->target, load_op, clear);
        return;
    }
    for (size_t i = 0; i < n; i++)
        if (items[i].image && !image_use(v, items[i].image)) return;
    VkBuffer buffer;
    VkDeviceSize offset;
    struct scene_instance *out = stage_alloc(v, n * sizeof(*out), &buffer, &offset);
    if (!out) return;
    for (size_t i = 0; i < n; i++) out[i] = items[i].in;
    r->uploads++;
    r->upload_bytes += n * sizeof(*out);
    VkRect2D area = load_op == VK_ATTACHMENT_LOAD_OP_LOAD ? pass_area(pass, v->target) :
        (VkRect2D){ { 0, 0 }, { (uint32_t)v->target->width, (uint32_t)v->target->height } };
    if (!rendering_begin_area(v, v->target, area, load_op, clear)) return;
    VkCommandBuffer cb = v->open->main;
    vkCmdBindVertexBuffers(cb, 0, 1, &buffer, &offset);
    r->gl_calls++;
    int blend = -1, filter = -1;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkRect2D last_scissor = { { -1, -1 }, { 0, 0 } };
    struct scene_run run = {0};
    for (size_t i = 0; i < n; i++) {
        const struct scene_item *in = &items[i];
        bool joins = run.count && run.blend == in->blend &&
            (!in->image || !run.image || (run.image == in->image && run.filter == in->filter)) &&
            !memcmp(run.scissor, in->scissor, sizeof(run.scissor));
        if (joins) {
            if (!run.image && in->image) {
                run.image = in->image;
                run.filter = in->filter;
            }
            run.count++;
            continue;
        }
        if (run.count) run_draw(v, cb, pipes, &run, &blend, &set, &filter, &last_scissor);
        run = (struct scene_run){ .first = i, .count = 1, .image = in->image,
            .filter = in->filter, .blend = in->blend };
        memcpy(run.scissor, in->scissor, sizeof(run.scissor));
    }
    if (run.count) run_draw(v, cb, pipes, &run, &blend, &set, &filter, &last_scissor);
}

void pass_trail(struct pass *pass, const struct trail_cell *cells, size_t count, uint64_t pixels) {
    if (!count) return;
    struct render *r = pass->r;
    pass_flush(pass);
    for (size_t i = 0; i < count; i++) {
        const struct trail_cell *c = &cells[i];
        struct scene_quad quad = {
            .kind = SCENE_RECT, .alpha = 1, .blend = 1, .radius = c->radius,
            .shader_clip = c->rounded > 0.5f, .size = { c->extent[0], c->extent[1] },
            .pos = { c->clip[0], c->clip[1], c->clip[2], c->clip[1], c->clip[0], c->clip[3],
                c->clip[2], c->clip[3] },
            .local = { 0, 0, c->extent[0], 0, 0, c->extent[1], c->extent[0], c->extent[1] },
        };
        memcpy(quad.color, c->color, sizeof(quad.color));
        uint64_t before = r->fragments;
        pass_scene(pass, &quad);
        r->fragments = before;
    }
    pass_flush(pass);
    r->fragments += pixels;
}

void gfx_texture_draw(struct pass *pass, const struct texture_options *options,
        const float pos[8], const float texcoords[8], float alpha) {
    struct vk_texture *t = texture_of(options->texture);
    struct scene_quad quad = {
        .kind = SCENE_TEXTURE, .texture = options->texture, .alpha = alpha,
        .opaque = t->alpha ? 0 : 1, .clip = options->clip,
        .filter = options->filter_mode == FILTER_NEAREST ? SCENE_FILTER_NEAREST :
            SCENE_FILTER_LINEAR,
        .blend = (t->alpha || alpha < 1) && options->blend_mode == BLEND_PREMULTIPLIED };
    if (options->tint)
        for (int i = 0; i < 4; i++) quad.tint[i] = options->tint[i];
    memcpy(quad.pos, pos, sizeof(quad.pos));
    memcpy(quad.tex, texcoords, sizeof(quad.tex));
    if (!t->external && !options->lut) {
        pass_scene(pass, &quad);
        return;
    }
    pass_flush(pass);
    if (t->external) {
        struct ext_push push = { .alpha = alpha, .opaque = t->alpha ? 0 : 1 };
        memcpy(push.tint, quad.tint, sizeof(push.tint));
        external_quad(pass, t, pos, NULL, texcoords, options->clip ? options->clip : pass->clip,
            &push);
        return;
    }
    struct vk *v = V(pass->r);
    struct render *r = pass->r;
    struct vk_texture *lut = texture_of(options->lut);
    struct target_pipelines *pipes = v->target ? pipelines_for(v, v->target->format) : NULL;
    if (!pipes || !pipes->lut || !t->image->set || !lut->image->set ||
            !image_use(v, t->image) || !image_use(v, lut->image)) return;
    VkBuffer buffer;
    VkDeviceSize offset;
    struct scene_instance *in = stage_alloc(v, sizeof(*in), &buffer, &offset);
    if (!in || !rendering_begin_area(v, v->target, pass_area(pass, v->target), VK_ATTACHMENT_LOAD_OP_LOAD, NULL)) return;
    memset(in, 0, sizeof(*in));
    memcpy(in->pos, pos, sizeof(in->pos));
    memcpy(in->tex, texcoords, sizeof(in->tex));
    memcpy(in->tint, quad.tint, sizeof(in->tint));
    in->params[0] = alpha;
    in->params[1] = t->alpha ? 0 : 1;
    VkCommandBuffer cb = v->open->main;
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipes->lut);
    VkDescriptorSet sets[4] = { t->image->set, v->sampler_sets[quad.filter == SCENE_FILTER_NEAREST],
        lut->image->set, v->sampler_sets[1] };
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, v->lut_layout, 0, 4, sets, 0, NULL);
    vkCmdBindVertexBuffers(cb, 0, 1, &buffer, &offset);
    r->gl_calls += 3;
    const struct region *clip = options->clip ? options->clip : pass->clip;
    int count = 1;
    const struct rect *rects = clip ? region_rectangles((struct region *)clip, &count) : NULL;
    for (int i = 0; i < count; i++) {
        VkRect2D scissor = { { 0, 0 }, { (uint32_t)v->target->width, (uint32_t)v->target->height } };
        if (rects) {
            int32_t x1 = rects[i].x1 < 0 ? 0 : rects[i].x1, y1 = rects[i].y1 < 0 ? 0 : rects[i].y1;
            int32_t x2 = rects[i].x2 > v->target->width ? v->target->width : rects[i].x2;
            int32_t y2 = rects[i].y2 > v->target->height ? v->target->height : rects[i].y2;
            if (x2 <= x1 || y2 <= y1) continue;
            scissor = (VkRect2D){ { x1, y1 }, { (uint32_t)(x2 - x1), (uint32_t)(y2 - y1) } };
        }
        vkCmdSetScissor(cb, 0, 1, &scissor);
        vkCmdDraw(cb, 6, 1, 0, 0);
        r->gl_calls += 2;
        r->quads++;
    }
    r->runs++;
}

static bool has_extension(const VkExtensionProperties *list, uint32_t count, const char *name) {
    for (uint32_t i = 0; i < count; i++)
        if (!strcmp(list[i].extensionName, name)) return true;
    return false;
}

static const char *const device_extensions[] = {
    VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
    VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
    VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
    VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
    VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
};

static bool device_usable(VkPhysicalDevice phys, bool drm) {
    uint32_t count = 0;
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(phys, &props);
    if (props.apiVersion < VK_API_VERSION_1_3) return false;
    vkEnumerateDeviceExtensionProperties(phys, NULL, &count, NULL);
    VkExtensionProperties *list = calloc(count ? count : 1, sizeof(*list));
    if (!list) return false;
    vkEnumerateDeviceExtensionProperties(phys, NULL, &count, list);
    bool ok = !drm || has_extension(list, count, VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME);
    for (size_t i = 0; ok && i < sizeof(device_extensions) / sizeof(device_extensions[0]); i++)
        ok = has_extension(list, count, device_extensions[i]);
    free(list);
    return ok;
}

static bool device_software(VkPhysicalDevice phys) {
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(phys, &props);
    return props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU && device_usable(phys, false);
}

static bool device_matches(VkPhysicalDevice phys, dev_t node) {
    if (!device_usable(phys, true)) return false;
    VkPhysicalDeviceDrmPropertiesEXT drm = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT,
    };
    VkPhysicalDeviceProperties2 props2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
        .pNext = &drm,
    };
    vkGetPhysicalDeviceProperties2(phys, &props2);
    return (drm.hasRender && (dev_t)makedev(drm.renderMajor, drm.renderMinor) == node) ||
        (drm.hasPrimary && (dev_t)makedev(drm.primaryMajor, drm.primaryMinor) == node);
}

static void icd_append_dir(char **out, size_t *len, size_t *cap, const char *base,
        const char *const *prefixes, size_t prefix_count) {
    if (!base || !*base) return;
    char dirpath[PATH_MAX];
    int n = snprintf(dirpath, sizeof(dirpath), "%s/vulkan/icd.d", base);
    if (n <= 0 || (size_t)n >= sizeof(dirpath)) return;
    DIR *dir = opendir(dirpath);
    if (!dir) return;
    struct dirent *ent;
    while ((ent = readdir(dir))) {
        const char *name = ent->d_name;
        size_t namelen = strlen(name);
        if (namelen < 6 || strcmp(name + namelen - 5, ".json") != 0) continue;
        bool match = false;
        for (size_t i = 0; i < prefix_count; i++) {
            size_t plen = strlen(prefixes[i]);
            if (namelen >= plen && strncmp(name, prefixes[i], plen) == 0) {
                match = true;
                break;
            }
        }
        if (!match) continue;
        char full[PATH_MAX];
        int fn = snprintf(full, sizeof(full), "%s/%s", dirpath, name);
        if (fn <= 0 || (size_t)fn >= sizeof(full)) continue;
        if (*out && strstr(*out, full)) continue;
        size_t need = *len + (size_t)fn + (*len ? 2 : 1);
        if (need > *cap) {
            size_t ncap = *cap ? *cap * 2 : 512;
            while (ncap < need) ncap *= 2;
            char *grown = realloc(*out, ncap);
            if (!grown) break;
            *out = grown;
            *cap = ncap;
        }
        if (*len) (*out)[(*len)++] = ':';
        memcpy(*out + *len, full, (size_t)fn + 1);
        *len += (size_t)fn;
    }
    closedir(dir);
}

static char *find_icd_files(dev_t rdev, bool software) {
    const char *prefixes[4];
    size_t count = 0;
    if (software) {
        prefixes[count++] = "lvp_icd";
    } else {
        char uevent_path[128];
        snprintf(uevent_path, sizeof(uevent_path), "/sys/dev/char/%u:%u/device/uevent",
            major(rdev), minor(rdev));
        FILE *f = fopen(uevent_path, "r");
        char driver[64] = "";
        if (f) {
            char line[256];
            while (fgets(line, sizeof(line), f)) {
                if (strncmp(line, "DRIVER=", 7) == 0) {
                    char *nl = strchr(line + 7, '\n');
                    if (nl) *nl = '\0';
                    snprintf(driver, sizeof(driver), "%.63s", line + 7);
                    break;
                }
            }
            fclose(f);
        }
        if (!strcmp(driver, "nvidia")) {
            prefixes[count++] = "nvidia_icd";
        } else if (!strcmp(driver, "amdgpu") || !strcmp(driver, "radeon")) {
            prefixes[count++] = "radeon_icd";
        } else if (!strcmp(driver, "i915") || !strcmp(driver, "xe")) {
            prefixes[count++] = "intel_icd";
            prefixes[count++] = "intel_hasvk_icd";
        } else if (!strcmp(driver, "nouveau")) {
            prefixes[count++] = "nouveau_icd";
        } else if (!strcmp(driver, "virtio-pci") || !strcmp(driver, "virtio_gpu")) {
            prefixes[count++] = "virtio_icd";
        } else if (!strcmp(driver, "panfrost")) {
            prefixes[count++] = "panfrost_icd";
        } else if (!strcmp(driver, "msm")) {
            prefixes[count++] = "freedreno_icd";
        } else if (!strcmp(driver, "v3d")) {
            prefixes[count++] = "broadcom_icd";
        }
    }
    if (!count) return NULL;
    char *out = NULL;
    size_t len = 0, cap = 0;
    const char *xdg = getenv("XDG_DATA_DIRS");
    if (xdg && *xdg) {
        char *copy = strdup(xdg);
        if (copy) {
            for (char *tok = copy, *sep; tok; tok = sep) {
                sep = strchr(tok, ':');
                if (sep) *sep++ = '\0';
                icd_append_dir(&out, &len, &cap, tok, prefixes, count);
            }
            free(copy);
        }
    }
    if (!len) icd_append_dir(&out, &len, &cap, "/run/opengl-driver/share", prefixes, count);
    if (!len) icd_append_dir(&out, &len, &cap, "/etc", prefixes, count);
    if (!len) icd_append_dir(&out, &len, &cap, "/usr/share", prefixes, count);
    return out;
}

static bool instance_pick(struct vk *v, const VkInstanceCreateInfo *instance, dev_t rdev) {
    VkResult result = vkCreateInstance(instance, NULL, &v->instance);
    if (result != VK_SUCCESS) {
        v->instance = VK_NULL_HANDLE;
        return false;
    }
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(v->instance, &count, NULL);
    VkPhysicalDevice *devices = calloc(count ? count : 1, sizeof(*devices));
    if (!devices) return false;
    vkEnumeratePhysicalDevices(v->instance, &count, devices);
    for (uint32_t i = 0; i < count && !v->phys; i++)
        if (device_matches(devices[i], rdev)) v->phys = devices[i];
    for (uint32_t i = 0; i < count && !v->phys && tomoe_test_flag("gfx-software"); i++)
        if (device_software(devices[i])) v->phys = devices[i];
    free(devices);
    return v->phys != VK_NULL_HANDLE;
}

static bool device_init(struct vk *v) {
    struct stat st;
    if (fstat(v->base.fd, &st) != 0) return false;
    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "tomoe",
        .pEngineName = "tomoe",
        .apiVersion = VK_API_VERSION_1_3,
    };
    VkInstanceCreateInfo instance = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app,
    };
    bool filtered = false;
    if (!getenv("VK_DRIVER_FILES") && !getenv("VK_ICD_FILENAMES")) {
        char *icds = find_icd_files(st.st_rdev, tomoe_test_flag("gfx-software"));
        if (icds && *icds) {
            setenv("VK_DRIVER_FILES", icds, 1);
            filtered = true;
            instance_pick(v, &instance, st.st_rdev);
            unsetenv("VK_DRIVER_FILES");
        }
        free(icds);
    }
    if (!v->phys) {
        if (v->instance) {
            vkDestroyInstance(v->instance, NULL);
            v->instance = VK_NULL_HANDLE;
        }
        if (filtered || !instance_pick(v, &instance, st.st_rdev)) {
            if (filtered) instance_pick(v, &instance, st.st_rdev);
        }
    }
    if (!v->instance) {
        tomoe_log(LOG_ERROR, "tomoe: vulkan instance creation failed");
        return false;
    }
    if (!v->phys) {
        tomoe_log(LOG_ERROR, "tomoe: no Vulkan 1.3 device with dmabuf support matches %s",
            v->base.node ? v->base.node : "the render node");
        return false;
    }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(v->phys, &props);
    v->atom = props.limits.nonCoherentAtomSize;
    v->vendor_id = props.vendorID;
    v->device_id = props.deviceID;
    memcpy(v->cache_uuid, props.pipelineCacheUUID, VK_UUID_SIZE);
    v->ubo_align = props.limits.minUniformBufferOffsetAlignment;
    if (v->ubo_align < 16) v->ubo_align = 16;
    v->max_extent = props.limits.maxImageDimension2D;
    vkGetPhysicalDeviceMemoryProperties(v->phys, &v->memory);
    uint32_t families = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(v->phys, &families, NULL);
    VkQueueFamilyProperties *family = calloc(families ? families : 1, sizeof(*family));
    if (!family) return false;
    vkGetPhysicalDeviceQueueFamilyProperties(v->phys, &families, family);
    v->family = UINT32_MAX;
    uint32_t ts_bits = 0;
    for (uint32_t i = 0; i < families; i++) {
        if (!(family[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
        bool has_comp = (family[i].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0;
        if (v->family == UINT32_MAX || (!v->queue_compute && has_comp)) {
            v->family = i;
            v->queue_compute = has_comp;
            ts_bits = family[i].timestampValidBits;
            if (has_comp) break;
        }
    }
    free(family);
    if (v->family == UINT32_MAX) return false;
    float priority = 1;
    VkDeviceQueueCreateInfo queue = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = v->family,
        .queueCount = 1,
        .pQueuePriorities = &priority,
    };
    VkPhysicalDeviceVulkan11Features have11 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
    };
    VkPhysicalDeviceFeatures2 have = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = &have11,
    };
    vkGetPhysicalDeviceFeatures2(v->phys, &have);
    v->ycbcr = have11.samplerYcbcrConversion;
    VkPhysicalDeviceVulkan11Features features11 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
        .samplerYcbcrConversion = v->ycbcr,
    };
    VkPhysicalDeviceVulkan13Features features13 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
        .pNext = &features11,
        .synchronization2 = VK_TRUE,
        .dynamicRendering = VK_TRUE,
    };
    VkPhysicalDeviceFeatures supported_features = {0};
    vkGetPhysicalDeviceFeatures(v->phys, &supported_features);
    VkFormatProperties3 fp3 = { .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3 };
    VkFormatProperties2 fp2 = {
        .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
        .pNext = &fp3,
    };
    vkGetPhysicalDeviceFormatProperties2(v->phys, (VkFormat)LEVEL_FORMAT, &fp2);
    VkFormatFeatureFlags2 need_storage = VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT |
        VK_FORMAT_FEATURE_2_STORAGE_WRITE_WITHOUT_FORMAT_BIT;
    v->compute_blur = v->queue_compute && supported_features.shaderStorageImageWriteWithoutFormat &&
        (fp3.optimalTilingFeatures & need_storage) == need_storage;
    VkPhysicalDeviceFeatures enabled_features = {
        .shaderStorageImageWriteWithoutFormat = v->compute_blur ? VK_TRUE : VK_FALSE,
    };
    VkDeviceCreateInfo device = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &features13,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue,
        .enabledExtensionCount = sizeof(device_extensions) / sizeof(device_extensions[0]),
        .ppEnabledExtensionNames = device_extensions,
        .pEnabledFeatures = &enabled_features,
    };
    VkResult result = vkCreateDevice(v->phys, &device, NULL, &v->dev);
    if (result != VK_SUCCESS) {
        tomoe_log(LOG_ERROR, "tomoe: vulkan device creation failed (%d)", (int)result);
        v->dev = VK_NULL_HANDLE;
        return false;
    }
    vkGetDeviceQueue(v->dev, v->family, 0, &v->queue);
    v->memory_fd_properties = (PFN_vkGetMemoryFdPropertiesKHR)
        vkGetDeviceProcAddr(v->dev, "vkGetMemoryFdPropertiesKHR");
    v->memory_fd = (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(v->dev, "vkGetMemoryFdKHR");
    v->image_modifier = (PFN_vkGetImageDrmFormatModifierPropertiesEXT)
        vkGetDeviceProcAddr(v->dev, "vkGetImageDrmFormatModifierPropertiesEXT");
    v->semaphore_fd = (PFN_vkGetSemaphoreFdKHR)vkGetDeviceProcAddr(v->dev, "vkGetSemaphoreFdKHR");
    v->import_semaphore = (PFN_vkImportSemaphoreFdKHR)
        vkGetDeviceProcAddr(v->dev, "vkImportSemaphoreFdKHR");
    if (!v->memory_fd_properties || !v->memory_fd || !v->image_modifier) return false;
    VkPhysicalDeviceExternalSemaphoreInfo semaphore = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO,
        .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
    };
    VkExternalSemaphoreProperties semaphore_props = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES,
    };
    vkGetPhysicalDeviceExternalSemaphoreProperties(v->phys, &semaphore, &semaphore_props);
    VkExternalSemaphoreFeatureFlags both = VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT |
        VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT;
    v->sync_fd = v->semaphore_fd && v->import_semaphore &&
        (semaphore_props.externalSemaphoreFeatures & both) == both;
    VkCommandPoolCreateInfo pool = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = v->family,
    };
    if (vkCreateCommandPool(v->dev, &pool, NULL, &v->command_pool) != VK_SUCCESS) return false;
    const char *ts_env = getenv("TOMOE_GPU_TIMESTAMPS");
    if (ts_env && *ts_env && strcmp(ts_env, "0") != 0) {
        if (ts_bits > 0 && props.limits.timestampPeriod > 0) {
            v->timestamps = true;
            v->ts_bits = ts_bits;
            v->ts_period = (double)props.limits.timestampPeriod;
            v->base.gpu_timestamps = true;
            if (ts_env[0] == '/') {
                int fd = open(ts_env, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
                if (fd >= 0) {
                    v->ts_log = fdopen(fd, "a");
                    if (!v->ts_log) close(fd);
                    else {
                        setvbuf(v->ts_log, NULL, _IOLBF, 0);
                        fprintf(v->ts_log, "# vulkan %s period=%.6f bits=%u\n",
                            props.deviceName, v->ts_period, v->ts_bits);
                    }
                }
            }
            tomoe_log(LOG_INFO, "tomoe: vulkan GPU timestamps enabled (period %.3f ns, %u bits)",
                v->ts_period, v->ts_bits);
        } else {
            tomoe_log(LOG_WARNING, "tomoe: vulkan device does not support GPU timestamps");
        }
    }
    tomoe_log(LOG_INFO, "tomoe: vulkan renderer on %s (%s)", props.deviceName,
        v->sync_fd ? "sync_file fences" : "no fence export");
    return true;
}

static bool modifier_usable(struct vk *v, VkFormat format, uint64_t modifier,
        VkImageUsageFlags usage, VkExternalMemoryFeatureFlags need) {
    VkPhysicalDeviceImageDrmFormatModifierInfoEXT mod = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT,
        .drmFormatModifier = modifier,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkPhysicalDeviceExternalImageFormatInfo external = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
        .pNext = &mod,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    VkPhysicalDeviceImageFormatInfo2 info = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
        .pNext = &external,
        .format = format,
        .type = VK_IMAGE_TYPE_2D,
        .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
        .usage = usage,
    };
    VkExternalImageFormatProperties external_props = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES,
    };
    VkImageFormatProperties2 props = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
        .pNext = &external_props,
    };
    if (vkGetPhysicalDeviceImageFormatProperties2(v->phys, &info, &props) != VK_SUCCESS)
        return false;
    return (external_props.externalMemoryProperties.externalMemoryFeatures & need) == need;
}

static uint32_t modifier_planes(struct vk *v, VkFormat format, uint64_t modifier) {
    VkDrmFormatModifierPropertiesListEXT list = {
        .sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT,
    };
    VkFormatProperties2 props = {
        .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
        .pNext = &list,
    };
    vkGetPhysicalDeviceFormatProperties2(v->phys, format, &props);
    if (!list.drmFormatModifierCount) return 0;
    VkDrmFormatModifierPropertiesEXT *mods = calloc(list.drmFormatModifierCount, sizeof(*mods));
    if (!mods) return 0;
    list.pDrmFormatModifierProperties = mods;
    vkGetPhysicalDeviceFormatProperties2(v->phys, format, &props);
    uint32_t planes = 0;
    for (uint32_t i = 0; i < list.drmFormatModifierCount; i++) {
        if (mods[i].drmFormatModifier == modifier) {
            planes = mods[i].drmFormatModifierPlaneCount;
            break;
        }
    }
    free(mods);
    return planes;
}

static bool formats_init(struct vk *v) {
    struct render *r = &v->base;
    for (size_t i = 0; i < sizeof(pixels) / sizeof(pixels[0]); i++) {
        const struct pixel *p = &pixels[i];
        VkDrmFormatModifierPropertiesListEXT list = {
            .sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT,
        };
        VkFormatProperties2 props = {
            .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
            .pNext = &list,
        };
        vkGetPhysicalDeviceFormatProperties2(v->phys, p->vk, &props);
        if (!list.drmFormatModifierCount) continue;
        VkDrmFormatModifierPropertiesEXT *mods = calloc(list.drmFormatModifierCount, sizeof(*mods));
        if (!mods) return false;
        list.pDrmFormatModifierProperties = mods;
        vkGetPhysicalDeviceFormatProperties2(v->phys, p->vk, &props);
        for (uint32_t j = 0; j < list.drmFormatModifierCount; j++) {
            VkFormatFeatureFlags features = mods[j].drmFormatModifierTilingFeatures;
            uint64_t modifier = mods[j].drmFormatModifier;
            bool sample = features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
            bool render = (features & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) &&
                (features & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT) &&
                (features & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT) &&
                (features & VK_FORMAT_FEATURE_TRANSFER_DST_BIT) &&
                (features & VK_FORMAT_FEATURE_BLIT_SRC_BIT);
            if (sample && modifier_usable(v, p->vk, modifier,
                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                    VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT))
                format_set_add(&r->texture_formats, p->drm, modifier);
            if (sample && render && modifier_usable(v, p->vk, modifier,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT |
                    VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT))
                format_set_add(&r->render_formats, p->drm, modifier);
        }
        free(mods);
    }
    for (int i = 0; v->ycbcr && i < YUV_FORMATS; i++) {
        struct yuv_path *y = &v->yuv[i];
        VkDrmFormatModifierPropertiesListEXT list = {
            .sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT,
        };
        VkFormatProperties2 props = {
            .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
            .pNext = &list,
        };
        vkGetPhysicalDeviceFormatProperties2(v->phys, yuv_formats[i].vk, &props);
        if (!list.drmFormatModifierCount) continue;
        VkDrmFormatModifierPropertiesEXT *mods = calloc(list.drmFormatModifierCount, sizeof(*mods));
        if (!mods) return false;
        list.pDrmFormatModifierProperties = mods;
        vkGetPhysicalDeviceFormatProperties2(v->phys, yuv_formats[i].vk, &props);
        bool all_midpoint = true, all_linear = true;
        for (uint32_t j = 0; j < list.drmFormatModifierCount; j++) {
            VkFormatFeatureFlags features = mods[j].drmFormatModifierTilingFeatures;
            bool midpoint = features & VK_FORMAT_FEATURE_MIDPOINT_CHROMA_SAMPLES_BIT;
            bool cosited = features & VK_FORMAT_FEATURE_COSITED_CHROMA_SAMPLES_BIT;
            if (!(features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) || (!midpoint && !cosited) ||
                    !modifier_usable(v, yuv_formats[i].vk, mods[j].drmFormatModifier,
                    VK_IMAGE_USAGE_SAMPLED_BIT,
                    VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT)) continue;
            if (y->modifier_count < 32) y->modifiers[y->modifier_count++] = mods[j].drmFormatModifier;
            y->supported = true;
            all_midpoint = all_midpoint && midpoint;
            all_linear = all_linear &&
                (features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_YCBCR_CONVERSION_LINEAR_FILTER_BIT);
        }
        y->midpoint = all_midpoint;
        y->linear = all_linear;
        free(mods);
    }
    const uint32_t shm[] = { DRM_FORMAT_ARGB8888, DRM_FORMAT_XRGB8888, DRM_FORMAT_ABGR8888,
        DRM_FORMAT_XBGR8888 };
    for (size_t i = 0; i < sizeof(shm) / sizeof(shm[0]); i++)
        format_set_add(&r->shm_formats, shm[i], DRM_FORMAT_MOD_LINEAR);
    return r->render_formats.len > 0;
}

static VkShaderModule shader_module(struct vk *v, const uint32_t *code, size_t bytes) {
    VkShaderModuleCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = bytes,
        .pCode = code,
    };
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(v->dev, &info, NULL, &module) != VK_SUCCESS) return VK_NULL_HANDLE;
    return module;
}

enum { USER_SETS = 5, UBO_SETS = 256 };

static bool user_layouts_init(struct vk *v) {
    VkDescriptorSetLayoutBinding binding = {
        .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
    };
    VkDescriptorSetLayoutCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1,
        .pBindings = &binding,
    };
    if (vkCreateDescriptorSetLayout(v->dev, &info, NULL, &v->ubo_layout) != VK_SUCCESS) {
        v->ubo_layout = VK_NULL_HANDLE;
        return false;
    }
    VkDescriptorPoolSize size = { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, UBO_SETS };
    VkDescriptorPoolCreateInfo pool = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
        .maxSets = UBO_SETS,
        .poolSizeCount = 1,
        .pPoolSizes = &size,
    };
    if (vkCreateDescriptorPool(v->dev, &pool, NULL, &v->ubo_pool) != VK_SUCCESS) {
        v->ubo_pool = VK_NULL_HANDLE;
        return false;
    }
    VkDescriptorSetLayout sets[USER_SETS] = { v->ubo_layout, v->sampler_layout, v->image_layout,
        v->image_layout, v->image_layout };
    VkPipelineLayoutCreateInfo layout = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = USER_SETS,
        .pSetLayouts = sets,
    };
    return vkCreatePipelineLayout(v->dev, &layout, NULL, &v->user_layout) == VK_SUCCESS;
}

enum { PIPELINE_CACHE_LIMIT = 64 << 20 };

static char *pipeline_cache_file(void) {
    const char *cache = getenv("XDG_CACHE_HOME"), *home = getenv("HOME");
    char *base = NULL, *tomoe = NULL, *path = NULL;
    if (cache && *cache == '/') base = strdup(cache);
    else if (home && *home == '/' && asprintf(&base, "%s/.cache", home) < 0) base = NULL;
    if (!base) return NULL;
    if (asprintf(&tomoe, "%s/tomoe", base) >= 0 && (mkdir(base, 0700) == 0 || errno == EEXIST) &&
            (mkdir(tomoe, 0700) == 0 || errno == EEXIST) && asprintf(&path, "%s/vk-pipelines.bin", tomoe) < 0)
        path = NULL;
    free(tomoe);
    free(base);
    return path;
}

static bool pipeline_cache_matches(const struct vk *v, const uint8_t *data, size_t length) {
    if (length < 16 + VK_UUID_SIZE) return false;
    uint32_t header[4];
    memcpy(header, data, sizeof(header));
    return header[0] >= 16 + VK_UUID_SIZE && header[0] <= length &&
        header[1] == VK_PIPELINE_CACHE_HEADER_VERSION_ONE && header[2] == v->vendor_id &&
        header[3] == v->device_id && !memcmp(data + 16, v->cache_uuid, VK_UUID_SIZE);
}

static void pipeline_cache_open(struct vk *v) {
    v->pipeline_cache_path = pipeline_cache_file();
    uint8_t *data = NULL;
    size_t length = 0;
    int fd = v->pipeline_cache_path ? open(v->pipeline_cache_path, O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW) : -1;
    struct stat st;
    if (fd >= 0 && fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0 &&
            st.st_size <= PIPELINE_CACHE_LIMIT && (data = malloc((size_t)st.st_size))) {
        while (length < (size_t)st.st_size) {
            ssize_t n = read(fd, data + length, (size_t)st.st_size - length);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            length += (size_t)n;
        }
        if (length != (size_t)st.st_size || !pipeline_cache_matches(v, data, length)) length = 0;
    }
    if (fd >= 0) close(fd);
    VkPipelineCacheCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
        .initialDataSize = length,
        .pInitialData = length ? data : NULL,
    };
    if (vkCreatePipelineCache(v->dev, &info, NULL, &v->pipeline_cache) != VK_SUCCESS) {
        info.initialDataSize = 0;
        info.pInitialData = NULL;
        if (vkCreatePipelineCache(v->dev, &info, NULL, &v->pipeline_cache) != VK_SUCCESS)
            v->pipeline_cache = VK_NULL_HANDLE;
        length = 0;
    }
    v->pipeline_cache_bytes = length;
    free(data);
}

static void pipeline_cache_save(struct vk *v) {
    size_t length = 0;
    if (!v->pipeline_cache || !v->pipeline_cache_path ||
            vkGetPipelineCacheData(v->dev, v->pipeline_cache, &length, NULL) != VK_SUCCESS ||
            !length || length == v->pipeline_cache_bytes || length > PIPELINE_CACHE_LIMIT) return;
    uint8_t *data = malloc(length);
    char *temporary = NULL;
    if (data && vkGetPipelineCacheData(v->dev, v->pipeline_cache, &length, data) == VK_SUCCESS &&
            pipeline_cache_matches(v, data, length) &&
            asprintf(&temporary, "%s.XXXXXX", v->pipeline_cache_path) >= 0) {
        int fd = mkostemp(temporary, O_CLOEXEC);
        if (fd >= 0) {
            size_t done = 0;
            while (done < length) {
                ssize_t n = write(fd, data + done, length - done);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) break;
                done += (size_t)n;
            }
            if (close(fd) == 0 && done == length && rename(temporary, v->pipeline_cache_path) == 0)
                v->pipeline_cache_bytes = length;
            else
                unlink(temporary);
        }
    }
    free(temporary);
    free(data);
}

static bool layouts_init(struct vk *v) {
    VkDescriptorSetLayoutBinding image = {
        .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT,
    };
    VkDescriptorSetLayoutCreateInfo image_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1,
        .pBindings = &image,
    };
    VkDescriptorSetLayoutBinding sampler = {
        .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT,
    };
    VkDescriptorSetLayoutCreateInfo sampler_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1,
        .pBindings = &sampler,
    };
    if (vkCreateDescriptorSetLayout(v->dev, &image_info, NULL, &v->image_layout) != VK_SUCCESS ||
            vkCreateDescriptorSetLayout(v->dev, &sampler_info, NULL, &v->sampler_layout) !=
            VK_SUCCESS) return false;
    VkDescriptorSetLayout sets[2] = { v->image_layout, v->sampler_layout };
    VkPipelineLayoutCreateInfo layout = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 2,
        .pSetLayouts = sets,
    };
    if (vkCreatePipelineLayout(v->dev, &layout, NULL, &v->layout) != VK_SUCCESS) return false;
    VkDescriptorPoolSize size = { VK_DESCRIPTOR_TYPE_SAMPLER, 2 };
    VkDescriptorPoolCreateInfo pool = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 2,
        .poolSizeCount = 1,
        .pPoolSizes = &size,
    };
    if (vkCreateDescriptorPool(v->dev, &pool, NULL, &v->sampler_pool) != VK_SUCCESS) return false;
    VkDescriptorSetLayout layouts[2] = { v->sampler_layout, v->sampler_layout };
    VkDescriptorSetAllocateInfo alloc = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = v->sampler_pool,
        .descriptorSetCount = 2,
        .pSetLayouts = layouts,
    };
    if (vkAllocateDescriptorSets(v->dev, &alloc, v->sampler_sets) != VK_SUCCESS) return false;
    for (int i = 0; i < 2; i++) {
        VkFilter filter = i ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
        VkSamplerCreateInfo info = {
            .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter = filter,
            .minFilter = filter,
            .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
            .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .maxLod = 0.25f,
        };
        if (vkCreateSampler(v->dev, &info, NULL, &v->samplers[i]) != VK_SUCCESS) return false;
        VkDescriptorImageInfo image_sampler = { .sampler = v->samplers[i] };
        VkWriteDescriptorSet write = {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = v->sampler_sets[i],
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
            .pImageInfo = &image_sampler,
        };
        vkUpdateDescriptorSets(v->dev, 1, &write, 0, NULL);
    }
    v->vert = shader_module(v, scene_vert, sizeof(scene_vert));
    v->frag = shader_module(v, scene_frag, sizeof(scene_frag));
    v->blur_vert = shader_module(v, blur_vert, sizeof(blur_vert));
    v->down_frag = shader_module(v, down_frag, sizeof(down_frag));
    v->up_frag = shader_module(v, up_frag, sizeof(up_frag));
    VkPushConstantRange push = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(struct blur_push) };
    VkPipelineLayoutCreateInfo blur = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 2,
        .pSetLayouts = sets,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push,
    };
    if (!v->vert || !v->frag || !v->blur_vert || !v->down_frag || !v->up_frag ||
            vkCreatePipelineLayout(v->dev, &blur, NULL, &v->blur_layout) != VK_SUCCESS)
        return false;
    v->lut_frag = shader_module(v, lut_frag, sizeof(lut_frag));
    VkDescriptorSetLayout lut_sets[4] = { v->image_layout, v->sampler_layout, v->image_layout,
        v->sampler_layout };
    VkPipelineLayoutCreateInfo lut_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 4,
        .pSetLayouts = lut_sets,
    };
    if (!v->lut_frag || vkCreatePipelineLayout(v->dev, &lut_info, NULL, &v->lut_layout) != VK_SUCCESS)
        v->lut_layout = VK_NULL_HANDLE;
    v->glass_frag = shader_module(v, glass_frag, sizeof(glass_frag));
    VkPushConstantRange glass_push = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(struct glass_push) };
    VkPipelineLayoutCreateInfo glass_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 4,
        .pSetLayouts = lut_sets,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &glass_push,
    };
    if (!v->glass_frag ||
            vkCreatePipelineLayout(v->dev, &glass_info, NULL, &v->glass_layout) != VK_SUCCESS)
        v->glass_layout = VK_NULL_HANDLE;
    if (!user_layouts_init(v)) {
        tomoe_log(LOG_ERROR, "tomoe: vulkan cannot lay out user shaders; they will not draw");
        v->user_layout = VK_NULL_HANDLE;
    }
    v->down = pipeline_make(v, (VkFormat)LEVEL_FORMAT, 0, v->blur_vert, v->down_frag,
        v->blur_layout, false);
    v->up = pipeline_make(v, (VkFormat)LEVEL_FORMAT, 0, v->blur_vert, v->up_frag,
        v->blur_layout, false);
    if (!v->down || !v->up) tomoe_log(LOG_ERROR, "tomoe: vulkan blur pipelines failed");
    if (v->compute_blur) {
        VkDescriptorSetLayoutBinding storage = {
            .binding = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        };
        VkDescriptorSetLayoutCreateInfo storage_info = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = 1,
            .pBindings = &storage,
        };
        VkDescriptorSetLayout csets[3] = { v->image_layout, v->sampler_layout, VK_NULL_HANDLE };
        VkPushConstantRange cpush = { VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(struct blur_comp_push) };
        bool ok = vkCreateDescriptorSetLayout(v->dev, &storage_info, NULL, &v->storage_layout) == VK_SUCCESS;
        if (ok) {
            csets[2] = v->storage_layout;
            VkPipelineLayoutCreateInfo clayout = {
                .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                .setLayoutCount = 3,
                .pSetLayouts = csets,
                .pushConstantRangeCount = 1,
                .pPushConstantRanges = &cpush,
            };
            ok = vkCreatePipelineLayout(v->dev, &clayout, NULL, &v->blur_comp_layout) == VK_SUCCESS;
        }
        if (ok) {
            v->down_comp_mod = shader_module(v, down_comp, sizeof(down_comp));
            v->up_comp_mod = shader_module(v, up_comp, sizeof(up_comp));
            ok = v->down_comp_mod && v->up_comp_mod;
        }
        if (ok) {
            VkComputePipelineCreateInfo cp[2] = {
                {
                    .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                    .stage = {
                        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                        .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                        .module = v->down_comp_mod,
                        .pName = "main",
                    },
                    .layout = v->blur_comp_layout,
                },
                {
                    .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                    .stage = {
                        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                        .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                        .module = v->up_comp_mod,
                        .pName = "main",
                    },
                    .layout = v->blur_comp_layout,
                },
            };
            VkPipeline out[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
            if (vkCreateComputePipelines(v->dev, v->pipeline_cache, 2, cp, NULL, out) == VK_SUCCESS) {
                v->down_comp = out[0];
                v->up_comp = out[1];
            } else {
                ok = false;
            }
        }
        if (!ok) v->compute_blur = false;
    }
    return true;
}

static bool yuv_init(struct vk *v, int index) {
    struct yuv_path *y = &v->yuv[index];
    VkFilter filter = y->linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    VkChromaLocation location = y->midpoint ? VK_CHROMA_LOCATION_MIDPOINT :
        VK_CHROMA_LOCATION_COSITED_EVEN;
    VkSamplerYcbcrConversionCreateInfo conversion = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO,
        .format = yuv_formats[index].vk,
        .ycbcrModel = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709,
        .ycbcrRange = VK_SAMPLER_YCBCR_RANGE_ITU_NARROW,
        .xChromaOffset = location,
        .yChromaOffset = location,
        .chromaFilter = filter,
    };
    if (vkCreateSamplerYcbcrConversion(v->dev, &conversion, NULL, &y->conversion) != VK_SUCCESS) {
        y->conversion = VK_NULL_HANDLE;
        return false;
    }
    VkSamplerYcbcrConversionInfo info = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO,
        .conversion = y->conversion,
    };
    VkSamplerCreateInfo sampler = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .pNext = &info,
        .magFilter = filter,
        .minFilter = filter,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
    };
    VkDescriptorSetLayoutBinding binding = {
        .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        .pImmutableSamplers = &y->sampler,
    };
    VkDescriptorSetLayoutCreateInfo set = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1,
        .pBindings = &binding,
    };
    VkPushConstantRange push = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(struct ext_push) };
    VkPipelineLayoutCreateInfo layout = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &y->set_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push,
    };
    if (vkCreateSampler(v->dev, &sampler, NULL, &y->sampler) != VK_SUCCESS ||
            vkCreateDescriptorSetLayout(v->dev, &set, NULL, &y->set_layout) != VK_SUCCESS ||
            vkCreatePipelineLayout(v->dev, &layout, NULL, &y->layout) != VK_SUCCESS) {
        tomoe_log(LOG_ERROR, "tomoe: vulkan cannot set up YUV sampling for 0x%08x",
            yuv_formats[index].drm);
        return false;
    }
    return true;
}

static void yuv_finish(struct vk *v) {
    for (int i = 0; i < YUV_FORMATS; i++) {
        struct yuv_path *y = &v->yuv[i];
        for (int k = 0; k < TARGET_FORMATS; k++)
            if (y->pipelines[k]) vkDestroyPipeline(v->dev, y->pipelines[k], NULL);
        for (size_t k = 0; k < y->pool_count; k++) vkDestroyDescriptorPool(v->dev, y->pools[k], NULL);
        free(y->pools);
        if (y->layout) vkDestroyPipelineLayout(v->dev, y->layout, NULL);
        if (y->set_layout) vkDestroyDescriptorSetLayout(v->dev, y->set_layout, NULL);
        if (y->sampler) vkDestroySampler(v->dev, y->sampler, NULL);
        if (y->conversion) vkDestroySamplerYcbcrConversion(v->dev, y->conversion, NULL);
        *y = (struct yuv_path){0};
    }
}

static VkPipeline yuv_pipeline(struct vk *v, int index, VkFormat format) {
    struct yuv_path *y = &v->yuv[index];
    for (int i = 0; i < TARGET_FORMATS; i++)
        if (y->pipelines[i] && y->targets[i] == format) return y->pipelines[i];
    for (int i = 0; i < TARGET_FORMATS; i++) {
        if (y->pipelines[i]) continue;
        y->pipelines[i] = pipeline_make(v, format, 1, v->vert, v->ext_frag, y->layout, true);
        y->targets[i] = format;
        return y->pipelines[i];
    }
    return VK_NULL_HANDLE;
}

static void external_quad(struct pass *pass, struct vk_texture *t, const float pos[8],
        const float local[8], const float texcoords[8], const struct region *clip,
        const struct ext_push *push) {
    struct vk *v = V(pass->r);
    struct render *r = pass->r;
    pass_flush(pass);
    struct vk_image *image = t->image;
    if (!v->target || !image || image->yuv < 0 || !image->set) return;
    VkPipeline pipeline = yuv_pipeline(v, image->yuv, v->target->format);
    if (!pipeline || !image_use(v, image)) return;
    VkBuffer buffer;
    VkDeviceSize offset;
    struct scene_instance *in = stage_alloc(v, sizeof(*in), &buffer, &offset);
    if (!in || !rendering_begin_area(v, v->target, pass_area(pass, v->target), VK_ATTACHMENT_LOAD_OP_LOAD, NULL)) return;
    memset(in, 0, sizeof(*in));
    memcpy(in->pos, pos, sizeof(in->pos));
    if (local) memcpy(in->local, local, sizeof(in->local));
    memcpy(in->tex, texcoords, sizeof(in->tex));
    VkCommandBuffer cb = v->open->main;
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, v->yuv[image->yuv].layout, 0, 1,
        &image->set, 0, NULL);
    vkCmdPushConstants(cb, v->yuv[image->yuv].layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
        sizeof(*push), push);
    vkCmdBindVertexBuffers(cb, 0, 1, &buffer, &offset);
    r->gl_calls += 4;
    int count = 1;
    const struct rect *rects = clip ? region_rectangles((struct region *)clip, &count) : NULL;
    for (int i = 0; i < count; i++) {
        VkRect2D scissor = { { 0, 0 }, { (uint32_t)v->target->width, (uint32_t)v->target->height } };
        if (rects) {
            int32_t x1 = rects[i].x1 < 0 ? 0 : rects[i].x1, y1 = rects[i].y1 < 0 ? 0 : rects[i].y1;
            int32_t x2 = rects[i].x2 > v->target->width ? v->target->width : rects[i].x2;
            int32_t y2 = rects[i].y2 > v->target->height ? v->target->height : rects[i].y2;
            if (x2 <= x1 || y2 <= y1) continue;
            scissor = (VkRect2D){ { x1, y1 }, { (uint32_t)(x2 - x1), (uint32_t)(y2 - y1) } };
            r->fragments += quad_pixels(pass, pos, &rects[i]);
        } else {
            r->fragments += quad_pixels(pass, pos, NULL);
        }
        vkCmdSetScissor(cb, 0, 1, &scissor);
        vkCmdDraw(cb, 6, 1, 0, 0);
        r->gl_calls += 2;
        r->quads++;
    }
    r->runs++;
}

struct render *gfx_alloc(void) {
    struct vk *v = calloc(1, sizeof(*v));
    if (!v) return NULL;
    wl_list_init(&v->images);
    return &v->base;
}

bool gfx_open(struct render *r) {
    struct vk *v = V(r);
    current_vk = v;
    const char *lose = tomoe_test_flag_value("gfx-lose-device");
    if (lose) v->lose_at = strtoull(lose, NULL, 10);
    if (!device_init(v)) return false;
    if (!formats_init(v)) {
        tomoe_log(LOG_ERROR, "tomoe: vulkan reports no renderable dmabuf formats");
        return false;
    }
    r->read_bgra = true;
    return true;
}

bool gfx_ready(struct render *r) {
    struct vk *v = V(r);
    pipeline_cache_open(v);
    if (!layouts_init(v)) {
        tomoe_log(LOG_ERROR, "tomoe: vulkan pipeline setup failed");
        return false;
    }
    v->ext_frag = shader_module(v, ext_frag, sizeof(ext_frag));
    for (int i = 0; v->ext_frag && i < YUV_FORMATS; i++) {
        if (!v->yuv[i].supported || !yuv_init(v, i)) continue;
        for (int k = 0; k < v->yuv[i].modifier_count; k++)
            format_set_add(&r->texture_formats, yuv_formats[i].drm, v->yuv[i].modifiers[k]);
    }
    v->dummy = image_new(v, VK_FORMAT_R8G8B8A8_UNORM, 1, 1,
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, false);
    if (!v->dummy) return false;
    struct scene *s = &v->scene;
    s->items = malloc(SCENE_RESERVE * sizeof(*s->items));
    if (s->items) s->cap = SCENE_RESERVE;
    if (!pipelines_for(v, VK_FORMAT_B8G8R8A8_UNORM)) return false;
    pipeline_cache_save(v);
    finish(v);
    r->upload = upload_create();
    return true;
}

bool gfx_fences(struct render *r) {
    return V(r)->sync_fd;
}

void gfx_stop(struct render *r) {
    struct vk *v = V(r);
    if (v->dev) finish(v);
    if (r->upload) {
        upload_report(r->upload);
        upload_destroy(r->upload);
        r->upload = NULL;
    }
}

void gfx_drop_images(struct render *r) {
    struct vk *v = V(r);
    if (!v->dev) return;
    finish(v);
    vkDeviceWaitIdle(v->dev);
    struct vk_image *image, *next;
    wl_list_for_each_safe(image, next, &v->images, link) import_destroy(image);
    graves_sweep(v, true);
}


struct gfx_bo {
    VkImage image;
    VkDeviceMemory memory;
};

struct gfx_bo *gfx_bo_alloc(struct render *r, int width, int height,
        const struct format *format, struct dmabuf_attributes *dmabuf) {
    struct vk *v = V(r);
    if (!v || !v->dev || !format || !dmabuf || width < 1 || height < 1 ||
            (uint32_t)width > v->max_extent || (uint32_t)height > v->max_extent)
        return NULL;
    const struct pixel *p = pixel_for(format->format);
    if (!p) return NULL;
    VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    const struct format *fallback = format_set_get(&r->render_formats, format->format);
    size_t cap = format->len + (fallback ? fallback->len : 0) + 1;
    uint64_t *mods = calloc(cap, sizeof(*mods));
    if (!mods) return NULL;
    uint32_t mod_count = 0;
    for (size_t i = 0; i < format->len; i++) {
        uint64_t m = format->modifiers[i];
        if (m == DRM_FORMAT_MOD_INVALID) continue;
        bool dup = false;
        for (uint32_t k = 0; k < mod_count; k++)
            if (mods[k] == m) { dup = true; break; }
        if (dup) continue;
        if (!modifier_usable(v, p->vk, m, usage, VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT))
            continue;
        mods[mod_count++] = m;
    }
    if (!mod_count && format_has(format, DRM_FORMAT_MOD_INVALID)) {
        if (modifier_usable(v, p->vk, DRM_FORMAT_MOD_LINEAR, usage,
                VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT)) {
            mods[mod_count++] = DRM_FORMAT_MOD_LINEAR;
        } else if (fallback) {
            for (size_t i = 0; i < fallback->len; i++) {
                uint64_t m = fallback->modifiers[i];
                if (m == DRM_FORMAT_MOD_INVALID) continue;
                if (modifier_usable(v, p->vk, m, usage, VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT))
                    mods[mod_count++] = m;
            }
        }
    }
    if (!mod_count) {
        free(mods);
        return NULL;
    }
    VkImageDrmFormatModifierListCreateInfoEXT mod_list = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT,
        .drmFormatModifierCount = mod_count,
        .pDrmFormatModifiers = mods,
    };
    VkExternalMemoryImageCreateInfo external = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .pNext = &mod_list,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    VkImageCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &external,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = p->vk,
        .extent = { (uint32_t)width, (uint32_t)height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkImage image = VK_NULL_HANDLE;
    VkResult result = vkCreateImage(v->dev, &info, NULL, &image);
    free(mods);
    if (result != VK_SUCCESS) return NULL;
    VkImageDrmFormatModifierPropertiesEXT chosen = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT,
    };
    if (v->image_modifier(v->dev, image, &chosen) != VK_SUCCESS) {
        vkDestroyImage(v->dev, image, NULL);
        return NULL;
    }
    uint32_t planes = modifier_planes(v, p->vk, chosen.drmFormatModifier);
    if (planes < 1 || planes > DMABUF_MAX_PLANES) {
        vkDestroyImage(v->dev, image, NULL);
        return NULL;
    }
    VkMemoryDedicatedRequirements dreq = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS,
    };
    VkMemoryRequirements2 req = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
        .pNext = &dreq,
    };
    VkImageMemoryRequirementsInfo2 req_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,
        .image = image,
    };
    vkGetImageMemoryRequirements2(v->dev, &req_info, &req);
    int type = memory_type(v, req.memoryRequirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    if (type < 0) type = memory_type(v, req.memoryRequirements.memoryTypeBits, 0, 0);
    if (type < 0) {
        vkDestroyImage(v->dev, image, NULL);
        return NULL;
    }
    VkMemoryDedicatedAllocateInfo dedicated = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .image = image,
    };
    VkExportMemoryAllocateInfo export_info = {
        .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
        .pNext = &dedicated,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    VkMemoryAllocateInfo alloc = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &export_info,
        .allocationSize = req.memoryRequirements.size,
        .memoryTypeIndex = (uint32_t)type,
    };
    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (vkAllocateMemory(v->dev, &alloc, NULL, &memory) != VK_SUCCESS) {
        vkDestroyImage(v->dev, image, NULL);
        return NULL;
    }
    if (vkBindImageMemory(v->dev, image, memory, 0) != VK_SUCCESS) {
        vkFreeMemory(v->dev, memory, NULL);
        vkDestroyImage(v->dev, image, NULL);
        return NULL;
    }
    VkMemoryGetFdInfoKHR fd_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
        .memory = memory,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    int fd0 = -1;
    if (v->memory_fd(v->dev, &fd_info, &fd0) != VK_SUCCESS || fd0 < 0) {
        vkFreeMemory(v->dev, memory, NULL);
        vkDestroyImage(v->dev, image, NULL);
        return NULL;
    }
    fcntl(fd0, F_SETFD, FD_CLOEXEC);
    *dmabuf = (struct dmabuf_attributes){
        .width = width,
        .height = height,
        .format = format->format,
        .n_planes = (int)planes,
        .modifier = chosen.drmFormatModifier,
    };
    for (int i = 0; i < DMABUF_MAX_PLANES; i++) dmabuf->fd[i] = -1;
    for (uint32_t i = 0; i < planes; i++) {
        VkImageSubresource sub = {
            .aspectMask = VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT << i,
            .mipLevel = 0,
            .arrayLayer = 0,
        };
        VkSubresourceLayout layout = {0};
        vkGetImageSubresourceLayout(v->dev, image, &sub, &layout);
        if (layout.offset > UINT32_MAX || layout.rowPitch == 0 || layout.rowPitch > UINT32_MAX)
            goto fail_fd;
        dmabuf->offset[i] = (uint32_t)layout.offset;
        dmabuf->stride[i] = (uint32_t)layout.rowPitch;
        dmabuf->fd[i] = (i == 0) ? fd0 : fcntl(fd0, F_DUPFD_CLOEXEC, 0);
        if (dmabuf->fd[i] < 0) goto fail_fd;
    }
    struct gfx_bo *bo = calloc(1, sizeof(*bo));
    if (!bo) goto fail_fd;
    bo->image = image;
    bo->memory = memory;
    return bo;
fail_fd:
    if (dmabuf->fd[0] < 0 && fd0 >= 0) close(fd0);
    dmabuf_attributes_finish(dmabuf);
    vkFreeMemory(v->dev, memory, NULL);
    vkDestroyImage(v->dev, image, NULL);
    return NULL;
}

void gfx_bo_destroy(struct render *r, struct gfx_bo *bo) {
    if (!bo) return;
    struct vk *v = V(r);
    if (v && v->dev)
        bury(v, (struct grave){ .image = bo->image, .memory = bo->memory });
    free(bo);
}

void gfx_close(struct render *r) {
    struct vk *v = V(r);
    if (v->dev) {
        tomoe_log(LOG_INFO, "tomoe: vulkan staging chunks %llu (%llu MiB), images %llu, batches %zu, "
            "batch waits %llu, levels %llu, imports %llu", (unsigned long long)v->chunks_made,
            (unsigned long long)(v->chunk_bytes >> 20), (unsigned long long)v->images_made,
            v->batch_count, (unsigned long long)v->fence_waits, (unsigned long long)v->levels_made,
            (unsigned long long)v->imports_made);
        vkDeviceWaitIdle(v->dev);
        retire(v, true);
        if (v->ts_log) {
            fclose(v->ts_log);
            v->ts_log = NULL;
        }
        staging_free(v);
        level_pool_drain(v);
        if (v->dummy) image_free(v->dummy);
        v->dummy = NULL;
        graves_sweep(v, true);
        for (size_t i = 0; i < v->batch_count; i++) batch_finish(v, &v->batches[i]);
        while (v->spare) {
            struct chunk *c = v->spare;
            v->spare = c->next;
            chunk_free(v, c);
        }
        for (int i = 0; i < TARGET_FORMATS; i++)
            for (int b = 0; b < BLENDS; b++)
                if (v->targets[i].blend[b]) vkDestroyPipeline(v->dev, v->targets[i].blend[b], NULL);
        for (size_t i = 0; i < v->pool_count; i++) vkDestroyDescriptorPool(v->dev, v->pools[i], NULL);
        for (size_t i = 0; i < v->storage_pool_count; i++)
            vkDestroyDescriptorPool(v->dev, v->storage_pools[i], NULL);
        if (v->sampler_pool) vkDestroyDescriptorPool(v->dev, v->sampler_pool, NULL);
        for (int i = 0; i < 2; i++)
            if (v->samplers[i]) vkDestroySampler(v->dev, v->samplers[i], NULL);
        for (int i = 0; i < TARGET_FORMATS; i++) {
            if (v->targets[i].lut) vkDestroyPipeline(v->dev, v->targets[i].lut, NULL);
            if (v->targets[i].glass) vkDestroyPipeline(v->dev, v->targets[i].glass, NULL);
        }
        yuv_finish(v);
        if (v->ext_frag) vkDestroyShaderModule(v->dev, v->ext_frag, NULL);
        if (v->lut_layout) vkDestroyPipelineLayout(v->dev, v->lut_layout, NULL);
        if (v->lut_frag) vkDestroyShaderModule(v->dev, v->lut_frag, NULL);
        if (v->glass_layout) vkDestroyPipelineLayout(v->dev, v->glass_layout, NULL);
        for (struct gfx_prog *p = v->user_progs; p; p = p->next) {
            for (int i = 0; i < TARGET_FORMATS; i++)
                if (p->pipelines[i]) vkDestroyPipeline(v->dev, p->pipelines[i], NULL);
            if (p->frag) vkDestroyShaderModule(v->dev, p->frag, NULL);
            p->frag = VK_NULL_HANDLE;
            memset(p->pipelines, 0, sizeof(p->pipelines));
            p->v = NULL;
        }
        if (v->user_layout) vkDestroyPipelineLayout(v->dev, v->user_layout, NULL);
        if (v->ubo_pool) vkDestroyDescriptorPool(v->dev, v->ubo_pool, NULL);
        if (v->ubo_layout) vkDestroyDescriptorSetLayout(v->dev, v->ubo_layout, NULL);
        if (v->glass_frag) vkDestroyShaderModule(v->dev, v->glass_frag, NULL);
        if (v->down_comp) vkDestroyPipeline(v->dev, v->down_comp, NULL);
        if (v->up_comp) vkDestroyPipeline(v->dev, v->up_comp, NULL);
        if (v->blur_comp_layout) vkDestroyPipelineLayout(v->dev, v->blur_comp_layout, NULL);
        if (v->down_comp_mod) vkDestroyShaderModule(v->dev, v->down_comp_mod, NULL);
        if (v->up_comp_mod) vkDestroyShaderModule(v->dev, v->up_comp_mod, NULL);
        if (v->storage_layout) vkDestroyDescriptorSetLayout(v->dev, v->storage_layout, NULL);
        if (v->down) vkDestroyPipeline(v->dev, v->down, NULL);
        if (v->up) vkDestroyPipeline(v->dev, v->up, NULL);
        if (v->blur_layout) vkDestroyPipelineLayout(v->dev, v->blur_layout, NULL);
        if (v->blur_vert) vkDestroyShaderModule(v->dev, v->blur_vert, NULL);
        if (v->down_frag) vkDestroyShaderModule(v->dev, v->down_frag, NULL);
        if (v->up_frag) vkDestroyShaderModule(v->dev, v->up_frag, NULL);
        if (v->vert) vkDestroyShaderModule(v->dev, v->vert, NULL);
        if (v->frag) vkDestroyShaderModule(v->dev, v->frag, NULL);
        if (v->layout) vkDestroyPipelineLayout(v->dev, v->layout, NULL);
        if (v->image_layout) vkDestroyDescriptorSetLayout(v->dev, v->image_layout, NULL);
        if (v->sampler_layout) vkDestroyDescriptorSetLayout(v->dev, v->sampler_layout, NULL);
        if (v->command_pool) vkDestroyCommandPool(v->dev, v->command_pool, NULL);
        if (!v->lost) pipeline_cache_save(v);
        if (v->pipeline_cache) vkDestroyPipelineCache(v->dev, v->pipeline_cache, NULL);
        vkDestroyDevice(v->dev, NULL);
    }
    if (v->instance) vkDestroyInstance(v->instance, NULL);
    if (current_vk == v) current_vk = NULL;
    free(v->batches);
    free(v->pipeline_cache_path);
    free(v->pools);
    free(v->storage_pools);
    free(v->scene.items);
}

size_t gfx_image_count(struct render *r) {
    return (size_t)wl_list_length(&V(r)->images);
}

void gfx_current(struct render *r) {
}

void gfx_flush(struct render *r) {
    submit(V(r), false);
}

void gfx_upload_trim(struct render *r) {
    struct vk *v = V(r);
    if (v->open && !v->target) submit(v, false);
    level_pool_drain(v);
    if (v->staging.chunk) {
        staging_free(v);
        if (r->upload) r->upload->stats.trims++;
    }
    while (v->spare) {
        struct chunk *c = v->spare;
        v->spare = c->next;
        chunk_free(v, c);
    }
}

bool gfx_idle_trim(struct render *r) {
    struct vk *v = V(r);
    retire(v, false);
    graves_sweep(v, false);
    level_pool_drain(v);
    if (v->staging.chunk && !v->staging.fences &&
            (!r->upload || (upload_idle(r->upload) && !upload_pending(r->upload)))) {
        staging_free(v);
        if (r->upload) {
            r->upload->resume = UPLOAD_MIN_STAGING;
            r->upload->stats.trims++;
        }
    }
    while (v->spare) {
        struct chunk *c = v->spare;
        v->spare = c->next;
        chunk_free(v, c);
    }
    if (v->scene.count == 0 && v->scene.cap > SCENE_RESERVE) {
        struct scene_item *shrunk = realloc(v->scene.items, SCENE_RESERVE * sizeof(*shrunk));
        if (shrunk) {
            v->scene.items = shrunk;
            v->scene.cap = SCENE_RESERVE;
        }
    }
    for (size_t i = 0; i < v->batch_count; i++)
        if (v->batches[i].busy) return false;
    if (!v->open && !v->lost && v->command_pool != VK_NULL_HANDLE) {
        vkResetCommandPool(v->dev, v->command_pool, VK_COMMAND_POOL_RESET_RELEASE_RESOURCES_BIT);
        for (size_t i = 0; i < v->batch_count; i++) {
            free(v->batches[i].acquired);
            v->batches[i].acquired = NULL;
            v->batches[i].acquired_len = 0;
            v->batches[i].acquired_cap = 0;
        }
    }
    return v->graves == NULL;
}

void gfx_finish(struct render *r) {
    finish(V(r));
}

void gfx_retire(struct render *r) {
    struct vk *v = V(r);
    if (!v->dev) return;
    retire(v, false);
    if (v->ts_log) fflush(v->ts_log);
}

int gfx_fence_export(struct render *r) {
    struct vk *v = V(r);
    if (!v->sync_fd) return -1;
    return submit(v, true);
}

bool gfx_target_prepare(struct render *r, struct buffer *buffer) {
    struct vk_image *image = import(V(r), buffer);
    return image && image->target;
}

static bool target_bind(struct vk *v, struct buffer *buffer) {
    struct vk_image *image = import(v, buffer);
    if (!image || !image->target) return false;
    if (v->bound && v->bound != image) rendering_end(v);
    v->target = image;
    return true;
}

bool gfx_target_begin(struct render *r, struct buffer *buffer) {
    struct vk *v = V(r);
    if (!target_bind(v, buffer)) return false;
    v->in_target = true;
    if (v->open) v->open->is_frame = true;
    latch(v);
    return true;
}

void gfx_target_end(struct render *r) {
    struct vk *v = V(r);
    rendering_end(v);
    v->target = NULL;
    v->in_target = false;
}

void gfx_target_viewport(struct render *r, struct buffer *buffer) {
    target_bind(V(r), buffer);
}

void gfx_target_rebind(struct render *r, struct buffer *buffer) {
    target_bind(V(r), buffer);
}

void gfx_state_dirty(struct pass *pass) {
}

bool gfx_tex_alpha(const struct texture *texture) {
    return texture_of((struct texture *)texture)->alpha;
}

bool gfx_tex_external(const struct texture *texture) {
    return texture_of((struct texture *)texture)->external;
}

bool gfx_tex_info(const struct texture *texture, bool *external, bool *alpha) {
    if (!texture) return false;
    const struct vk_texture *t = texture_of((struct texture *)texture);
    if (!t->image) return false;
    if (external) *external = t->external;
    if (alpha) *alpha = t->alpha;
    return true;
}

bool gfx_external_ready(struct render *r) {
    struct vk *v = V(r);
    for (int i = 0; i < YUV_FORMATS; i++)
        if (v->yuv[i].conversion) return true;
    return false;
}

bool gfx_prog_reads(const struct gfx_prog *p, enum gfx_input input) {
    return p && !p->builtin && (p->reads & (1u << input));
}

static void level_pool_put(struct vk *v, struct vk_image *image) {
    if (v->pooled == LEVEL_POOL) {
        image_free(v->pool[0]);
        memmove(v->pool, v->pool + 1, sizeof(v->pool[0]) * (LEVEL_POOL - 1));
        v->pooled--;
    }
    v->pool[v->pooled++] = image;
}

static struct vk_image *level_pool_take(struct vk *v, int width, int height) {
    for (int i = v->pooled - 1; i >= 0; i--) {
        struct vk_image *image = v->pool[i];
        if (image->width != width || image->height != height) continue;
        memmove(v->pool + i, v->pool + i + 1, sizeof(v->pool[0]) * (size_t)(v->pooled - i - 1));
        v->pooled--;
        return image;
    }
    return NULL;
}

static void level_pool_drain(struct vk *v) {
    while (v->pooled > 0) image_free(v->pool[--v->pooled]);
}

bool gfx_level_ensure(struct gfx_level *level, int width, int height) {
    struct vk *v = current_vk;
    if (!v || width < 1 || height < 1) return false;
    struct vk_image *image = level_image(level);
    if (image && level->width == width && level->height == height) return true;
    if (image) level_pool_put(v, image);
    level->token = 0;
    image = level_pool_take(v, width, height);
    if (!image) {
        VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (v->compute_blur) usage |= VK_IMAGE_USAGE_STORAGE_BIT;
        image = image_new(v, (VkFormat)LEVEL_FORMAT, width, height, usage, true);
    }
    if (!image) {
        *level = (struct gfx_level){0};
        return false;
    }
    level->token = (uint64_t)(uintptr_t)image;
    v->levels_made++;
    level->width = width;
    level->height = height;
    return true;
}

void gfx_level_release(struct gfx_level *level) {
    struct vk_image *image = level_image(level);
    if (image && current_vk) level_pool_put(current_vk, image);
    *level = (struct gfx_level){0};
}

static bool clip_copy(int *sx, int *sy, int *dx, int *dy, int *w, int *h,
        const struct vk_image *from, const struct vk_image *to) {
    if (*sx < 0) { *dx -= *sx; *w += *sx; *sx = 0; }
    if (*sy < 0) { *dy -= *sy; *h += *sy; *sy = 0; }
    if (*dx < 0) { *sx -= *dx; *w += *dx; *dx = 0; }
    if (*dy < 0) { *sy -= *dy; *h += *dy; *dy = 0; }
    if (*sx + *w > from->width) *w = from->width - *sx;
    if (*sy + *h > from->height) *h = from->height - *sy;
    if (*dx + *w > to->width) *w = to->width - *dx;
    if (*dy + *h > to->height) *h = to->height - *dy;
    return *w > 0 && *h > 0;
}

enum { COPY_STACK = 32 };

static bool can_copy_image(const struct vk_image *from, const struct vk_image *to) {
    return from->format == to->format && from->yuv < 0 && to->yuv < 0 &&
        (from->usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) &&
        (to->usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT);
}

static VkImageCopy copy_region(int sx, int sy, int dx, int dy, int w, int h) {
    return (VkImageCopy){
        .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .srcOffset = { sx, sy, 0 },
        .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .dstOffset = { dx, dy, 0 },
        .extent = { (uint32_t)w, (uint32_t)h, 1 },
    };
}

static VkImageBlit blit_region(int sx0, int sy0, int sx1, int sy1,
        int dx0, int dy0, int dx1, int dy1) {
    return (VkImageBlit){
        .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .srcOffsets = { { sx0, sy0, 0 }, { sx1, sy1, 1 } },
        .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .dstOffsets = { { dx0, dy0, 0 }, { dx1, dy1, 1 } },
    };
}

static bool rects_overlap(const struct rect *rects, int count) {
    for (int i = 0; i < count; i++)
        for (int j = i + 1; j < count; j++)
            if (rects[i].x1 < rects[j].x2 && rects[j].x1 < rects[i].x2 &&
                    rects[i].y1 < rects[j].y2 && rects[j].y1 < rects[i].y2)
                return true;
    return false;
}

static void blit_barrier(struct vk *v, VkCommandBuffer cb) {
    memory_barrier(v, cb, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT,
        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT |
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_TRANSFER_READ_BIT |
            VK_ACCESS_2_TRANSFER_WRITE_BIT);
}

static VkCommandBuffer transfer_begin(struct vk *v) {
    struct batch *b = batch(v);
    if (!b) return VK_NULL_HANDLE;
    rendering_end(v);
    return b->main;
}

bool gfx_buffer_copy(struct pass *pass, struct buffer *src, const struct region *region) {
    struct vk *v = pass ? V(pass->r) : NULL;
    if (!v || !src || !pass->buffer || src == pass->buffer || !region) return false;
    struct vk_image *from = import(v, src), *to = import(v, pass->buffer);
    if (!from || !to || from == to || from->width != to->width || from->height != to->height ||
            from->format != to->format || from->drm_format != to->drm_format) return false;
    if (from->owned && !from->used) return false;
    if (!(from->usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) ||
            !(to->usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) return false;
    pass_flush(pass);
    if (!image_use(v, from) || !image_use_write(v, to)) return false;
    struct batch *b = batch(v);
    if (!b) return false;
    VkCommandBuffer cb = transfer_begin(v);
    if (!cb) return false;
    if (!from->foreign || from->written == b->serial)
        stage_barrier(cb, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    int count = 0;
    const struct rect *rects = region_rectangles((struct region *)region, &count);
    VkImageCopy copies[32];
    uint32_t queued = 0;
    bool copied = false;
    for (int i = 0; i < count; i++) {
        int x1 = rects[i].x1 < 0 ? 0 : rects[i].x1;
        int y1 = rects[i].y1 < 0 ? 0 : rects[i].y1;
        int x2 = rects[i].x2 > to->width ? to->width : rects[i].x2;
        int y2 = rects[i].y2 > to->height ? to->height : rects[i].y2;
        if (x2 <= x1 || y2 <= y1) continue;
        copies[queued++] = (VkImageCopy){
            .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .srcOffset = { x1, y1, 0 },
            .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .dstOffset = { x1, y1, 0 },
            .extent = { (uint32_t)(x2 - x1), (uint32_t)(y2 - y1), 1 },
        };
        if (queued == 32) {
            vkCmdCopyImage(cb, from->image, VK_IMAGE_LAYOUT_GENERAL,
                to->image, VK_IMAGE_LAYOUT_GENERAL, queued, copies);
            queued = 0;
            copied = true;
        }
    }
    if (queued) {
        vkCmdCopyImage(cb, from->image, VK_IMAGE_LAYOUT_GENERAL,
            to->image, VK_IMAGE_LAYOUT_GENERAL, queued, copies);
        copied = true;
    }
    if (copied) {
        to->written = b->serial;
        full_barrier(v, cb);
    }
    return true;
}

void gfx_level_blit(const struct gfx_level *from, const struct gfx_level *to,
        const struct box *samples, int count, int sx, int sy, int dx, int dy) {
    struct vk *v = current_vk;
    struct vk_image *a = level_image(from), *b = level_image(to);
    if (!v || !a || !b || count <= 0 || !can_copy_image(a, b)) return;
    if (a == b && sx == dx && sy == dy) return;
    VkCommandBuffer cb = transfer_begin(v);
    if (!cb) return;
    int ox = dx - sx, oy = dy - sy;
    struct rect clipped_stack[COPY_STACK];
    struct rect *clipped = count > COPY_STACK ? malloc((size_t)count * sizeof(*clipped)) :
        clipped_stack;
    int kept = 0;
    bool copied = false;
    if (clipped) {
        for (int i = 0; i < count; i++) {
            int x0 = samples[i].x - sx, y0 = samples[i].y - sy;
            int x1 = samples[i].x - dx, y1 = samples[i].y - dy;
            int w = samples[i].width, h = samples[i].height;
            if (clip_copy(&x0, &y0, &x1, &y1, &w, &h, a, b))
                clipped[kept++] = (struct rect){ x1, y1, x1 + w, y1 + h };
        }
    }
    struct region merged;
    bool use_merged = kept > 1 && rects_overlap(clipped, kept);
    const struct rect *rects = clipped;
    int rcount = kept;
    if (use_merged) {
        region_init(&merged);
        for (int i = 0; i < kept; i++)
            region_union_rect(&merged, &merged, clipped[i].x1, clipped[i].y1,
                (uint32_t)(clipped[i].x2 - clipped[i].x1),
                (uint32_t)(clipped[i].y2 - clipped[i].y1));
        rects = region_rectangles(&merged, &rcount);
    }
    if (rcount > 0) {
        VkImageCopy stack[COPY_STACK];
        VkImageCopy *regions = rcount > COPY_STACK ? malloc((size_t)rcount * sizeof(*regions)) :
            stack;
        if (!regions) regions = stack;
        uint32_t n = 0;
        for (int i = 0; i < rcount; i++) {
            if (regions == stack && n == COPY_STACK) {
                vkCmdCopyImage(cb, a->image, VK_IMAGE_LAYOUT_GENERAL, b->image,
                    VK_IMAGE_LAYOUT_GENERAL, n, regions);
                n = 0;
                copied = true;
            }
            int x1 = rects[i].x1, y1 = rects[i].y1;
            int w = rects[i].x2 - x1, h = rects[i].y2 - y1;
            regions[n++] = copy_region(x1 + ox, y1 + oy, x1, y1, w, h);
        }
        if (n) {
            vkCmdCopyImage(cb, a->image, VK_IMAGE_LAYOUT_GENERAL, b->image,
                VK_IMAGE_LAYOUT_GENERAL, n, regions);
            copied = true;
        }
        if (regions != stack) free(regions);
    }
    if (use_merged) region_fini(&merged);
    if (clipped != clipped_stack) free(clipped);
    if (copied) blit_barrier(v, cb);
}

bool gfx_level_spill(const struct gfx_level *from, struct gfx_level *to, int width, int height,
        const struct box *samples, int count, int dx, int dy) {
    bool ok = gfx_level_ensure(to, width, height);
    if (ok) gfx_level_blit(from, to, samples, count, 0, 0, dx, dy);
    else gfx_level_release(to);
    return ok;
}

void gfx_blur_copy_in(struct render *r, struct buffer *buffer, struct gfx_level *level,
        struct box grown, const struct rect *copy, int copies, int shrink) {
    struct vk *v = V(r);
    struct vk_image *source = import(v, buffer), *dest = level_image(level);
    if (!source || !dest || copies <= 0 || !image_use(v, source)) return;
    VkCommandBuffer cb = transfer_begin(v);
    if (!cb) return;
    bool copied = false;
    if (!shrink && can_copy_image(source, dest)) {
        VkImageCopy stack[COPY_STACK];
        VkImageCopy *regions = copies > COPY_STACK ? malloc((size_t)copies * sizeof(*regions)) :
            stack;
        if (!regions) regions = stack;
        uint32_t n = 0;
        for (int i = 0; i < copies; i++) {
            int dx0 = copy[i].x1, dy0 = copy[i].y1, dx1 = copy[i].x2, dy1 = copy[i].y2;
            int sx0 = grown.x + dx0, sy0 = grown.y + dy0;
            int w = dx1 - dx0, h = dy1 - dy0;
            if (!clip_copy(&sx0, &sy0, &dx0, &dy0, &w, &h, source, dest)) continue;
            if (regions == stack && n == COPY_STACK) {
                vkCmdCopyImage(cb, source->image, VK_IMAGE_LAYOUT_GENERAL, dest->image,
                    VK_IMAGE_LAYOUT_GENERAL, n, regions);
                n = 0;
                copied = true;
            }
            regions[n++] = copy_region(sx0, sy0, dx0, dy0, w, h);
        }
        if (n) {
            vkCmdCopyImage(cb, source->image, VK_IMAGE_LAYOUT_GENERAL, dest->image,
                VK_IMAGE_LAYOUT_GENERAL, n, regions);
            copied = true;
        }
        if (regions != stack) free(regions);
    } else {
        VkImageBlit stack[COPY_STACK];
        VkImageBlit *regions = copies > COPY_STACK ? malloc((size_t)copies * sizeof(*regions)) :
            stack;
        if (!regions) regions = stack;
        uint32_t n = 0;
        VkFilter filter = shrink ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        for (int i = 0; i < copies; i++) {
            int dx0 = copy[i].x1, dy0 = copy[i].y1, dx1 = copy[i].x2, dy1 = copy[i].y2;
            int sx0 = grown.x + (dx0 << shrink), sy0 = grown.y + (dy0 << shrink);
            int sx1 = grown.x + (dx1 << shrink), sy1 = grown.y + (dy1 << shrink);
            if (shrink) {
                if (sx0 < 0 || sy0 < 0 || sx1 > source->width || sy1 > source->height ||
                        dx0 < 0 || dy0 < 0 || dx1 > dest->width || dy1 > dest->height ||
                        dx1 <= dx0 || dy1 <= dy0) continue;
            } else {
                int w = dx1 - dx0, h = dy1 - dy0;
                if (!clip_copy(&sx0, &sy0, &dx0, &dy0, &w, &h, source, dest)) continue;
                sx1 = sx0 + w;
                sy1 = sy0 + h;
                dx1 = dx0 + w;
                dy1 = dy0 + h;
            }
            if (regions == stack && n == COPY_STACK) {
                vkCmdBlitImage(cb, source->image, VK_IMAGE_LAYOUT_GENERAL, dest->image,
                    VK_IMAGE_LAYOUT_GENERAL, n, regions, filter);
                n = 0;
                copied = true;
            }
            regions[n++] = blit_region(sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1);
        }
        if (n) {
            vkCmdBlitImage(cb, source->image, VK_IMAGE_LAYOUT_GENERAL, dest->image,
                VK_IMAGE_LAYOUT_GENERAL, n, regions, filter);
            copied = true;
        }
        if (regions != stack) free(regions);
    }
    if (copied) blit_barrier(v, cb);
}

static void sample_pass(struct vk *v, VkPipeline pipeline, struct vk_image *from,
        struct vk_image *to, double offset, const struct region *where, float grain,
        float vibrancy, VkPipelineStageFlags2 src_stage, VkPipelineStageFlags2 dst_stage,
        VkAccessFlags2 dst_access) {
    int count = 0;
    const struct rect *rects = region_rectangles(where, &count);
    if (!count || !from->set) return;
    int32_t min_x = to->width, min_y = to->height, max_x = 0, max_y = 0;
    int valid = 0;
    for (int i = 0; i < count; i++) {
        int32_t x1 = rects[i].x1 < 0 ? 0 : rects[i].x1;
        int32_t y1 = rects[i].y1 < 0 ? 0 : rects[i].y1;
        int32_t x2 = rects[i].x2 > to->width ? to->width : rects[i].x2;
        int32_t y2 = rects[i].y2 > to->height ? to->height : rects[i].y2;
        if (x2 <= x1 || y2 <= y1) continue;
        if (x1 < min_x) min_x = x1;
        if (y1 < min_y) min_y = y1;
        if (x2 > max_x) max_x = x2;
        if (y2 > max_y) max_y = y2;
        valid++;
    }
    if (!valid) return;
    VkRect2D area = { { min_x, min_y }, { (uint32_t)(max_x - min_x), (uint32_t)(max_y - min_y) } };
    VkAttachmentLoadOp load_op = valid == 1 ? VK_ATTACHMENT_LOAD_OP_DONT_CARE :
        VK_ATTACHMENT_LOAD_OP_LOAD;
    if (!rendering_begin_area(v, to, area, load_op, NULL)) return;
    VkCommandBuffer cb = v->open->main;
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    VkDescriptorSet sets[2] = { from->set, v->sampler_sets[0] };
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, v->blur_layout, 0, 2, sets, 0, NULL);
    struct blur_push push = { { 0.5f / (float)to->width, 0.5f / (float)to->height },
        (float)offset, grain, vibrancy };
    vkCmdPushConstants(cb, v->blur_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    v->base.gl_calls += 3;
    for (int i = 0; i < count; i++) {
        int32_t x1 = rects[i].x1 < 0 ? 0 : rects[i].x1, y1 = rects[i].y1 < 0 ? 0 : rects[i].y1;
        int32_t x2 = rects[i].x2 > to->width ? to->width : rects[i].x2;
        int32_t y2 = rects[i].y2 > to->height ? to->height : rects[i].y2;
        if (x2 <= x1 || y2 <= y1) continue;
        VkRect2D scissor = { { x1, y1 }, { (uint32_t)(x2 - x1), (uint32_t)(y2 - y1) } };
        vkCmdSetScissor(cb, 0, 1, &scissor);
        vkCmdDraw(cb, 4, 1, 0, 0);
        v->base.gl_calls += 2;
    }
    rendering_close(v);
    memory_barrier(v, cb, src_stage, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, dst_stage,
        dst_access);
}

static bool blur_compute_dispatch(struct vk *v, VkCommandBuffer cb, struct vk_image *from,
        struct vk_image *to, const struct box *sub, double offset, const struct region *region,
        float grain, float vibrancy) {
    int count = 0;
    const struct rect *rects = region_rectangles(region, &count);
    if (count == 0) return false;
    if (!from->set || !to->storage_set || !image_use(v, from) || !image_use(v, to)) return false;
    VkDescriptorSet sets[3] = { from->set, v->sampler_sets[0], to->storage_set };
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, v->blur_comp_layout, 0, 3,
        sets, 0, NULL);
    struct blur_comp_push push = {
        .offset = (float)offset,
        .grain = grain,
        .vibrancy = vibrancy,
    };
    if (sub) {
        float sx = (float)sub->width / (float)from->width;
        float sy = (float)sub->height / (float)from->height;
        float ox = (float)sub->x / (float)from->width;
        float oy = (float)sub->y / (float)from->height;
        push.uv_step[0] = sx / (float)to->width;
        push.uv_step[1] = sy / (float)to->height;
        push.uv_origin[0] = ox + 0.5f * push.uv_step[0];
        push.uv_origin[1] = oy + 0.5f * push.uv_step[1];
        push.half_pixel[0] = 0.5f * push.uv_step[0];
        push.half_pixel[1] = 0.5f * push.uv_step[1];
        push.uv_min[0] = ((float)sub->x + 0.5f) / (float)from->width;
        push.uv_min[1] = ((float)sub->y + 0.5f) / (float)from->height;
        push.uv_max[0] = ((float)(sub->x + sub->width) - 0.5f) / (float)from->width;
        push.uv_max[1] = ((float)(sub->y + sub->height) - 0.5f) / (float)from->height;
    } else {
        push.uv_step[0] = 1.0f / (float)to->width;
        push.uv_step[1] = 1.0f / (float)to->height;
        push.uv_origin[0] = 0.5f / (float)to->width;
        push.uv_origin[1] = 0.5f / (float)to->height;
        push.half_pixel[0] = 0.5f / (float)to->width;
        push.half_pixel[1] = 0.5f / (float)to->height;
        push.uv_min[0] = 0.0f;
        push.uv_min[1] = 0.0f;
        push.uv_max[0] = 1.0f;
        push.uv_max[1] = 1.0f;
    }
    v->base.gl_calls++;
    bool dispatched = false;
    for (int i = 0; i < count; i++) {
        int32_t x1 = rects[i].x1 < 0 ? 0 : rects[i].x1, y1 = rects[i].y1 < 0 ? 0 : rects[i].y1;
        int32_t x2 = rects[i].x2 > to->width ? to->width : rects[i].x2;
        int32_t y2 = rects[i].y2 > to->height ? to->height : rects[i].y2;
        if (x2 <= x1 || y2 <= y1) continue;
        push.rect_off[0] = x1;
        push.rect_off[1] = y1;
        push.rect_end[0] = x2;
        push.rect_end[1] = y2;
        vkCmdPushConstants(cb, v->blur_comp_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push),
            &push);
        vkCmdDispatch(cb, (uint32_t)((x2 - x1 + 7) / 8), (uint32_t)((y2 - y1 + 7) / 8), 1);
        v->base.gl_calls += 2;
        dispatched = true;
    }
    return dispatched;
}

static bool blur_compute_passes(struct vk *v, struct vk_image *direct_src, const struct box *grown,
        struct gfx_level *const *levels, int passes, double offset, const struct region *down,
        const struct region *up, float noise, float vibrancy) {
    if (!v->compute_blur || !v->down_comp || !v->up_comp) return false;
    if (direct_src && (!direct_src->set || !image_use(v, direct_src))) return false;
    for (int i = 0; i <= passes; i++) {
        struct vk_image *img = level_image(levels[i]);
        if (!img || !img->storage_set || !img->set) return false;
    }
    struct batch *b = batch(v);
    if (!b) return false;
    rendering_close(v);
    VkCommandBuffer cb = b->main;
    memory_barrier(v, cb,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_COPY_BIT |
            VK_PIPELINE_STAGE_2_BLIT_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT,
        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT |
            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    bool ran = false;
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, v->down_comp);
    v->base.gl_calls++;
    for (int i = 1; i <= passes; i++) {
        struct vk_image *from = (i == 1 && direct_src) ? direct_src : level_image(levels[i - 1]);
        struct vk_image *to = level_image(levels[i]);
        if (!from || !to) continue;
        if (blur_compute_dispatch(v, cb, from, to, (i == 1 && direct_src) ? grown : NULL,
                offset, &down[i], 0, 1)) {
            ran = true;
            memory_barrier(v, cb, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        }
    }
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, v->up_comp);
    v->base.gl_calls++;
    for (int i = passes - 1; i >= 0; i--) {
        struct vk_image *from = level_image(levels[i + 1]), *to = level_image(levels[i]);
        if (!from || !to) continue;
        bool did = blur_compute_dispatch(v, cb, from, to, NULL, offset, &up[i],
            i == 0 ? noise : 0, i == 0 ? vibrancy : 1);
        if (did) ran = true;
        if (i > 0 && did) {
            memory_barrier(v, cb, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        }
    }
    if (ran) {
        memory_barrier(v, cb, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT |
                VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT |
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT |
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_TRANSFER_READ_BIT |
                VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    }
    return true;
}

bool gfx_blur_direct(struct render *r, struct buffer *buffer, struct box grown, int shrink,
        struct gfx_level *const *levels, int passes, double offset, const struct region *down,
        const struct region *up, float noise, float vibrancy) {
    struct vk *v = V(r);
    if (shrink != 0 || !v->compute_blur) return false;
    struct vk_image *src = import(v, buffer);
    if (!src) return false;
    return blur_compute_passes(v, src, &grown, levels, passes, offset, down, up, noise, vibrancy);
}

void gfx_blur_passes(struct render *r, struct gfx_level *const *levels, int passes,
        double offset, const struct region *down, const struct region *up, float noise,
        float vibrancy) {
    struct vk *v = V(r);
    if (v->compute_blur &&
            blur_compute_passes(v, NULL, NULL, levels, passes, offset, down, up, noise, vibrancy))
        return;
    if (!v->down || !v->up) return;
    VkPipelineStageFlags2 color = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkPipelineStageFlags2 frag = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    VkPipelineStageFlags2 both = frag | color;
    VkAccessFlags2 sample = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    VkAccessFlags2 attach = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT |
        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    for (int i = 1; i <= passes; i++) {
        struct vk_image *from = level_image(levels[i - 1]), *to = level_image(levels[i]);
        if (!from || !to) continue;
        if (i < passes)
            sample_pass(v, v->down, from, to, offset, &down[i], 0, 1, color, frag, sample);
        else
            sample_pass(v, v->down, from, to, offset, &down[i], 0, 1, both, both, sample | attach);
    }
    for (int i = passes - 1; i >= 0; i--) {
        struct vk_image *from = level_image(levels[i + 1]), *to = level_image(levels[i]);
        if (!from || !to) continue;
        if (i > 0)
            sample_pass(v, v->up, from, to, offset, &up[i], 0, 1, both, both, sample | attach);
        else
            sample_pass(v, v->up, from, to, offset, &up[i], noise, vibrancy, both,
                both | VK_PIPELINE_STAGE_2_BLIT_BIT, sample | attach |
                VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
    }
}

void gfx_backdrop_grab(struct render *r, struct buffer *buffer, const struct gfx_level *level,
        struct box need, const struct box *shown, const struct box *clear, int count) {
    struct vk *v = V(r);
    struct vk_image *source = import(v, buffer), *dest = level_image(level);
    if (!source || !dest || !image_use(v, source)) return;
    bool any = false, full = false;
    for (int i = 0; i < count; i++) {
        if (box_empty(&clear[i])) continue;
        any = true;
        if (clear[i].x <= 0 && clear[i].y <= 0 &&
                clear[i].x + clear[i].width >= dest->width &&
                clear[i].y + clear[i].height >= dest->height) full = true;
    }
    static const VkClearColorValue clear_color = {{ 0, 0, 0, 1 }};
    if (full && !(v->rendering && v->bound == dest)) {
        if (rendering_begin_op(v, dest, VK_ATTACHMENT_LOAD_OP_CLEAR, clear_color.float32))
            rendering_end(v);
    } else if (any && rendering_begin(v, dest)) {
        for (int i = 0; i < count; i++) {
            if (box_empty(&clear[i])) continue;
            int x = clear[i].x, y = clear[i].y, w = clear[i].width, h = clear[i].height;
            if (x < 0) { w += x; x = 0; }
            if (y < 0) { h += y; y = 0; }
            if (x + w > dest->width) w = dest->width - x;
            if (y + h > dest->height) h = dest->height - y;
            if (w <= 0 || h <= 0) continue;
            VkClearAttachment attachment = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .colorAttachment = 0,
                .clearValue = { .color = clear_color },
            };
            VkClearRect rect = { { { x, y }, { (uint32_t)w, (uint32_t)h } }, 0, 1 };
            vkCmdClearAttachments(v->open->main, 1, &attachment, 1, &rect);
        }
    }
    if (shown) {
        VkCommandBuffer cb = transfer_begin(v);
        int sx = shown->x, sy = shown->y, dx = shown->x - need.x, dy = shown->y - need.y;
        int w = shown->width, h = shown->height;
        if (cb && clip_copy(&sx, &sy, &dx, &dy, &w, &h, source, dest)) {
            if (can_copy_image(source, dest)) {
                VkImageCopy region = copy_region(sx, sy, dx, dy, w, h);
                vkCmdCopyImage(cb, source->image, VK_IMAGE_LAYOUT_GENERAL, dest->image,
                    VK_IMAGE_LAYOUT_GENERAL, 1, &region);
            } else {
                VkImageBlit region = blit_region(sx, sy, sx + w, sy + h, dx, dy, dx + w, dy + h);
                vkCmdBlitImage(cb, source->image, VK_IMAGE_LAYOUT_GENERAL, dest->image,
                    VK_IMAGE_LAYOUT_GENERAL, 1, &region, VK_FILTER_NEAREST);
            }
            blit_barrier(v, cb);
        }
    }
    rendering_end(v);
}

static VkPipeline user_pipeline(struct vk *v, struct gfx_prog *p, VkFormat format) {
    int slot = -1;
    for (int i = 0; i < TARGET_FORMATS; i++) {
        if (p->pipelines[i] && p->formats[i] == format) return p->pipelines[i];
        if (!p->pipelines[i] && slot < 0) slot = i;
    }
    if (slot < 0) return VK_NULL_HANDLE;
    p->pipelines[slot] = pipeline_make(v, format, 1, v->vert, p->frag, v->user_layout, true);
    p->formats[slot] = format;
    if (!p->pipelines[slot]) tomoe_log(LOG_ERROR, "tomoe: vulkan cannot build a user shader pipeline");
    return p->pipelines[slot];
}

static VkDescriptorSet chunk_ubo(struct vk *v, struct chunk *c) {
    if (c->ubo) return c->ubo;
    VkDescriptorSetAllocateInfo alloc = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = v->ubo_pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &v->ubo_layout,
    };
    if (vkAllocateDescriptorSets(v->dev, &alloc, &c->ubo) != VK_SUCCESS) {
        for (struct chunk *spare = v->spare; spare; spare = spare->next) {
            if (!spare->ubo) continue;
            vkFreeDescriptorSets(v->dev, v->ubo_pool, 1, &spare->ubo);
            spare->ubo = VK_NULL_HANDLE;
        }
        if (vkAllocateDescriptorSets(v->dev, &alloc, &c->ubo) != VK_SUCCESS) {
            c->ubo = VK_NULL_HANDLE;
            tomoe_log(LOG_ERROR, "tomoe: vulkan ran out of user shader uniform sets");
            return VK_NULL_HANDLE;
        }
    }
    VkDescriptorBufferInfo info = { c->buffer, 0, sizeof(struct user_uniforms) };
    VkWriteDescriptorSet write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = c->ubo,
        .dstBinding = 0,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
        .pBufferInfo = &info,
    };
    vkUpdateDescriptorSets(v->dev, 1, &write, 0, NULL);
    return c->ubo;
}

static void user_uniforms_fill(struct user_uniforms *u, const struct gfx_shader_params *sp) {
    memset(u, 0, sizeof(*u));
    u->mouse[0] = (float)sp->mouse_x;
    u->mouse[1] = (float)sp->mouse_y;
    if (sp->marks_box) memcpy(u->marks_box, sp->marks_box, sizeof(u->marks_box));
    if (sp->beneath) memcpy(u->beneath_box, sp->beneath_box, sizeof(u->beneath_box));
    if (sp->frost) {
        double fw = (double)(sp->frost->width << 1), fh = (double)(sp->frost->height << 1);
        if (fw > 0 && fh > 0) {
            u->frost_box[0] = (float)-sp->box.x;
            u->frost_box[1] = (float)(sp->box.y + sp->box.height);
            u->frost_box[2] = (float)(1.0 / fw);
            u->frost_box[3] = (float)(-1.0 / fh);
        }
    }
    u->resolution[0] = (float)sp->box.width;
    u->resolution[1] = (float)sp->box.height;
    u->resolution[2] = 1;
    u->time = (float)sp->time;
    u->frame = sp->frame;
    int trails = sp->trail_count < 0 ? 0 : sp->trail_count > POINTER_TRAIL ? POINTER_TRAIL :
        sp->trail_count;
    u->trail_count = sp->trail_count;
    if (trails && sp->trail) memcpy(u->trail, sp->trail, (size_t)trails * sizeof(u->trail[0]));
    int data = sp->data_count < 0 ? 0 : sp->data_count > SHADER_DATA ? SHADER_DATA : sp->data_count;
    if (data && sp->data) memcpy(u->data, sp->data, (size_t)data * sizeof(float));
    int windows = sp->window_count < 0 ? 0 : sp->window_count > SHADER_WINDOWS ? SHADER_WINDOWS :
        sp->window_count;
    u->window_count = sp->window_count;
    if (windows && sp->windows) memcpy(u->windows, sp->windows, (size_t)windows * sizeof(u->windows[0]));
    if (sp->selection) memcpy(u->selection, sp->selection, sizeof(u->selection));
}

void gfx_shader_draw(struct pass *pass, struct gfx_prog *p, const struct gfx_shader_params *sp,
        const float pos[8], const float local[8]) {
    struct vk *v = V(pass->r);
    struct render *r = pass->r;
    if (!p || p->builtin || !p->frag || p->v != v || !v->user_layout || !v->target || !v->dummy)
        return;
    VkPipeline pipeline = user_pipeline(v, p, v->target->format);
    if (!pipeline) return;
    struct vk_image *beneath = sp->beneath ? level_image(sp->beneath) : NULL;
    struct vk_image *frost = sp->frost && sp->frost_ok ? level_image(sp->frost) : NULL;
    struct vk_image *marks = NULL;
    if (sp->marks && (p->reads & (1u << GFX_IN_MARKS))) {
        struct vk_texture *t = texture_of(sp->marks);
        if (t->image && t->image->set && image_use(v, t->image)) marks = t->image;
    }
    pass_flush(pass);
    struct chunk *chunk;
    VkDeviceSize uniform_offset;
    struct user_uniforms *u = stage_alloc_in(v, sizeof(*u), v->ubo_align, &chunk, &uniform_offset);
    if (!u) return;
    user_uniforms_fill(u, sp);
    VkDescriptorSet ubo = chunk_ubo(v, chunk);
    VkBuffer buffer;
    VkDeviceSize offset;
    struct scene_instance *in = stage_alloc(v, sizeof(*in), &buffer, &offset);
    if (!ubo || !in || !rendering_begin_area(v, v->target, pass_area(pass, v->target), VK_ATTACHMENT_LOAD_OP_LOAD, NULL)) return;
    memset(in, 0, sizeof(*in));
    memcpy(in->pos, pos, sizeof(in->pos));
    memcpy(in->local, local, sizeof(in->local));
    VkCommandBuffer cb = v->open->main;
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    VkDescriptorSet sets[USER_SETS] = { ubo, v->sampler_sets[0],
        beneath && beneath->set ? beneath->set : v->dummy->set,
        marks ? marks->set : v->dummy->set,
        frost && frost->set ? frost->set : v->dummy->set };
    uint32_t dynamic = (uint32_t)uniform_offset;
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, v->user_layout, 0, USER_SETS, sets,
        1, &dynamic);
    vkCmdBindVertexBuffers(cb, 0, 1, &buffer, &offset);
    r->gl_calls += 4;
    int count = 1;
    const struct rect *rects = pass->clip ? region_rectangles((struct region *)pass->clip, &count) :
        NULL;
    for (int i = 0; i < count; i++) {
        VkRect2D scissor = { { 0, 0 }, { (uint32_t)v->target->width, (uint32_t)v->target->height } };
        if (rects) {
            int32_t x1 = rects[i].x1 < 0 ? 0 : rects[i].x1, y1 = rects[i].y1 < 0 ? 0 : rects[i].y1;
            int32_t x2 = rects[i].x2 > v->target->width ? v->target->width : rects[i].x2;
            int32_t y2 = rects[i].y2 > v->target->height ? v->target->height : rects[i].y2;
            if (x2 <= x1 || y2 <= y1) continue;
            scissor = (VkRect2D){ { x1, y1 }, { (uint32_t)(x2 - x1), (uint32_t)(y2 - y1) } };
            r->fragments += quad_pixels(pass, pos, &rects[i]);
        } else {
            r->fragments += quad_pixels(pass, pos, NULL);
        }
        vkCmdSetScissor(cb, 0, 1, &scissor);
        vkCmdDraw(cb, 6, 1, 0, 0);
        r->gl_calls += 2;
        r->quads++;
    }
    r->runs++;
}

void gfx_glass_draw(struct pass *pass, struct gfx_prog *p, struct texture *map,
        const struct gfx_level *glass, struct box grown, struct fbox box, double bend,
        const float pos[8], const float local[8]) {
    struct vk *v = V(pass->r);
    struct render *r = pass->r;
    struct vk_texture *lens = texture_of(map);
    struct vk_image *beneath = level_image(glass);
    struct target_pipelines *pipes = v->target ? pipelines_for(v, v->target->format) : NULL;
    if (!p || !pipes || !pipes->glass || !lens->image || !lens->image->set || !beneath ||
            !beneath->set || grown.width < 1 || grown.height < 1 ||
            !image_use(v, lens->image)) return;
    pass_flush(pass);
    VkBuffer buffer;
    VkDeviceSize offset;
    struct scene_instance *in = stage_alloc(v, sizeof(*in), &buffer, &offset);
    if (!in || !rendering_begin_area(v, v->target, pass_area(pass, v->target), VK_ATTACHMENT_LOAD_OP_LOAD, NULL)) return;
    memset(in, 0, sizeof(*in));
    memcpy(in->pos, pos, sizeof(in->pos));
    memcpy(in->local, local, sizeof(in->local));
    struct glass_push push = {
        { (float)grown.x, (float)grown.y, 1.0f / (float)grown.width, 1.0f / (float)grown.height },
        { (float)box.width, (float)box.height }, (float)bend };
    VkCommandBuffer cb = v->open->main;
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipes->glass);
    VkDescriptorSet sets[4] = { lens->image->set, v->sampler_sets[0], beneath->set,
        v->sampler_sets[0] };
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, v->glass_layout, 0, 4, sets, 0,
        NULL);
    vkCmdPushConstants(cb, v->glass_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    vkCmdBindVertexBuffers(cb, 0, 1, &buffer, &offset);
    r->gl_calls += 4;
    int count = 1;
    const struct rect *rects = pass->clip ? region_rectangles((struct region *)pass->clip, &count) :
        NULL;
    for (int i = 0; i < count; i++) {
        VkRect2D scissor = { { 0, 0 }, { (uint32_t)v->target->width, (uint32_t)v->target->height } };
        if (rects) {
            int32_t x1 = rects[i].x1 < 0 ? 0 : rects[i].x1, y1 = rects[i].y1 < 0 ? 0 : rects[i].y1;
            int32_t x2 = rects[i].x2 > v->target->width ? v->target->width : rects[i].x2;
            int32_t y2 = rects[i].y2 > v->target->height ? v->target->height : rects[i].y2;
            if (x2 <= x1 || y2 <= y1) continue;
            scissor = (VkRect2D){ { x1, y1 }, { (uint32_t)(x2 - x1), (uint32_t)(y2 - y1) } };
            r->fragments += quad_pixels(pass, pos, &rects[i]);
        } else {
            r->fragments += quad_pixels(pass, pos, NULL);
        }
        vkCmdSetScissor(cb, 0, 1, &scissor);
        vkCmdDraw(cb, 6, 1, 0, 0);
        r->gl_calls += 2;
        r->quads++;
    }
    r->runs++;
}

struct gfx_prog *gfx_glass_program(struct render *r) {
    struct vk *v = V(r);
    if (!v->glass_layout) return NULL;
    v->glass_program.builtin = 1;
    return &v->glass_program;
}

void gfx_external_draw(struct pass *pass, const struct texture_options *options, bool alpha,
        struct fbox clip, double radius, const float pos[8], const float local[8],
        const float texcoords[8]) {
    struct vk_texture *t = texture_of(options->texture);
    if (!t->external) return;
    struct ext_push push = { .alpha = options->alpha ? *options->alpha : 1, .opaque = alpha ? 0 : 1,
        .clip = 1, .size = { (float)clip.width, (float)clip.height }, .radius = (float)radius };
    if (options->tint) memcpy(push.tint, options->tint, sizeof(push.tint));
    external_quad(pass, t, pos, local, texcoords, pass->clip, &push);
}

bool gfx_spirv_shaders(struct render *r) {
    return r && V(r)->user_layout;
}

enum {
    SPV_NAME = 5, SPV_ENTRY = 15, SPV_TYPE_INT = 21, SPV_CONSTANT = 43, SPV_VARIABLE = 59,
    SPV_LOAD = 61, SPV_ACCESS = 65, SPV_ACCESS_IN_BOUNDS = 66, SPV_PTR_ACCESS = 67,
    SPV_DECORATE = 71, SPV_COPY_MEMORY = 63,
    DECO_BUILTIN = 11, DECO_LOCATION = 30, DECO_BINDING = 33, DECO_SET = 34,
    CLASS_UNIFORM_CONSTANT = 0, CLASS_INPUT = 1, CLASS_UNIFORM = 2, CLASS_OUTPUT = 3,
    CLASS_PUSH = 9, CLASS_STORAGE = 12,
};

struct spv_id { int32_t set, binding, location, constant; uint32_t storage; bool builtin, variable; };

static bool spirv_scan(const uint32_t *code, size_t words, uint32_t *reads) {
    if (words < 5 || code[0] != 0x07230203u || code[1] > 0x00010600u || code[3] == 0 ||
            code[3] > (1u << 22)) return false;
    uint32_t bound = code[3];
    struct spv_id *ids = calloc(bound, sizeof(*ids));
    if (!ids) return false;
    for (uint32_t i = 0; i < bound; i++) ids[i] = (struct spv_id){ -1, -1, -1, -1, 0, false, false };
    bool ok = true;
    for (int pass = 0; pass < 2 && ok; pass++) {
        size_t at = 5;
        while (at < words) {
            uint32_t count = code[at] >> 16, op = code[at] & 0xffff;
            if (!count || at + count > words) {
                ok = false;
                break;
            }
            const uint32_t *w = code + at;
            if (pass == 0) {
                if (op == SPV_DECORATE && count >= 3 && w[1] < bound) {
                    struct spv_id *id = &ids[w[1]];
                    if (w[2] == DECO_SET && count >= 4) id->set = (int32_t)w[3];
                    else if (w[2] == DECO_BINDING && count >= 4) id->binding = (int32_t)w[3];
                    else if (w[2] == DECO_LOCATION && count >= 4) id->location = (int32_t)w[3];
                    else if (w[2] == DECO_BUILTIN) id->builtin = true;
                } else if (op == SPV_CONSTANT && count == 4 && w[2] < bound) {
                    ids[w[2]].constant = (int32_t)w[3];
                } else if (op == SPV_VARIABLE && count >= 4 && w[2] < bound) {
                    ids[w[2]].variable = true;
                    ids[w[2]].storage = w[3];
                }
            } else {
                if (op == SPV_VARIABLE && count >= 4 && w[2] < bound) {
                    const struct spv_id *id = &ids[w[2]];
                    switch (w[3]) {
                    case CLASS_UNIFORM_CONSTANT:
                    case CLASS_UNIFORM:
                        if (id->binding != 0 || id->set < 0 || id->set >= USER_SETS) ok = false;
                        break;
                    case CLASS_INPUT:
                        if (!id->builtin && id->location != 5) ok = false;
                        break;
                    case CLASS_OUTPUT:
                        if (id->location != 0) ok = false;
                        break;
                    case CLASS_PUSH:
                    case CLASS_STORAGE:
                        ok = false;
                        break;
                    default:
                        break;
                    }
                } else if ((op == SPV_LOAD && count >= 4) || op == SPV_COPY_MEMORY) {
                    uint32_t pointer = op == SPV_LOAD ? w[3] : count >= 3 ? w[2] : 0;
                    if (pointer < bound && ids[pointer].variable) {
                        int32_t set = ids[pointer].set;
                        if (set == 0) *reads |= ~0u;
                        else if (set == 2) *reads |= 1u << GFX_IN_BENEATH;
                        else if (set == 3) *reads |= 1u << GFX_IN_MARKS;
                        else if (set == 4) *reads |= 1u << GFX_IN_FROST;
                    }
                } else if ((op == SPV_ACCESS || op == SPV_ACCESS_IN_BOUNDS || op == SPV_PTR_ACCESS) &&
                        count >= 5 && w[3] < bound && ids[w[3]].variable && ids[w[3]].set == 0) {
                    uint32_t index = op == SPV_PTR_ACCESS ? (count >= 6 ? w[5] : 0) : w[4];
                    int32_t member = index < bound ? ids[index].constant : -1;
                    switch (member) {
                    case TOMOE_VK_MOUSE: *reads |= 1u << GFX_IN_MOUSE; break;
                    case TOMOE_VK_MARKS_BOX: *reads |= 1u << GFX_IN_MARKS; break;
                    case TOMOE_VK_BENEATH_BOX: *reads |= 1u << GFX_IN_BENEATH; break;
                    case TOMOE_VK_FROST_BOX: *reads |= 1u << GFX_IN_FROST; break;
                    case TOMOE_VK_TRAIL_COUNT: case TOMOE_VK_TRAIL: *reads |= 1u << GFX_IN_TRAIL; break;
                    case TOMOE_VK_WINDOW_COUNT: case TOMOE_VK_WINDOWS: *reads |= 1u << GFX_IN_WINDOWS; break;
                    case TOMOE_VK_DATA: *reads |= 1u << GFX_IN_DATA; break;
                    case TOMOE_VK_SELECTION: *reads |= 1u << GFX_IN_SELECTION; break;
                    case TOMOE_VK_TIME: *reads |= 1u << GFX_IN_TIME; break;
                    case TOMOE_VK_FRAME: *reads |= 1u << GFX_IN_FRAME; break;
                    case TOMOE_VK_RESOLUTION: break;
                    default: *reads |= ~0u; break;
                    }
                }
            }
            at += count;
        }
    }
    free(ids);
    return ok;
}

struct gfx_prog *gfx_user_shader(struct render *r, const void *bytes, size_t length) {
    struct vk *v = V(r);
    if (!v->user_layout || !bytes || length < 20 || length % 4 || length > (1u << 20)) return NULL;
    size_t words = length / 4;
    uint64_t hash = UINT64_C(0xcbf29ce484222325);
    for (size_t i = 0; i < length; i++) {
        hash ^= ((const unsigned char *)bytes)[i];
        hash *= UINT64_C(0x100000001b3);
    }
    for (struct gfx_prog *p = v->user_progs; p; p = p->next) {
        if (p->hash != hash || p->words != words || memcmp(p->code, bytes, length)) continue;
        if (p->refs == INT32_MAX) return NULL;
        p->refs++;
        return p;
    }
    struct gfx_prog *p = calloc(1, sizeof(*p));
    if (!p || !(p->code = malloc(length))) {
        free(p);
        return NULL;
    }
    memcpy(p->code, bytes, length);
    p->words = words;
    p->hash = hash;
    if (!spirv_scan(p->code, words, &p->reads)) {
        tomoe_log(LOG_ERROR, "tomoe: a user shader declares inputs outside the Tomoe interface");
        free(p->code);
        free(p);
        return NULL;
    }
    VkShaderModuleCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = length,
        .pCode = p->code,
    };
    if (vkCreateShaderModule(v->dev, &info, NULL, &p->frag) != VK_SUCCESS) {
        free(p->code);
        free(p);
        return NULL;
    }
    p->v = v;
    p->refs = 1;
    p->next = v->user_progs;
    v->user_progs = p;
    return p;
}

void render_shader_destroy(struct gfx_prog *program) {
    if (!program || program->builtin || --program->refs > 0) return;
    struct vk *v = program->v;
    if (v) {
        struct gfx_prog **link = &v->user_progs;
        while (*link && *link != program) link = &(*link)->next;
        if (*link) *link = program->next;
        struct grave g = { .module = program->frag };
        memcpy(g.pipelines, program->pipelines, sizeof(g.pipelines));
        bury(v, g);
    }
    free(program->code);
    free(program);
}

int tomoe_warm_shaders(const char *node, const char *directory, const char *marker,
        const char *name) {
    return 0;
}
