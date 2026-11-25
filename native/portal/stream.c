#include "portal.h"
#include "pw.h"

#include <drm/drm_fourcc.h>
#include <errno.h>
#include <linux/udmabuf.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

struct portal_vk_bo {
    VkImage image;
    VkDeviceMemory memory;
};

struct portal_vk {
    VkInstance instance;
    VkPhysicalDevice phys;
    VkDevice dev;
    VkPhysicalDeviceMemoryProperties memory;
    PFN_vkGetMemoryFdKHR memory_fd;
    VkFormat format;
    uint64_t mod_values[PWV_MAX_MODIFIERS];
    int mod_planes[PWV_MAX_MODIFIERS];
    size_t mod_count;
};

struct stream {
    struct portal *portal;
    struct session *session;
    uint32_t source_type;
    struct wl_output *output;
    struct ext_foreign_toplevel_handle_v1 *toplevel;
    struct ext_image_capture_source_v1 *source;
    struct ext_image_copy_capture_session_v1 *capture;
    int width;
    int height;
    uint32_t format;
    uint32_t shm_format;
    bool have_shm_format;
    dev_t device;
    bool have_device;
    bool constrained;
    bool stopped;
    bool dying;
    size_t refresh;
    bool dmabuf;
    bool modifier_fixated;
    int alloc;
    int udmabuf;
    struct portal_vk vk;
    uint64_t modifier;
    uint64_t *modifiers;
    size_t modifier_count;
    uint64_t offered_modifiers[PWV_MAX_MODIFIERS];
    size_t offered_modifier_count;
    int planes;
    bool wants_cursor;
    struct ext_image_copy_capture_cursor_session_v1 *cursor;
    struct ext_image_copy_capture_session_v1 *cursor_capture;
    struct ext_image_copy_capture_frame_v1 *cursor_frame;
    struct loop_timer *cursor_retry;
    int cursor_failures;
    struct wl_shm_pool *cursor_pool;
    struct wl_buffer *cursor_buffer;
    void *cursor_map;
    size_t cursor_map_size;
    int bitmap_width;
    int bitmap_height;
    int bitmap_stride;
    uint32_t bitmap_format;
    void *bitmap;
    uint64_t bitmap_generation;
    uint64_t bitmap_sent;
    bool cursor_visible;
    int cursor_x;
    int cursor_y;
    int cursor_hotspot_x;
    int cursor_hotspot_y;
    struct ext_image_copy_capture_frame_v1 *frame;
    struct pwv_buffer *frame_pw;
    struct pwv *pw;
    uint32_t node_id;
    struct slot *slots[SLOTS_MAX];
    int slot_count;
    uint64_t submitted_at;
    int rejections;
    struct loop_timer *retry;
    bool started;
    bool arming;
    bool noted_meta;
    bool renegotiating;
};

static void stream_arm(struct stream *s);
static void stream_republish(struct stream *s);
static void cursor_arm(struct stream *s);
static void stream_fail(struct stream *s, const char *why);

static uint32_t spa_format_of(uint32_t fourcc) {
    switch (fourcc) {
    case DRM_FORMAT_ARGB8888: return SPA_VIDEO_FORMAT_BGRA;
    case DRM_FORMAT_XRGB8888: return SPA_VIDEO_FORMAT_BGRX;
    case DRM_FORMAT_ABGR8888: return SPA_VIDEO_FORMAT_RGBA;
    case DRM_FORMAT_XBGR8888: return SPA_VIDEO_FORMAT_RGBX;
    default: return 0;
    }
}

static uint32_t spa_format_of_shm(uint32_t format) {
    switch (format) {
    case WL_SHM_FORMAT_ARGB8888: return SPA_VIDEO_FORMAT_BGRA;
    case WL_SHM_FORMAT_XRGB8888: return SPA_VIDEO_FORMAT_BGRX;
    case WL_SHM_FORMAT_ABGR8888: return SPA_VIDEO_FORMAT_RGBA;
    case WL_SHM_FORMAT_XBGR8888: return SPA_VIDEO_FORMAT_RGBX;
    default: return 0;
    }
}

static uint32_t drm_format_of_shm(uint32_t format) {
    switch (format) {
    case WL_SHM_FORMAT_ARGB8888: return DRM_FORMAT_ARGB8888;
    case WL_SHM_FORMAT_XRGB8888: return DRM_FORMAT_XRGB8888;
    default: return format;
    }
}

static uint32_t shm_format_of_drm(uint32_t fourcc) {
    switch (fourcc) {
    case DRM_FORMAT_ARGB8888: return WL_SHM_FORMAT_ARGB8888;
    case DRM_FORMAT_XRGB8888: return WL_SHM_FORMAT_XRGB8888;
    default: return fourcc;
    }
}

static uint64_t monotonic_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
}

static const VkImageUsageFlags PORTAL_VK_USAGE =
    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
    VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

static VkFormat vk_format_of(uint32_t fourcc) {
    switch (fourcc) {
    case DRM_FORMAT_ARGB8888:
    case DRM_FORMAT_XRGB8888: return VK_FORMAT_B8G8R8A8_UNORM;
    case DRM_FORMAT_ABGR8888:
    case DRM_FORMAT_XBGR8888: return VK_FORMAT_R8G8B8A8_UNORM;
    default: return VK_FORMAT_UNDEFINED;
    }
}

static bool vk_has_ext(const VkExtensionProperties *list, uint32_t count, const char *name) {
    for (uint32_t i = 0; i < count; i++)
        if (strcmp(list[i].extensionName, name) == 0) return true;
    return false;
}

static void vk_teardown(struct stream *s) {
    if (s->vk.dev != VK_NULL_HANDLE) vkDestroyDevice(s->vk.dev, NULL);
    if (s->vk.instance != VK_NULL_HANDLE) vkDestroyInstance(s->vk.instance, NULL);
    memset(&s->vk, 0, sizeof(s->vk));
}

static void vk_bo_destroy(struct stream *s, struct portal_vk_bo *bo) {
    if (!bo) return;
    if (s->vk.dev != VK_NULL_HANDLE) {
        if (bo->image != VK_NULL_HANDLE) vkDestroyImage(s->vk.dev, bo->image, NULL);
        if (bo->memory != VK_NULL_HANDLE) vkFreeMemory(s->vk.dev, bo->memory, NULL);
    }
    free(bo);
}

static int vk_modifier_planes(const struct stream *s, uint64_t modifier) {
    for (size_t i = 0; i < s->vk.mod_count; i++)
        if (s->vk.mod_values[i] == modifier) return s->vk.mod_planes[i];
    return 0;
}

