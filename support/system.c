#include <dlfcn.h>

struct nvml_utilization { unsigned gpu, memory; };
struct nvml_memory { unsigned version; unsigned long long total, reserved, free, used; };

static int (*count)(unsigned *);
static int (*handle)(unsigned, void **);
static int (*utilization)(void *, struct nvml_utilization *);
static int (*memory)(void *, struct nvml_memory *);
static int (*temperature)(void *, int, unsigned *);

static int nvml_open(void) {
    static int state;
    if (state) return state > 0;
    state = -1;
    void *library = dlopen("libnvidia-ml.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!library) library = dlopen("/run/opengl-driver/lib/libnvidia-ml.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!library) return 0;
    int (*init)(void) = (int (*)(void))dlsym(library, "nvmlInit_v2");
    count = (int (*)(unsigned *))dlsym(library, "nvmlDeviceGetCount_v2");
    handle = (int (*)(unsigned, void **))dlsym(library, "nvmlDeviceGetHandleByIndex_v2");
    utilization = (int (*)(void *, struct nvml_utilization *))dlsym(library, "nvmlDeviceGetUtilizationRates");
    memory = (int (*)(void *, struct nvml_memory *))dlsym(library, "nvmlDeviceGetMemoryInfo_v2");
    temperature = (int (*)(void *, int, unsigned *))dlsym(library, "nvmlDeviceGetTemperature");
    if (!init || !count || !handle || !utilization || !memory || !temperature || init()) {
        dlclose(library);
        return 0;
    }
    state = 1;
    return 1;
}

unsigned tomoe_nvml_count(void) {
    unsigned devices = 0;
    return nvml_open() && !count(&devices) ? devices : 0;
}

int tomoe_nvml_sample(unsigned index, unsigned *busy, unsigned *used, unsigned *total,
        unsigned *celsius) {
    void *device;
    struct nvml_utilization rates;
    struct nvml_memory info = { .version = (unsigned)sizeof(info) | 2u << 24 };
    if (!nvml_open() || handle(index, &device) || utilization(device, &rates) ||
            memory(device, &info) || temperature(device, 0, celsius)) return 0;
    *busy = rates.gpu;
    *used = (unsigned)(info.used >> 20);
    *total = (unsigned)(info.total >> 20);
    return 1;
}
