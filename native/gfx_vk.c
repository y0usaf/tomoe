#include "gfx.h"
#include "drm.h"
#include "upload.h"
#include <fcntl.h>
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
#include "shaders/lut.frag.h"
#include "shaders/ext.frag.h"

enum {
    BLENDS = 3,
    TARGET_FORMATS = 8,
    POOL_SETS = 256,
    CHUNK_MIN = 4 << 20,
    SCENE_RESERVE = 4096,
    BATCH_LIMIT = 32,
    LEVEL_POOL = 24,
    YUV_FORMATS = 3,
    YUV_POOL_SETS = 64,
};

struct vk;

struct vk_image {
    struct addon addon;
    struct wl_list link;
    struct vk *v;
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view, sample;
    VkDescriptorSet set;
    VkDescriptorPool pool;
    VkFormat format;
    VkImageUsageFlags usage;
    uint32_t drm_format;
    int width, height;
    bool foreign, target, owned, used;
    uint64_t batch, written, copied;
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
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view, sample;
    VkDescriptorSet set;
    VkDescriptorPool pool;
};

struct chunk {
    struct chunk *next;
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
    uint64_t serial;
    bool busy;
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

struct target_pipelines {
    VkFormat format;
    VkPipeline blend[BLENDS];
    VkPipeline lut;
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
    PFN_vkGetMemoryFdPropertiesKHR memory_fd_properties;
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
    VkDescriptorSetLayout image_layout, sampler_layout;
    VkDescriptorPool sampler_pool;
    VkDescriptorPool *pools;
    size_t pool_count;
    VkSampler samplers[2];
    VkDescriptorSet sampler_sets[2];
    VkPipelineLayout layout;
    VkShaderModule vert, frag, blur_vert, down_frag, up_frag, lut_frag, ext_frag;
    bool ycbcr;
    struct yuv_path yuv[YUV_FORMATS];
    VkPipelineLayout blur_layout, lut_layout;
    VkPipeline down, up;
    struct target_pipelines targets[TARGET_FORMATS];
    struct wl_list images;
    struct vk_image *dummy;
    struct vk_image *target, *bound;
    struct vk_image *pool[LEVEL_POOL];
    int pooled;
    struct {
        struct chunk *chunk;
        uint64_t serial[UPLOAD_FENCES], limit[UPLOAD_FENCES];
        int fences;
        bool failed, unfenced;
    } staging;
    bool rendering;
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

static void full_barrier(struct vk *v, VkCommandBuffer cb) {
    VkMemoryBarrier2 memory = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
    };
    VkDependencyInfo info = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &memory,
    };
    vkCmdPipelineBarrier2(cb, &info);
}