static bool vk_modifier_exportable(VkPhysicalDevice phys, VkFormat format, uint64_t modifier,
        int width, int height) {
    VkPhysicalDeviceImageDrmFormatModifierInfoEXT mod = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT,
        .drmFormatModifier = modifier,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkPhysicalDeviceExternalImageFormatInfo ext = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
        .pNext = &mod,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    VkPhysicalDeviceImageFormatInfo2 info = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
        .pNext = &ext,
        .format = format,
        .type = VK_IMAGE_TYPE_2D,
        .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
        .usage = PORTAL_VK_USAGE,
    };
    VkExternalImageFormatProperties eprops = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES,
    };
    VkImageFormatProperties2 props = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
        .pNext = &eprops,
    };
    if (vkGetPhysicalDeviceImageFormatProperties2(phys, &info, &props) != VK_SUCCESS)
        return false;
    if (props.imageFormatProperties.maxExtent.width < (uint32_t)width ||
            props.imageFormatProperties.maxExtent.height < (uint32_t)height)
        return false;
    return (eprops.externalMemoryProperties.externalMemoryFeatures &
        VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) != 0;
}

static bool vk_device_matches(VkPhysicalDevice phys, dev_t wanted) {
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(phys, &props);
    if (props.apiVersion < VK_API_VERSION_1_1) return false;
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(phys, NULL, &count, NULL);
    VkExtensionProperties *list = calloc(count ? count : 1, sizeof(*list));
    if (!list) return false;
    vkEnumerateDeviceExtensionProperties(phys, NULL, &count, list);
    bool ok = vk_has_ext(list, count, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME) &&
        vk_has_ext(list, count, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME) &&
        vk_has_ext(list, count, VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME) &&
        vk_has_ext(list, count, VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME);
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
    return (drm.hasRender && (dev_t)makedev(drm.renderMajor, drm.renderMinor) == wanted) ||
        (drm.hasPrimary && (dev_t)makedev(drm.primaryMajor, drm.primaryMinor) == wanted);
}

static bool udmabuf_setup(struct stream *s) {
    if (!s->portal->dmabuf || !s->have_device || !s->modifier_count) return false;
    bool has_linear = false;
    for (size_t i = 0; i < s->modifier_count; i++) {
        if (s->modifiers[i] == DRM_FORMAT_MOD_LINEAR) { has_linear = true; break; }
    }
    if (!has_linear) return false;
    s->udmabuf = open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
    if (s->udmabuf < 0) return false;
    s->modifier = DRM_FORMAT_MOD_LINEAR;
    s->modifier_fixated = false;
    s->offered_modifiers[0] = DRM_FORMAT_MOD_LINEAR;
    s->offered_modifier_count = 1;
    s->planes = 1;
    s->alloc = ALLOC_UDMABUF;
    return true;
}

static bool dmabuf_setup(struct stream *s) {
    if (!s->portal->dmabuf || !s->have_device || !s->modifier_count) return false;
    VkFormat vk_format = vk_format_of(s->format);
    if (vk_format == VK_FORMAT_UNDEFINED) return false;
    dev_t wanted = s->device;
    const char *named = getenv("TOMOE_SCREENCAST_RENDER_NODE");
    if (named && *named) {
        struct stat st;
        if (stat(named, &st) < 0) return false;
        wanted = st.st_rdev;
    }
    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "tomoe-portal",
        .pEngineName = "tomoe",
        .apiVersion = VK_API_VERSION_1_1,
    };
    VkInstanceCreateInfo inst_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app,
    };
    if (vkCreateInstance(&inst_info, NULL, &s->vk.instance) != VK_SUCCESS) {
        s->vk.instance = VK_NULL_HANDLE;
        return false;
    }
    uint32_t dev_count = 0;
    vkEnumeratePhysicalDevices(s->vk.instance, &dev_count, NULL);
    VkPhysicalDevice *devs = calloc(dev_count ? dev_count : 1, sizeof(*devs));
    if (!devs) {
        vk_teardown(s);
        return false;
    }
    vkEnumeratePhysicalDevices(s->vk.instance, &dev_count, devs);
    for (uint32_t i = 0; i < dev_count && s->vk.phys == VK_NULL_HANDLE; i++)
        if (vk_device_matches(devs[i], wanted)) s->vk.phys = devs[i];
    free(devs);
    if (s->vk.phys == VK_NULL_HANDLE) {
        vk_teardown(s);
        return false;
    }
    VkDrmFormatModifierPropertiesListEXT mod_list = {
        .sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT,
    };
    VkFormatProperties2 fprops = {
        .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
        .pNext = &mod_list,
    };
    vkGetPhysicalDeviceFormatProperties2(s->vk.phys, vk_format, &fprops);
    if (!mod_list.drmFormatModifierCount) {
        vk_teardown(s);
        return false;
    }
    VkDrmFormatModifierPropertiesEXT *dev_mods = calloc(mod_list.drmFormatModifierCount,
        sizeof(*dev_mods));
    if (!dev_mods) {
        vk_teardown(s);
        return false;
    }
    mod_list.pDrmFormatModifierProperties = dev_mods;
    vkGetPhysicalDeviceFormatProperties2(s->vk.phys, vk_format, &fprops);

    s->offered_modifier_count = 0;
    s->vk.mod_count = 0;
    int first_planes = 0;
    bool linear_ok = false;
    int linear_planes = 0;
    for (size_t index = 0; index < s->modifier_count; index++) {
        uint64_t modifier = s->modifiers[index];
        if (modifier == DRM_FORMAT_MOD_INVALID) continue;
        bool duplicate = false;
        for (size_t k = 0; k < s->offered_modifier_count; k++)
            if (s->offered_modifiers[k] == modifier) { duplicate = true; break; }
        if (duplicate || (modifier == DRM_FORMAT_MOD_LINEAR && linear_ok)) continue;
        int planes = 0;
        for (uint32_t j = 0; j < mod_list.drmFormatModifierCount; j++) {
            if (dev_mods[j].drmFormatModifier == modifier) {
                planes = (int)dev_mods[j].drmFormatModifierPlaneCount;
                break;
            }
        }
        if (planes < 1 || planes > PLANES_MAX) continue;
        if (!vk_modifier_exportable(s->vk.phys, vk_format, modifier, s->width, s->height)) continue;
        if (modifier == DRM_FORMAT_MOD_LINEAR) {
            linear_ok = true;
            linear_planes = planes;
        } else if (s->offered_modifier_count + 1 < PWV_MAX_MODIFIERS) {
            if (!s->offered_modifier_count) first_planes = planes;
            s->offered_modifiers[s->offered_modifier_count] = modifier;
            s->vk.mod_values[s->vk.mod_count] = modifier;
            s->vk.mod_planes[s->vk.mod_count] = planes;
            s->offered_modifier_count++;
            s->vk.mod_count++;
        }
    }
    free(dev_mods);
    if (linear_ok && s->offered_modifier_count < PWV_MAX_MODIFIERS) {
        if (!s->offered_modifier_count) first_planes = linear_planes;
        s->offered_modifiers[s->offered_modifier_count] = DRM_FORMAT_MOD_LINEAR;
        s->vk.mod_values[s->vk.mod_count] = DRM_FORMAT_MOD_LINEAR;
        s->vk.mod_planes[s->vk.mod_count] = linear_planes;
        s->offered_modifier_count++;
        s->vk.mod_count++;
    }
    if (!s->offered_modifier_count) {
        portal_log("no offered modifier allocates on this device; capturing through memory");
        vk_teardown(s);
        return false;
    }
    uint32_t qcount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(s->vk.phys, &qcount, NULL);
    if (!qcount) {
        vk_teardown(s);
        return false;
    }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qinfo = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = 0,
        .queueCount = 1,
        .pQueuePriorities = &prio,
    };
    const char *exts[] = {
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
        VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
    };
    VkDeviceCreateInfo dev_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &qinfo,
        .enabledExtensionCount = sizeof(exts) / sizeof(exts[0]),
        .ppEnabledExtensionNames = exts,
    };
    if (vkCreateDevice(s->vk.phys, &dev_info, NULL, &s->vk.dev) != VK_SUCCESS) {
        s->vk.dev = VK_NULL_HANDLE;
        vk_teardown(s);
        return false;
    }
    s->vk.memory_fd = (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(s->vk.dev, "vkGetMemoryFdKHR");
    if (!s->vk.memory_fd) {
        vk_teardown(s);
        return false;
    }
    vkGetPhysicalDeviceMemoryProperties(s->vk.phys, &s->vk.memory);
    s->vk.format = vk_format;
    s->modifier = s->offered_modifiers[0];
    s->modifier_fixated = false;
    s->planes = first_planes;
    s->alloc = ALLOC_VK;
    return true;
}

static void slot_free(struct stream *s, struct slot *slot) {
    if (slot->buffer) wl_buffer_destroy(slot->buffer);
    if (slot->vk_bo) vk_bo_destroy(s, slot->vk_bo);
    for (int plane = 0; plane < PLANES_MAX; plane++)
        if (slot->fds[plane] >= 0 && slot->fds[plane] != slot->memfd) close(slot->fds[plane]);
    if (slot->memfd >= 0) close(slot->memfd);
    free(slot);
}

static int vk_memory_type(const struct portal_vk *vk, uint32_t bits) {
    for (uint32_t i = 0; i < vk->memory.memoryTypeCount; i++)
        if ((bits & (1u << i)) &&
                (vk->memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
            return (int)i;
    for (uint32_t i = 0; i < vk->memory.memoryTypeCount; i++)
        if (bits & (1u << i)) return (int)i;
    return -1;
}

static struct slot *slot_create(struct stream *s, bool want_dmabuf) {
    struct slot *slot = calloc(1, sizeof(*slot));
    if (!slot) return NULL;
    slot->memfd = -1;
    for (int plane = 0; plane < PLANES_MAX; plane++) slot->fds[plane] = -1;
    slot->width = s->width;
    slot->height = s->height;
    if (!want_dmabuf) goto memory;
    if (s->alloc == ALLOC_UDMABUF) {
        long page = sysconf(_SC_PAGESIZE);
        size_t stride = (size_t)s->width * 4;
        size_t size = stride * (size_t)s->height;
        size_t mapped = page > 0 ? (size + (size_t)page - 1) / (size_t)page * (size_t)page : size;
        slot->memfd = memfd_create("tomoe-portal-udmabuf", MFD_CLOEXEC | MFD_ALLOW_SEALING);
        if (slot->memfd < 0 || ftruncate(slot->memfd, (off_t)mapped) < 0) goto failed;
        if (fcntl(slot->memfd, F_ADD_SEALS, F_SEAL_SHRINK) < 0) goto failed;
        void *pages = mmap(NULL, mapped, PROT_READ | PROT_WRITE, MAP_SHARED, slot->memfd, 0);
        if (pages == MAP_FAILED) goto failed;
        memset(pages, 0, mapped);
        munmap(pages, mapped);
        struct udmabuf_create create = {
            .memfd = (uint32_t)slot->memfd,
            .flags = UDMABUF_FLAGS_CLOEXEC,
            .offset = 0,
            .size = mapped,
        };
        slot->fds[0] = ioctl(s->udmabuf, UDMABUF_CREATE, &create);
        if (slot->fds[0] < 0) goto failed;
        struct zwp_linux_buffer_params_v1 *params = zwp_linux_dmabuf_v1_create_params(
            s->portal->dmabuf);
        if (!params) goto failed;
        zwp_linux_buffer_params_v1_add(params, slot->fds[0], 0, 0, (int32_t)stride,
            (uint32_t)(s->modifier >> 32), (uint32_t)s->modifier);
        slot->buffer = zwp_linux_buffer_params_v1_create_immed(params, s->width, s->height,
            s->format, 0);
        zwp_linux_buffer_params_v1_destroy(params);
        if (!slot->buffer) goto failed;
        slot->planes = 1;
        slot->offset[0] = 0;
        slot->stride[0] = (uint32_t)stride;
        slot->size[0] = (uint32_t)size;
        slot->kind = SPA_DATA_DMABUF;
        return slot;
    }
    if (s->alloc == ALLOC_VK) {
        int planes = vk_modifier_planes(s, s->modifier);
        if (planes < 1 || planes > PLANES_MAX || s->vk.dev == VK_NULL_HANDLE) goto failed;
        VkImageDrmFormatModifierListCreateInfoEXT mod_list = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT,
            .drmFormatModifierCount = 1,
            .pDrmFormatModifiers = &s->modifier,
        };
        VkExternalMemoryImageCreateInfo ext = {
            .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
            .pNext = &mod_list,
            .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
        };
        VkImageCreateInfo info = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .pNext = &ext,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = s->vk.format,
            .extent = { (uint32_t)s->width, (uint32_t)s->height, 1 },
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
            .usage = PORTAL_VK_USAGE,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };
        VkImage image = VK_NULL_HANDLE;
        if (vkCreateImage(s->vk.dev, &info, NULL, &image) != VK_SUCCESS) goto failed;
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
        vkGetImageMemoryRequirements2(s->vk.dev, &req_info, &req);
        int mtype = vk_memory_type(&s->vk, req.memoryRequirements.memoryTypeBits);
        if (mtype < 0) {
            vkDestroyImage(s->vk.dev, image, NULL);
            goto failed;
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
            .memoryTypeIndex = (uint32_t)mtype,
        };
        VkDeviceMemory memory = VK_NULL_HANDLE;
        if (vkAllocateMemory(s->vk.dev, &alloc, NULL, &memory) != VK_SUCCESS) {
            vkDestroyImage(s->vk.dev, image, NULL);
            goto failed;
        }
        if (vkBindImageMemory(s->vk.dev, image, memory, 0) != VK_SUCCESS) {
            vkFreeMemory(s->vk.dev, memory, NULL);
            vkDestroyImage(s->vk.dev, image, NULL);
            goto failed;
        }
        struct portal_vk_bo *bo = calloc(1, sizeof(*bo));
        if (!bo) {
            vkFreeMemory(s->vk.dev, memory, NULL);
            vkDestroyImage(s->vk.dev, image, NULL);
            goto failed;
        }
        bo->image = image;
        bo->memory = memory;
        slot->vk_bo = bo;
        VkMemoryGetFdInfoKHR fd_info = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
            .memory = memory,
            .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
        };
        int fd0 = -1;
        if (s->vk.memory_fd(s->vk.dev, &fd_info, &fd0) != VK_SUCCESS || fd0 < 0) goto failed;
        fcntl(fd0, F_SETFD, FD_CLOEXEC);
        struct zwp_linux_buffer_params_v1 *params = zwp_linux_dmabuf_v1_create_params(
            s->portal->dmabuf);
        if (!params) {
            close(fd0);
            goto failed;
        }
        for (int plane = 0; plane < planes; plane++) {
            slot->fds[plane] = (plane == 0) ? fd0 : fcntl(fd0, F_DUPFD_CLOEXEC, 0);
            if (slot->fds[plane] < 0) {
                zwp_linux_buffer_params_v1_destroy(params);
                goto failed;
            }
            VkImageSubresource sub = {
                .aspectMask = VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT << plane,
                .mipLevel = 0,
                .arrayLayer = 0,
            };
            VkSubresourceLayout layout = {0};
            vkGetImageSubresourceLayout(s->vk.dev, image, &sub, &layout);
            if (layout.offset > UINT32_MAX || layout.rowPitch == 0 || layout.rowPitch > UINT32_MAX) {
                zwp_linux_buffer_params_v1_destroy(params);
                goto failed;
            }
            slot->offset[plane] = (uint32_t)layout.offset;
            slot->stride[plane] = (uint32_t)layout.rowPitch;
            slot->size[plane] = (layout.size > 0 && layout.size <= UINT32_MAX)
                ? (uint32_t)layout.size : slot->stride[plane] * (uint32_t)s->height;
            zwp_linux_buffer_params_v1_add(params, slot->fds[plane], (uint32_t)plane,
                slot->offset[plane], slot->stride[plane],
                (uint32_t)(s->modifier >> 32), (uint32_t)s->modifier);
            slot->planes = plane + 1;
        }
        slot->buffer = zwp_linux_buffer_params_v1_create_immed(params, s->width, s->height,
            s->format, 0);
        zwp_linux_buffer_params_v1_destroy(params);
        if (!slot->buffer) goto failed;
        slot->kind = SPA_DATA_DMABUF;
        return slot;
    }