static void image_barrier(VkCommandBuffer cb, VkImage image, VkImageLayout from, VkImageLayout to,
        uint32_t src_family, uint32_t dst_family) {
    VkImageMemoryBarrier2 barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
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

static void chunk_free(struct vk *v, struct chunk *c) {
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
    if (g->set) vkFreeDescriptorSets(v->dev, g->pool, 1, &g->set);
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

static void batch_recycle(struct vk *v, struct batch *b) {
    while (b->chunks) {
        struct chunk *c = b->chunks;
        b->chunks = c->next;
        c->used = 0;
        c->next = v->spare;
        v->spare = c;
    }
    b->acquired_len = 0;
    b->wait_count = 0;
    b->staged = 0;
}

static void retire(struct vk *v, bool wait) {
    for (size_t i = 0; i < v->batch_count; i++) {
        struct batch *b = &v->batches[i];
        if (!b->busy) continue;
        if (wait) vkWaitForFences(v->dev, 1, &b->fence, VK_TRUE, UINT64_MAX);
        if (vkGetFenceStatus(v->dev, b->fence) != VK_SUCCESS) continue;
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
    return true;
}

static void batch_finish(struct vk *v, struct batch *b) {
    batch_recycle(v, b);
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
    vkWaitForFences(v->dev, 1, &pick->fence, VK_TRUE, UINT64_MAX);
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
    v->open = b;
    full_barrier(v, b->pre);
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

static void *stage_alloc(struct vk *v, VkDeviceSize size, VkBuffer *buffer, VkDeviceSize *offset) {
    struct batch *b = batch(v);
    if (!b) return NULL;
    size = (size + 15) & ~(VkDeviceSize)15;
    struct chunk *c = b->chunks;
    if (!c || c->size - c->used < size) {
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
            VkDeviceSize want = CHUNK_MIN;
            while (want < size) want *= 2;
            c = chunk_new(v, want, VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, 0);
            if (!c) {
                tomoe_log(LOG_ERROR, "tomoe: vulkan cannot allocate %llu staging bytes",
                    (unsigned long long)want);
                return NULL;
            }
        }
        c->used = 0;
        c->next = b->chunks;
        b->chunks = c;
    }
    *buffer = c->buffer;
    *offset = c->used;
    b->staged += size;
    void *at = c->map + c->used;
    c->used += size;
    return at;
}

static void rendering_end(struct vk *v) {
    if (!v->rendering) return;
    vkCmdEndRendering(v->open->main);
    full_barrier(v, v->open->main);
    v->rendering = false;
    v->bound = NULL;
}

static bool image_use(struct vk *v, struct vk_image *image) {
    struct batch *b = batch(v);
    if (!b) return false;
    if (!image->foreign || image->batch == b->serial) return true;
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
        struct dma_buf_export_sync_file exported = { .flags = DMA_BUF_SYNC_READ, .fd = -1 };
        if (ioctl(image->fd, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &exported) == 0 && exported.fd >= 0)
            wait_fd(v, exported.fd);
    }
    return true;
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
    VkResult result = vkQueueSubmit2(v->queue, 1, &info, b->fence);
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
    if (v->open && (!v->target || v->open->staged > IDLE_SUBMIT_BYTES)) submit(v, false);
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
    if (!(image->usage & VK_IMAGE_USAGE_SAMPLED_BIT)) return true;
    image->set = descriptor_new(v, image->sample, &image->pool);
    return image->set != VK_NULL_HANDLE;
}

static void image_free(struct vk_image *image) {
    struct vk *v = image->v;
    if (image->fd >= 0) close(image->fd);
    if (v->bound == image) rendering_end(v);
    bury(v, (struct grave){ .image = image->image, .memory = image->memory, .view = image->view,
        .sample = image->sample, .set = image->set, .pool = image->pool });
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
    image_barrier(b->pre, image->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
        VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED);
    if (usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) {
        VkClearColorValue zero = {{ 0, 0, 0, 0 }};
        VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCmdClearColorImage(b->pre, image->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
        full_barrier(v, b->pre);
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
        for (size_t j = 0; j < rows; j++) memcpy(dst + j * row, src + j * stride, row);
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
    if (copied) full_barrier(v, b->main);
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

static struct upload *staging(struct vk *v) {
    struct render *r = &v->base;
    if (!r->upload) return NULL;
    if (!v->staging.chunk && !v->staging.failed &&
            !staging_alloc(v, upload_wanted(r->upload))) v->staging.failed = true;
    return v->staging.chunk ? r->upload : NULL;
}

static void reclaim(void *data) {
    struct vk *v = data;
    if (v->staging.fences) staging_poll(v, false);
}

static void latch_jobs(struct vk *v, bool ready) {
    struct render *r = &v->base;
    struct upload *u = r->upload;
    if (!u || (!upload_pending(u) && (ready || !v->staging.unfenced))) return;
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
        if (t->image->copied == seq) full_barrier(v, b->main);
        t->image->copied = seq;
        vkCmdCopyBufferToImage(b->main, v->staging.chunk->buffer, t->image->image,
            VK_IMAGE_LAYOUT_GENERAL, n, copies);
        r->gl_calls++;
        used = true;
    }
    if (used && v->open) full_barrier(v, v->open->main);
    if (ready && used && !v->target && v->open) {
        uint64_t limit = upload_pending(u) ? u->jobs[u->latch % UPLOAD_JOBS].start : u->alloc;
        bool merged = v->staging.fences == UPLOAD_FENCES;
        int at = merged ? UPLOAD_FENCES - 1 : v->staging.fences++;
        if (at > 0 && v->staging.limit[at - 1] > limit) limit = v->staging.limit[at - 1];
        if (merged && v->staging.limit[at] > limit) limit = v->staging.limit[at];
        v->staging.serial[at] = v->open->serial;
        v->staging.limit[at] = limit;
        used = false;
    }
    if (ready) {
        v->staging.unfenced |= used;
        upload_latch_end(u, false);
        if (timing_enabled) timing_add(TIMING_UPLOAD_LATCH, timing_now() - start);
        return;
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
    uint64_t want = upload_wanted(u);
    if (want && upload_idle(u)) {
        staging_free(v);
        u->stats.resizes++;
        staging_alloc(v, want);
    }
    if (timing_enabled) timing_add(TIMING_UPLOAD_LATCH, timing_now() - start);
}

static void latch(struct vk *v) {
    latch_jobs(v, false);
}

void gfx_upload_latch(struct render *r) {
    latch_jobs(V(r), true);
    idle_submit(V(r));
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
    if (vkCreateGraphicsPipelines(v->dev, VK_NULL_HANDLE, 1, &info, NULL, &pipeline) != VK_SUCCESS)
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
        return t;
    }
    return NULL;
}

static bool rendering_begin(struct vk *v, struct vk_image *target) {
    if (v->rendering && v->bound == target) return true;
    if (!target) return false;
    rendering_end(v);
    struct batch *b = batch(v);
    if (!b || !image_use(v, target)) return false;
    VkRenderingAttachmentInfo color = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = target->view,
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
    };
    VkRenderingInfo info = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = { { 0, 0 }, { (uint32_t)target->width, (uint32_t)target->height } },
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &color,
    };
    if (!target->owned && target->fd >= 0 && v->sync_fd && target->written != b->serial) {
        struct dma_buf_export_sync_file exported = { .flags = DMA_BUF_SYNC_RW, .fd = -1 };
        if (ioctl(target->fd, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &exported) == 0 && exported.fd >= 0)
            wait_fd(v, exported.fd);
    }
    vkCmdBeginRendering(b->main, &info);
    target->written = b->serial;
    VkViewport viewport = { 0, 0, (float)target->width, (float)target->height, 0, 1 };
    vkCmdSetViewport(b->main, 0, 1, &viewport);
    v->rendering = true;
    v->bound = target;
    v->base.gl_calls += 2;
    return true;
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
        const struct scene_run *run, int *blend, VkDescriptorSet *set, int *filter) {
    struct render *r = &v->base;
    if (*blend != run->blend) {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipes->blend[run->blend]);
        *blend = run->blend;
        *set = VK_NULL_HANDLE;
        *filter = -1;
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
    vkCmdSetScissor(cb, 0, 1, &scissor);
    vkCmdDraw(cb, 6, (uint32_t)run->count, 0, (uint32_t)run->first);
    r->gl_calls += 2;
    r->runs++;
    r->quads += run->count;
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
    for (size_t i = 0; i < count; i++)
        if (s->items[i].image && !image_use(v, s->items[i].image)) return;
    VkBuffer buffer;
    VkDeviceSize offset;
    struct scene_instance *out = stage_alloc(v, count * sizeof(*out), &buffer, &offset);
    if (!out) return;
    for (size_t i = 0; i < count; i++) out[i] = s->items[i].in;
    r->uploads++;
    r->upload_bytes += count * sizeof(*out);
    if (!rendering_begin(v, v->target)) return;
    VkCommandBuffer cb = v->open->main;
    vkCmdBindVertexBuffers(cb, 0, 1, &buffer, &offset);
    r->gl_calls++;
    int blend = -1, filter = -1;
    VkDescriptorSet set = VK_NULL_HANDLE;
    struct scene_run run = {0};
    for (size_t i = 0; i < count; i++) {
        const struct scene_item *in = &s->items[i];
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
        if (run.count) run_draw(v, cb, pipes, &run, &blend, &set, &filter);
        run = (struct scene_run){ .first = i, .count = 1, .image = in->image,
            .filter = in->filter, .blend = in->blend };
        memcpy(run.scissor, in->scissor, sizeof(run.scissor));
    }
    if (run.count) run_draw(v, cb, pipes, &run, &blend, &set, &filter);
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
    pass_flush(pass);
    if (t->external) {
        struct ext_push push = { .alpha = alpha, .opaque = t->alpha ? 0 : 1 };
        memcpy(push.tint, quad.tint, sizeof(push.tint));
        external_quad(pass, t, pos, NULL, texcoords, options->clip ? options->clip : pass->clip,
            &push);
        return;
    }
    if (!options->lut) {
        pass_scene(pass, &quad);
        pass_flush(pass);
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
    if (!in || !rendering_begin(v, v->target)) return;
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

static bool device_matches(VkPhysicalDevice phys, dev_t node) {
    uint32_t count = 0;
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(phys, &props);
    if (props.apiVersion < VK_API_VERSION_1_3) return false;
    vkEnumerateDeviceExtensionProperties(phys, NULL, &count, NULL);
    VkExtensionProperties *list = calloc(count ? count : 1, sizeof(*list));
    if (!list) return false;
    vkEnumerateDeviceExtensionProperties(phys, NULL, &count, list);
    bool ok = has_extension(list, count, VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME);
    for (size_t i = 0; ok && i < sizeof(device_extensions) / sizeof(device_extensions[0]); i++)
        ok = has_extension(list, count, device_extensions[i]);
    free(list);
    if (!ok) return false;
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
    VkResult result = vkCreateInstance(&instance, NULL, &v->instance);
    if (result != VK_SUCCESS) {
        tomoe_log(LOG_ERROR, "tomoe: vulkan instance creation failed (%d)", (int)result);
        v->instance = VK_NULL_HANDLE;
        return false;
    }
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(v->instance, &count, NULL);
    VkPhysicalDevice *devices = calloc(count ? count : 1, sizeof(*devices));
    if (!devices) return false;
    vkEnumeratePhysicalDevices(v->instance, &count, devices);
    for (uint32_t i = 0; i < count && !v->phys; i++)
        if (device_matches(devices[i], st.st_rdev)) v->phys = devices[i];
    free(devices);
    if (!v->phys) {
        tomoe_log(LOG_ERROR, "tomoe: no Vulkan 1.3 device with dmabuf support matches %s",
            v->base.node ? v->base.node : "the render node");
        return false;
    }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(v->phys, &props);
    v->atom = props.limits.nonCoherentAtomSize;
    v->max_extent = props.limits.maxImageDimension2D;
    vkGetPhysicalDeviceMemoryProperties(v->phys, &v->memory);
    uint32_t families = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(v->phys, &families, NULL);
    VkQueueFamilyProperties *family = calloc(families ? families : 1, sizeof(*family));
    if (!family) return false;
    vkGetPhysicalDeviceQueueFamilyProperties(v->phys, &families, family);
    v->family = UINT32_MAX;
    for (uint32_t i = 0; i < families && v->family == UINT32_MAX; i++)
        if (family[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) v->family = i;
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
    VkDeviceCreateInfo device = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &features13,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue,
        .enabledExtensionCount = sizeof(device_extensions) / sizeof(device_extensions[0]),
        .ppEnabledExtensionNames = device_extensions,
    };
    result = vkCreateDevice(v->phys, &device, NULL, &v->dev);
    if (result != VK_SUCCESS) {
        tomoe_log(LOG_ERROR, "tomoe: vulkan device creation failed (%d)", (int)result);
        v->dev = VK_NULL_HANDLE;
        return false;
    }
    vkGetDeviceQueue(v->dev, v->family, 0, &v->queue);
    v->memory_fd_properties = (PFN_vkGetMemoryFdPropertiesKHR)
        vkGetDeviceProcAddr(v->dev, "vkGetMemoryFdPropertiesKHR");
    v->semaphore_fd = (PFN_vkGetSemaphoreFdKHR)vkGetDeviceProcAddr(v->dev, "vkGetSemaphoreFdKHR");
    v->import_semaphore = (PFN_vkImportSemaphoreFdKHR)
        vkGetDeviceProcAddr(v->dev, "vkImportSemaphoreFdKHR");
    if (!v->memory_fd_properties) return false;
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
    tomoe_log(LOG_INFO, "tomoe: vulkan renderer on %s (%s)", props.deviceName,
        v->sync_fd ? "sync_file fences" : "no fence export");
    return true;
}

static bool modifier_usable(struct vk *v, VkFormat format, uint64_t modifier,
        VkImageUsageFlags usage) {
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
    return external_props.externalMemoryProperties.externalMemoryFeatures &
        VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT;
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
                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT))
                format_set_add(&r->texture_formats, p->drm, modifier);
            if (sample && render && modifier_usable(v, p->vk, modifier,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT))
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
                    VK_IMAGE_USAGE_SAMPLED_BIT)) continue;
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

static bool layouts_init(struct vk *v) {
    VkDescriptorSetLayoutBinding image = {
        .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
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
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
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
    v->down = pipeline_make(v, (VkFormat)LEVEL_FORMAT, 0, v->blur_vert, v->down_frag,
        v->blur_layout, false);
    v->up = pipeline_make(v, (VkFormat)LEVEL_FORMAT, 0, v->blur_vert, v->up_frag,
        v->blur_layout, false);
    if (!v->down || !v->up) tomoe_log(LOG_ERROR, "tomoe: vulkan blur pipelines failed");
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
    if (!in || !rendering_begin(v, v->target)) return;
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
        if (v->sampler_pool) vkDestroyDescriptorPool(v->dev, v->sampler_pool, NULL);
        for (int i = 0; i < 2; i++)
            if (v->samplers[i]) vkDestroySampler(v->dev, v->samplers[i], NULL);
        for (int i = 0; i < TARGET_FORMATS; i++)
            if (v->targets[i].lut) vkDestroyPipeline(v->dev, v->targets[i].lut, NULL);
        yuv_finish(v);
        if (v->ext_frag) vkDestroyShaderModule(v->dev, v->ext_frag, NULL);
        if (v->lut_layout) vkDestroyPipelineLayout(v->dev, v->lut_layout, NULL);
        if (v->lut_frag) vkDestroyShaderModule(v->dev, v->lut_frag, NULL);
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
        vkDestroyDevice(v->dev, NULL);
    }
    if (v->instance) vkDestroyInstance(v->instance, NULL);
    if (current_vk == v) current_vk = NULL;
    free(v->batches);
    free(v->pools);
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

void gfx_finish(struct render *r) {
    finish(V(r));
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
    latch(v);
    return true;
}

void gfx_target_end(struct render *r) {
    struct vk *v = V(r);
    rendering_end(v);
    v->target = NULL;
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
    return false;
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
    if (!image) image = image_new(v, (VkFormat)LEVEL_FORMAT, width, height,
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, true);
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

static void blit(VkCommandBuffer cb, struct vk_image *from, struct vk_image *to,
        int sx0, int sy0, int sx1, int sy1, int dx0, int dy0, int dx1, int dy1, VkFilter filter) {
    VkImageBlit region = {
        .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .srcOffsets = { { sx0, sy0, 0 }, { sx1, sy1, 1 } },
        .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .dstOffsets = { { dx0, dy0, 0 }, { dx1, dy1, 1 } },
    };
    vkCmdBlitImage(cb, from->image, VK_IMAGE_LAYOUT_GENERAL, to->image, VK_IMAGE_LAYOUT_GENERAL,
        1, &region, filter);
}

static VkCommandBuffer transfer_begin(struct vk *v) {
    struct batch *b = batch(v);
    if (!b) return VK_NULL_HANDLE;
    rendering_end(v);
    return b->main;
}

void gfx_level_blit(const struct gfx_level *from, const struct gfx_level *to,
        const struct box *samples, int count, int sx, int sy, int dx, int dy) {
    struct vk *v = current_vk;
    struct vk_image *a = level_image(from), *b = level_image(to);
    if (!v || !a || !b || count <= 0) return;
    VkCommandBuffer cb = transfer_begin(v);
    if (!cb) return;
    bool copied = false;
    for (int i = 0; i < count; i++) {
        int x0 = samples[i].x - sx, y0 = samples[i].y - sy;
        int x1 = samples[i].x - dx, y1 = samples[i].y - dy;
        int w = samples[i].width, h = samples[i].height;
        if (!clip_copy(&x0, &y0, &x1, &y1, &w, &h, a, b)) continue;
        blit(cb, a, b, x0, y0, x0 + w, y0 + h, x1, y1, x1 + w, y1 + h, VK_FILTER_NEAREST);
        copied = true;
    }
    if (copied) full_barrier(v, cb);
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
    if (!source || !dest || !image_use(v, source)) return;
    VkCommandBuffer cb = transfer_begin(v);
    if (!cb) return;
    bool copied = false;
    for (int i = 0; i < copies; i++) {
        int dx0 = copy[i].x1, dy0 = copy[i].y1, dx1 = copy[i].x2, dy1 = copy[i].y2;
        int sx0 = grown.x + (dx0 << shrink), sy0 = grown.y + (dy0 << shrink);
        int sx1 = grown.x + (dx1 << shrink), sy1 = grown.y + (dy1 << shrink);
        if (shrink) {
            if (sx0 < 0 || sy0 < 0 || sx1 > source->width || sy1 > source->height ||
                    dx0 < 0 || dy0 < 0 || dx1 > dest->width || dy1 > dest->height ||
                    dx1 <= dx0 || dy1 <= dy0) continue;
            blit(cb, source, dest, sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, VK_FILTER_LINEAR);
            copied = true;
            continue;
        }
        int w = dx1 - dx0, h = dy1 - dy0;
        if (!clip_copy(&sx0, &sy0, &dx0, &dy0, &w, &h, source, dest)) continue;
        blit(cb, source, dest, sx0, sy0, sx0 + w, sy0 + h, dx0, dy0, dx0 + w, dy0 + h,
            VK_FILTER_NEAREST);
        copied = true;
    }
    if (copied) full_barrier(v, cb);
}

static void sample_pass(struct vk *v, VkPipeline pipeline, struct vk_image *from,
        struct vk_image *to, double offset, const struct region *where, float grain,
        float vibrancy) {
    int count = 0;
    const struct rect *rects = region_rectangles(where, &count);
    if (!count || !from->set || !rendering_begin(v, to)) return;
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
    rendering_end(v);
}

void gfx_blur_passes(struct render *r, struct gfx_level *const *levels, int passes,
        double offset, const struct region *down, const struct region *up, float noise,
        float vibrancy) {
    struct vk *v = V(r);
    if (!v->down || !v->up) return;
    for (int i = 1; i <= passes; i++) {
        struct vk_image *from = level_image(levels[i - 1]), *to = level_image(levels[i]);
        if (from && to) sample_pass(v, v->down, from, to, offset, &down[i], 0, 1);
    }
    for (int i = passes - 1; i >= 0; i--) {
        struct vk_image *from = level_image(levels[i + 1]), *to = level_image(levels[i]);
        if (from && to) sample_pass(v, v->up, from, to, offset, &up[i], i ? 0 : noise,
            i ? 1 : vibrancy);
    }
}

void gfx_backdrop_grab(struct render *r, struct buffer *buffer, const struct gfx_level *level,
        struct box need, const struct box *shown, const struct box *clear, int count) {
    struct vk *v = V(r);
    struct vk_image *source = import(v, buffer), *dest = level_image(level);
    if (!source || !dest || !image_use(v, source)) return;
    bool any = false;
    for (int i = 0; i < count; i++) any = any || !box_empty(&clear[i]);
    if (any && rendering_begin(v, dest)) {
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
                .clearValue = { .color = {{ 0, 0, 0, 1 }} },
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
            blit(cb, source, dest, sx, sy, sx + w, sy + h, dx, dy, dx + w, dy + h, VK_FILTER_NEAREST);
            full_barrier(v, cb);
        }
    }
    rendering_end(v);
}

void gfx_shader_draw(struct pass *pass, struct gfx_prog *p, const struct gfx_shader_params *sp,
        const float pos[8], const float local[8]) {
}

void gfx_glass_draw(struct pass *pass, struct gfx_prog *p, struct texture *map,
        const struct gfx_level *glass, struct box grown, struct fbox box, double bend,
        const float pos[8], const float local[8]) {
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

struct gfx_prog *render_shader_create(struct render *r, const char *source, bool *pending) {
    if (pending) *pending = false;
    return NULL;
}

bool render_shader_reap(struct render *r) {
    return false;
}

struct gfx_prog *render_shader_compile(struct render *r, const char *source) {
    return NULL;
}

void render_shader_destroy(struct gfx_prog *program) {
}

int tomoe_warm_shaders(const char *node, const char *directory, const char *marker,
        const char *name) {
    tomoe_log(LOG_ERROR, "tomoe: GFX=vk has no shader warm cache");
    return 1;
}