memory:
    size_t stride = (size_t)s->width * 4;
    size_t size = stride * (size_t)s->height;
    slot->memfd = memfd_create("tomoe-portal-pwbuf", MFD_CLOEXEC);
    if (slot->memfd < 0 || ftruncate(slot->memfd, (off_t)size) < 0) goto failed;
    struct wl_shm_pool *pool = wl_shm_create_pool(s->portal->shm, slot->memfd, (int32_t)size);
    if (!pool) goto failed;
    slot->buffer = wl_shm_pool_create_buffer(pool, 0, s->width, s->height, (int32_t)stride,
        s->shm_format);
    wl_shm_pool_destroy(pool);
    if (!slot->buffer) goto failed;
    slot->planes = 1;
    slot->fds[0] = slot->memfd;
    slot->stride[0] = (uint32_t)stride;
    slot->size[0] = (uint32_t)size;
    slot->kind = SPA_DATA_MEMFD;
    return slot;

failed:
    slot_free(s, slot);
    return NULL;
}

static bool slot_matches(struct stream *s, const struct slot *slot) {
    return slot->buffer && slot->width == s->width && slot->height == s->height;
}

static void write_cursor_meta(struct stream *s, struct pwv_buffer *buffer) {
    if (!buffer || !buffer->buffer) return;
    struct spa_meta *meta = spa_buffer_find_meta(buffer->buffer, SPA_META_CURSOR);
    if (!meta || !meta->data || meta->size < sizeof(struct spa_meta_cursor)) {
        if (!s->noted_meta) {
            portal_log("the PipeWire buffer carries no cursor metadata");
            s->noted_meta = true;
        }
        return;
    }
    struct spa_meta_cursor *cursor = meta->data;
    memset(cursor, 0, sizeof(*cursor));
    cursor->id = s->cursor_visible ? (uint32_t)(s->bitmap_generation ? s->bitmap_generation : 1)
                                  : 0;
    cursor->position = (struct spa_point){ .x = s->cursor_x, .y = s->cursor_y };
    cursor->hotspot = (struct spa_point){ .x = s->cursor_hotspot_x, .y = s->cursor_hotspot_y };
    if (!s->cursor_visible || !s->bitmap || s->bitmap_generation == s->bitmap_sent) return;
    size_t cursor_size = sizeof(struct spa_meta_cursor);
    size_t bitmap_size = sizeof(struct spa_meta_bitmap);
    size_t data_size = (size_t)s->bitmap_stride * (size_t)s->bitmap_height;
    uint32_t format = spa_format_of_shm(s->bitmap_format);
    if (!data_size || !format || cursor_size + bitmap_size + data_size > meta->size) return;
    struct spa_meta_bitmap *bitmap = (struct spa_meta_bitmap *)((uint8_t *)cursor + cursor_size);
    memcpy((uint8_t *)cursor + cursor_size + bitmap_size, s->bitmap, data_size);
    *bitmap = (struct spa_meta_bitmap){
        .format = format,
        .size = { .width = (uint32_t)s->bitmap_width, .height = (uint32_t)s->bitmap_height },
        .stride = s->bitmap_stride,
        .offset = (uint32_t)bitmap_size,
    };
    cursor->bitmap_offset = (uint32_t)cursor_size;
    s->bitmap_sent = s->bitmap_generation;
}

static void frame_ready(void *data, struct ext_image_copy_capture_frame_v1 *frame) {
    struct stream *s = data;
    if (s->dying) return;
    s->rejections = 0;
    s->submitted_at = 0;
    struct pwv_buffer *buffer = s->frame_pw;
    if (buffer && s->pw) {
        write_cursor_meta(s, buffer);
        pwv_queue(s->pw, buffer);
    }
    s->frame_pw = NULL;
    if (s->frame) ext_image_copy_capture_frame_v1_destroy(s->frame);
    s->frame = NULL;
    stream_arm(s);
}

static void frame_retry(void *data, uint64_t expirations) {
    struct stream *s = data;
    loop_del_timer(s->portal->loop, s->retry);
    s->retry = NULL;
    stream_arm(s);
}

static void frame_failed(void *data, struct ext_image_copy_capture_frame_v1 *frame,
        uint32_t reason) {
    struct stream *s = data;
    if (s->dying) return;
    if (s->frame) ext_image_copy_capture_frame_v1_destroy(s->frame);
    s->frame = NULL;
    if (s->frame_pw && s->pw) pwv_queue(s->pw, s->frame_pw);
    s->frame_pw = NULL;
    uint64_t elapsed = s->submitted_at ? monotonic_ms() - s->submitted_at : 0;
    s->submitted_at = 0;
    if (elapsed < 100) {
        s->rejections++;
    } else {
        s->rejections = 0;
    }
    if (s->rejections >= 3) {
        s->rejections = 0;
        if (s->alloc != ALLOC_SHM) {
            portal_log("the compositor keeps refusing our buffers; capturing through memory");
            s->alloc = ALLOC_SHM;
            s->dmabuf = false;
            s->modifier_fixated = false;
            s->renegotiating = true;
            if (s->pw) stream_republish(s);
            return;
        }
        stream_fail(s, "the compositor keeps refusing capture buffers");
        return;
    }
    if (!s->retry) {
        s->retry = loop_add_timer(s->portal->loop, frame_retry, s);
        if (s->retry) {
            struct timespec retry = { .tv_sec = 0, .tv_nsec = FRAME_RETRY_MS * 1000000L };
            loop_update_timer(s->portal->loop, s->retry, &retry, NULL, false);
        }
    }
}

static void frame_transform(void *data, struct ext_image_copy_capture_frame_v1 *frame,
        uint32_t transform) {
}

static void frame_damage(void *data, struct ext_image_copy_capture_frame_v1 *frame, int32_t x,
        int32_t y, int32_t width, int32_t height) {
}

static void frame_presentation(void *data, struct ext_image_copy_capture_frame_v1 *frame,
        uint32_t tv_sec_hi, uint32_t tv_sec_lo, uint32_t tv_nsec) {
}

static const struct ext_image_copy_capture_frame_v1_listener frame_listener = {
    .transform = frame_transform,
    .damage = frame_damage,
    .presentation_time = frame_presentation,
    .ready = frame_ready,
    .failed = frame_failed,
};

static void stream_arm(struct stream *s) {
    if (!s->pw || !s->constrained || s->dying || s->frame || s->stopped) return;
    if (s->renegotiating) return;
    if (s->slot_count == 0) return;
    struct pwv_buffer *buffer = pwv_dequeue(s->pw);
    if (!buffer) return;
    struct slot *slot = buffer->user_data;
    if (!slot || !slot_matches(s, slot)) {
        pwv_queue(s->pw, buffer);
        return;
    }
    struct ext_image_copy_capture_frame_v1 *frame =
        ext_image_copy_capture_session_v1_create_frame(s->capture);
    if (!frame) {
        pwv_queue(s->pw, buffer);
        return;
    }
    ext_image_copy_capture_frame_v1_add_listener(frame, &frame_listener, s);
    s->frame = frame;
    s->frame_pw = buffer;
    ext_image_copy_capture_frame_v1_attach_buffer(frame, slot->buffer);
    ext_image_copy_capture_frame_v1_capture(frame);
    s->submitted_at = monotonic_ms();
    wl_display_flush(s->portal->display);
}

static void stream_state(void *data, int state, const char *error) {
    struct stream *s = data;
    if (s->dying) return;
    if (state == PWV_BOUND) {
        s->node_id = pwv_node_id(s->pw);
        if (!s->started) {
            s->started = true;
            portal_start_reply(s->session, 0, s->node_id, (uint32_t)s->width, (uint32_t)s->height);
        }
    } else if (state == PWV_FAILED) {
        stream_fail(s, error ? error : "the PipeWire stream disconnected");
    }
}

static void stream_republish(struct stream *s);

static void stream_format(void *data, const struct pwv_video *video) {
    struct stream *s = data;
    if (s->dying) return;
    if (!video) {
        s->dmabuf = false;
        s->modifier_fixated = false;
        return;
    }
    bool dmabuf = video->modifier_set && (s->vk.dev != VK_NULL_HANDLE || s->udmabuf >= 0);
    s->dmabuf = dmabuf;
    if (!dmabuf) {
        s->modifier_fixated = false;
        return;
    }
    s->modifier = video->modifier;
    bool republish = !video->modifier_fixated && !s->modifier_fixated;
    if (s->alloc == ALLOC_VK && s->vk.dev != VK_NULL_HANDLE) {
        int planes = vk_modifier_planes(s, s->modifier);
        if (planes >= 1 && planes <= PLANES_MAX && planes != s->planes) {
            s->planes = planes;
            republish = true;
        }
    }
    if (republish) {
        s->modifier_fixated = true;
        stream_republish(s);
    }
}

static void stream_process(void *data) {
    stream_arm(data);
}

static void arm_deferred(void *data) {
    stream_arm(data);
}

static bool stream_add_buffer(void *data, struct pwv_buffer *buffer) {
    struct stream *s = data;
    if (s->dying || s->slot_count >= SLOTS_MAX || !buffer->buffer) return false;
    uint32_t negotiated = buffer->buffer->n_datas ? buffer->buffer->datas[0].type : 0;
    bool want_dmabuf = s->dmabuf && s->alloc != ALLOC_SHM && (negotiated & (1u << SPA_DATA_DMABUF));
    struct slot *slot = slot_create(s, want_dmabuf);
    if (!slot) {
        if (s->alloc != ALLOC_SHM) {
            portal_log("cannot allocate a DMA-BUF capture buffer; capturing through memory");
            s->alloc = ALLOC_SHM;
            s->dmabuf = false;
            s->modifier_fixated = false;
            s->renegotiating = true;
            stream_republish(s);
        }
        return false;
    }
    if ((int)buffer->buffer->n_datas < slot->planes) {
        slot_free(s, slot);
        return false;
    }
    for (int plane = 0; plane < slot->planes; plane++) {
        struct spa_data *spa = &buffer->buffer->datas[plane];
        spa->type = (uint32_t)slot->kind;
        spa->flags = SPA_DATA_FLAG_READWRITE;
        spa->fd = slot->fds[plane];
        spa->mapoffset = 0;
        spa->maxsize = slot->size[plane];
        spa->data = NULL;
        *spa->chunk = (struct spa_chunk){
            .offset = slot->offset[plane],
            .size = slot->size[plane],
            .stride = (int32_t)slot->stride[plane],
        };
    }
    buffer->user_data = slot;
    s->slots[s->slot_count++] = slot;
    s->dmabuf = slot->kind == SPA_DATA_DMABUF;
    s->renegotiating = false;
    portal_defer(s->portal, arm_deferred, s);
    return true;
}

static void stream_remove_buffer(void *data, struct pwv_buffer *buffer) {
    struct stream *s = data;
    struct slot *slot = buffer->user_data;
    buffer->user_data = NULL;
    if (!slot) return;
    for (int index = 0; index < s->slot_count; index++)
        if (s->slots[index] == slot) {
            memmove(&s->slots[index], &s->slots[index + 1],
                (size_t)(s->slot_count - index - 1) * sizeof(s->slots[0]));
            s->slot_count--;
            break;
        }
    if (s->frame_pw == buffer) {
        if (s->frame) ext_image_copy_capture_frame_v1_destroy(s->frame);
        s->frame = NULL;
        s->frame_pw = NULL;
    }
    slot_free(s, slot);
}

static const struct pwv_events stream_events = {
    .state = stream_state,
    .format = stream_format,
    .add_buffer = stream_add_buffer,
    .remove_buffer = stream_remove_buffer,
    .process = stream_process,
};

static void fill_pwv_video(const struct stream *s, struct pwv_video *video) {
    *video = (struct pwv_video){
        .format = s->format,
        .modifier = s->modifier,
        .modifier_set = s->dmabuf,
        .modifier_fixated = s->modifier_fixated,
        .modifier_count = s->alloc == ALLOC_SHM ? 0 : s->offered_modifier_count,
        .width = s->width,
        .height = s->height,
        .planes = s->planes,
        .refresh = (uint32_t)(s->refresh ? s->refresh : 60),
    };
    if (video->modifier_count > PWV_MAX_MODIFIERS) video->modifier_count = PWV_MAX_MODIFIERS;
    for (size_t i = 0; i < video->modifier_count; i++)
        video->modifiers[i] = s->offered_modifiers[i];
}

static void stream_republish(struct stream *s) {
    struct pwv_video video;
    fill_pwv_video(s, &video);
    pwv_publish(s->pw, &video, s->alloc == ALLOC_SHM ? PWV_ALLOC_MEMORY : PWV_ALLOC_DMABUF);
}

static int stream_connect(struct stream *s) {
    struct pwv_video video;
    fill_pwv_video(s, &video);
    const char *name = s->source_type == SOURCE_WINDOW ? "tomoe-portal-toplevel-stream"
                                                      : "tomoe-portal-stream";
    s->pw = pwv_start(s->portal->loop, name, "tomoe portal screencast", &video,
        s->alloc == ALLOC_SHM ? PWV_ALLOC_MEMORY : PWV_ALLOC_DMABUF, s->wants_cursor,
        &stream_events, s);
    return s->pw ? 0 : -1;
}

static void cursor_ensure_buffer(struct stream *s) {
    if (!s->portal->shm || s->bitmap_width <= 0 || s->bitmap_height <= 0) return;
    size_t stride = (size_t)s->bitmap_width * 4;
    size_t size = stride * (size_t)s->bitmap_height;
    if (s->cursor_buffer && s->cursor_map_size == size) return;
    if (s->cursor_buffer) {
        wl_buffer_destroy(s->cursor_buffer);
        s->cursor_buffer = NULL;
    }
    if (s->cursor_pool) {
        wl_shm_pool_destroy(s->cursor_pool);
        s->cursor_pool = NULL;
    }
    if (s->cursor_map) {
        munmap(s->cursor_map, s->cursor_map_size);
        s->cursor_map = NULL;
    }
    s->cursor_map_size = 0;
    int fd = memfd_create("tomoe-portal-cursor", MFD_CLOEXEC);
    if (fd < 0) return;
    if (ftruncate(fd, (off_t)size) < 0) {
        close(fd);
        return;
    }
    void *map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        close(fd);
        return;
    }
    s->cursor_pool = wl_shm_create_pool(s->portal->shm, fd, (int32_t)size);
    close(fd);
    if (!s->cursor_pool) {
        munmap(map, size);
        return;
    }
    s->cursor_buffer = wl_shm_pool_create_buffer(s->cursor_pool, 0, s->bitmap_width,
        s->bitmap_height, (int32_t)stride, s->bitmap_format);
    if (!s->cursor_buffer) {
        wl_shm_pool_destroy(s->cursor_pool);
        s->cursor_pool = NULL;
        munmap(map, size);
        return;
    }
    s->cursor_map = map;
    s->cursor_map_size = size;
    s->bitmap_stride = (int)stride;
}

static void cursor_frame_ready(void *data, struct ext_image_copy_capture_frame_v1 *frame) {
    struct stream *s = data;
    if (s->dying) return;
    s->cursor_failures = 0;
    if (s->cursor_map && s->bitmap_width > 0 && s->bitmap_height > 0) {
        size_t size = (size_t)s->bitmap_stride * (size_t)s->bitmap_height;
        uint8_t *pixels = malloc(size);
        if (pixels) {
            memcpy(pixels, s->cursor_map, size);
            free(s->bitmap);
            s->bitmap = pixels;
            s->bitmap_generation++;
            portal_log("cursor bitmap %dx%d", s->bitmap_width, s->bitmap_height);
        }
    }
    if (s->cursor_frame) ext_image_copy_capture_frame_v1_destroy(s->cursor_frame);
    s->cursor_frame = NULL;
    cursor_arm(s);
}

static void cursor_retry(void *data, uint64_t expirations) {
    struct stream *s = data;
    loop_del_timer(s->portal->loop, s->cursor_retry);
    s->cursor_retry = NULL;
    cursor_arm(s);
}

static void cursor_frame_failed(void *data, struct ext_image_copy_capture_frame_v1 *frame,
        uint32_t reason) {
    struct stream *s = data;
    if (s->cursor_frame) ext_image_copy_capture_frame_v1_destroy(s->cursor_frame);
    s->cursor_frame = NULL;
    if (++s->cursor_failures <= 3)
        portal_log("cursor frame failed with reason %u", reason);
    if (!s->cursor_retry) {
        s->cursor_retry = loop_add_timer(s->portal->loop, cursor_retry, s);
        if (s->cursor_retry) {
            struct timespec retry = { .tv_sec = 0, .tv_nsec = 100000000L };
            loop_update_timer(s->portal->loop, s->cursor_retry, &retry, NULL, false);
        }
    }
}

static const struct ext_image_copy_capture_frame_v1_listener cursor_frame_listener = {
    .transform = frame_transform,
    .damage = frame_damage,
    .presentation_time = frame_presentation,
    .ready = cursor_frame_ready,
    .failed = cursor_frame_failed,
};

static void cursor_arm(struct stream *s) {
    if (!s->cursor_capture || s->cursor_frame || s->dying || s->stopped) return;
    if (s->bitmap_width <= 0 || s->bitmap_height <= 0) return;
    cursor_ensure_buffer(s);
    if (!s->cursor_buffer) {
        portal_log("no cursor buffer for %dx%d", s->bitmap_width, s->bitmap_height);
        return;
    }
    struct ext_image_copy_capture_frame_v1 *frame =
        ext_image_copy_capture_session_v1_create_frame(s->cursor_capture);
    if (!frame) return;
    ext_image_copy_capture_frame_v1_add_listener(frame, &cursor_frame_listener, s);
    s->cursor_frame = frame;
    ext_image_copy_capture_frame_v1_attach_buffer(frame, s->cursor_buffer);
    ext_image_copy_capture_frame_v1_capture(frame);
    wl_display_flush(s->portal->display);
}

static void cursor_buffer_size(void *data, struct ext_image_copy_capture_session_v1 *session,
        uint32_t width, uint32_t height) {
    struct stream *s = data;
    if (width <= 0 || height <= 0 || width > CURSOR_EDGE || height > CURSOR_EDGE) return;
    if ((int)width != s->bitmap_width || (int)height != s->bitmap_height) {
        if (s->cursor_frame) {
            ext_image_copy_capture_frame_v1_destroy(s->cursor_frame);
            s->cursor_frame = NULL;
        }
    }
    s->bitmap_width = (int)width;
    s->bitmap_height = (int)height;
}

static void cursor_shm_format(void *data, struct ext_image_copy_capture_session_v1 *session,
        uint32_t format) {
    struct stream *s = data;
    s->bitmap_format = format;
}

static void cursor_dmabuf_device(void *data, struct ext_image_copy_capture_session_v1 *session,
        struct wl_array *device) {
}

static void cursor_dmabuf_format(void *data, struct ext_image_copy_capture_session_v1 *session,
        uint32_t format, struct wl_array *modifiers) {
    portal_log("cursor session offers format 0x%x with %zu modifiers", format,
        modifiers->size / sizeof(uint64_t));
}

static void cursor_constraints_done(void *data, struct ext_image_copy_capture_session_v1 *session) {
    struct stream *s = data;
    portal_log("cursor session %dx%d format 0x%x", s->bitmap_width, s->bitmap_height,
        s->bitmap_format);
    cursor_arm(s);
}

static void cursor_constraints_stopped(void *data,
        struct ext_image_copy_capture_session_v1 *session) {
}

static const struct ext_image_copy_capture_session_v1_listener cursor_session_listener = {
    .buffer_size = cursor_buffer_size,
    .shm_format = cursor_shm_format,
    .dmabuf_device = cursor_dmabuf_device,
    .dmabuf_format = cursor_dmabuf_format,
    .done = cursor_constraints_done,
    .stopped = cursor_constraints_stopped,
};

static void cursor_enter(void *data, struct ext_image_copy_capture_cursor_session_v1 *cursor) {
    struct stream *s = data;
    s->cursor_visible = true;
}

static void cursor_leave(void *data, struct ext_image_copy_capture_cursor_session_v1 *cursor) {
    struct stream *s = data;
    s->cursor_visible = false;
}

static void cursor_position(void *data, struct ext_image_copy_capture_cursor_session_v1 *cursor,
        int32_t x, int32_t y) {
    struct stream *s = data;
    s->cursor_x = x;
    s->cursor_y = y;
}

static void cursor_hotspot(void *data, struct ext_image_copy_capture_cursor_session_v1 *cursor,
        int32_t x, int32_t y) {
    struct stream *s = data;
    s->cursor_hotspot_x = x;
    s->cursor_hotspot_y = y;
}

static const struct ext_image_copy_capture_cursor_session_v1_listener cursor_listener = {
    .enter = cursor_enter,
    .leave = cursor_leave,
    .position = cursor_position,
    .hotspot = cursor_hotspot,
};

static void capture_buffer_size(void *data, struct ext_image_copy_capture_session_v1 *session,
        uint32_t width, uint32_t height) {
    struct stream *s = data;
    if (width <= 0 || height <= 0) return;
    if (s->constrained && ((int)width != s->width || (int)height != s->height)) {
        portal_log("the source changed size to %dx%d", width, height);
        s->width = (int)width;
        s->height = (int)height;
        s->modifier_fixated = false;
        s->renegotiating = true;
        if (s->pw) stream_republish(s);
        return;
    }
    s->width = (int)width;
    s->height = (int)height;
}

static void capture_shm_format(void *data, struct ext_image_copy_capture_session_v1 *session,
        uint32_t format) {
    struct stream *s = data;
    if (format == WL_SHM_FORMAT_ARGB8888 || format == WL_SHM_FORMAT_XRGB8888 ||
            format == WL_SHM_FORMAT_ABGR8888 || format == WL_SHM_FORMAT_XBGR8888) {
        s->shm_format = format;
        s->have_shm_format = true;
    } else if (!s->have_shm_format) {
        s->shm_format = format;
        s->have_shm_format = true;
    }
}

static void capture_dmabuf_device(void *data, struct ext_image_copy_capture_session_v1 *session,
        struct wl_array *device) {
    struct stream *s = data;
    if (device->size < sizeof(dev_t)) return;
    memcpy(&s->device, device->data, sizeof(dev_t));
    s->have_device = true;
}

static void capture_dmabuf_format(void *data, struct ext_image_copy_capture_session_v1 *session,
        uint32_t format, struct wl_array *modifiers) {
    struct stream *s = data;
    free(s->modifiers);
    s->modifier_count = modifiers->size / sizeof(uint64_t);
    s->modifiers = NULL;
    s->format = format;
    if (!s->modifier_count) return;
    s->modifiers = malloc(modifiers->size);
    if (!s->modifiers) {
        s->modifier_count = 0;
        return;
    }
    memcpy(s->modifiers, modifiers->data, modifiers->size);
}

static void capture_done(void *data, struct ext_image_copy_capture_session_v1 *session) {
    struct stream *s = data;
    s->constrained = true;
}

static void capture_stopped(void *data, struct ext_image_copy_capture_session_v1 *session) {
    struct stream *s = data;
    s->stopped = true;
    if (!s->arming) stream_fail(s, "the compositor stopped the capture");
}

static const struct ext_image_copy_capture_session_v1_listener capture_listener = {
    .buffer_size = capture_buffer_size,
    .shm_format = capture_shm_format,
    .dmabuf_device = capture_dmabuf_device,
    .dmabuf_format = capture_dmabuf_format,
    .done = capture_done,
    .stopped = capture_stopped,
};

static void stream_fail_deferred(void *data) {
    struct stream *s = data;
    if (s->session->start_call) {
        s->session->stream = NULL;
        portal_start_reply(s->session, 2, 0, 0, 0);
        stream_free(s);
        return;
    }
    portal_defer_close(s->session, true);
}

static void stream_fail(struct stream *s, const char *why) {
    if (s->dying) return;
    s->dying = true;
    portal_log("%s", why);
    if (s->arming) return;
    portal_defer(s->portal, stream_fail_deferred, s);
}

void stream_free(struct stream *s) {
    if (!s) return;
    s->dying = true;
    struct portal *p = s->portal;
    struct pwv *pw = s->pw;
    s->pw = NULL;
    if (s->retry) loop_del_timer(p->loop, s->retry);
    if (s->cursor_retry) loop_del_timer(p->loop, s->cursor_retry);
    if (pw) pwv_stop(pw);
    for (int index = 0; index < s->slot_count; index++)
        if (s->slots[index]) slot_free(s, s->slots[index]);
    s->slot_count = 0;
    if (s->frame) ext_image_copy_capture_frame_v1_destroy(s->frame);
    if (s->cursor_frame) ext_image_copy_capture_frame_v1_destroy(s->cursor_frame);
    if (s->cursor_capture) ext_image_copy_capture_session_v1_destroy(s->cursor_capture);
    if (s->cursor) ext_image_copy_capture_cursor_session_v1_destroy(s->cursor);
    if (s->capture) ext_image_copy_capture_session_v1_destroy(s->capture);
    if (s->source) ext_image_capture_source_v1_destroy(s->source);
    if (s->cursor_buffer) wl_buffer_destroy(s->cursor_buffer);
    if (s->cursor_pool) wl_shm_pool_destroy(s->cursor_pool);
    if (s->cursor_map) munmap(s->cursor_map, s->cursor_map_size);
    vk_teardown(s);
    if (s->udmabuf >= 0) close(s->udmabuf);
    free(s->bitmap);
    free(s->modifiers);
    free(s);
}

struct stream *stream_start(struct session *session) {
    struct portal *p = session->portal;
    if (!p->capture || !p->shm) {
        portal_log("the compositor cannot be captured from");
        return NULL;
    }
    struct stream *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->portal = p;
    s->session = session;
    s->arming = true;
    s->source_type = session->choice.type;
    s->udmabuf = -1;
    s->wants_cursor = session->cursor_mode == CURSOR_METADATA;
    if (s->source_type == SOURCE_MONITOR) {
        struct output_info *info = portal_output_by_name(p, session->choice.output);
        if (!info) {
            portal_log("holding an output that is no longer connected");
            free(s);
            return NULL;
        }
        s->output = info->output;
        s->refresh = info->refresh_mhz > 0 ? (size_t)((info->refresh_mhz + 500) / 1000) : 60;
        s->source = ext_output_image_capture_source_manager_v1_create_source(p->output_sources,
            s->output);
    } else {
        struct toplevel_info *info = portal_toplevel_by_identifier(p, session->choice.identifier);
        if (!info) {
            portal_log("holding a window that is no longer open");
            free(s);
            return NULL;
        }
        s->toplevel = info->handle;
        s->refresh = 60;
        if (!p->window_sources) {
            portal_log("the compositor cannot capture windows");
            free(s);
            return NULL;
        }
        s->source = ext_foreign_toplevel_image_capture_source_manager_v1_create_source(
            p->window_sources, s->toplevel);
    }
    if (!s->source) {
        free(s);
        return NULL;
    }
    uint32_t options = session->cursor_mode == CURSOR_EMBEDDED
        ? EXT_IMAGE_COPY_CAPTURE_MANAGER_V1_OPTIONS_PAINT_CURSORS : 0;
    s->capture = ext_image_copy_capture_manager_v1_create_session(p->capture, s->source,
        options);
    if (!s->capture) {
        stream_free(s);
        return NULL;
    }
    ext_image_copy_capture_session_v1_add_listener(s->capture, &capture_listener, s);
    for (int attempt = 0; attempt < 3 && !s->constrained && !s->stopped; attempt++)
        wl_display_roundtrip(p->display);
    if (!s->constrained || s->stopped || s->width <= 0 || s->height <= 0) {
        portal_log("the compositor never advertised capture constraints");
        stream_free(s);
        return NULL;
    }
    if (!spa_format_of(s->format) && s->have_shm_format)
        s->format = drm_format_of_shm(s->shm_format);
    if (!spa_format_of(s->format)) {
        portal_log("the compositor offers no streamable pixel format");
        stream_free(s);
        return NULL;
    }
    if (!s->have_shm_format) {
        s->shm_format = shm_format_of_drm(s->format);
        s->have_shm_format = true;
    }
    if (!dmabuf_setup(s) && !udmabuf_setup(s)) s->alloc = ALLOC_SHM;
    s->dmabuf = s->alloc != ALLOC_SHM;
    if (s->dmabuf)
        portal_log("capturing %dx%d through DMA-BUF (%s) with modifier 0x%llx", s->width,
            s->height, s->alloc == ALLOC_VK ? "Vulkan" : "udmabuf",
            (unsigned long long)s->modifier);
    else
        portal_log("capturing %dx%d through memory", s->width, s->height);
    if (s->wants_cursor && s->output) {
        s->cursor = ext_image_copy_capture_manager_v1_create_pointer_cursor_session(p->capture,
            s->source, p->pointer);
        if (s->cursor) {
            ext_image_copy_capture_cursor_session_v1_add_listener(s->cursor, &cursor_listener, s);
            s->cursor_capture = ext_image_copy_capture_cursor_session_v1_get_capture_session(
                s->cursor);
            if (s->cursor_capture)
                ext_image_copy_capture_session_v1_add_listener(s->cursor_capture,
                    &cursor_session_listener, s);
        }
        for (int attempt = 0; attempt < 3 && !s->bitmap_width; attempt++)
            wl_display_roundtrip(p->display);
    }
    if (stream_connect(s) < 0 || s->dying) {
        if (!s->dying) portal_log("cannot open the PipeWire stream");
        stream_free(s);
        return NULL;
    }
    s->arming = false;
    return s;
}
