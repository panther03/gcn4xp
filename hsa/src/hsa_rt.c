/*
 * hsa_rt.c - HSA runtime core for gcn4xp: initialisation, agents, memory
 * regions, signals, queues and the AQL packet processor.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hsa_impl.h"
#include "gcn_regs.h"

int g_hsa_trace;

void hsa_trace(const char *fmt, ...)
{
    va_list ap;

    if (!g_hsa_trace)
        return;
    va_start(ap, fmt);
    fputs("hsa: ", stderr);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

void hsa_complain(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    fputs("hsa: ", stderr);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

/* ---- status strings ---------------------------------------------------- */

hsa_status_t hsa_status_string(hsa_status_t status, const char **s)
{
    static const struct {
        hsa_status_t st;
        const char *text;
    } tab[] = {
        { HSA_STATUS_SUCCESS, "HSA_STATUS_SUCCESS: The function has been "
                              "executed successfully." },
        { HSA_STATUS_INFO_BREAK, "HSA_STATUS_INFO_BREAK: A traversal over a "
                                 "list of elements has been interrupted." },
        { HSA_STATUS_ERROR, "HSA_STATUS_ERROR: A generic error has occurred." },
        { HSA_STATUS_ERROR_INVALID_ARGUMENT, "HSA_STATUS_ERROR_INVALID_ARGUMENT:"
          " One of the actual arguments does not meet a precondition." },
        { HSA_STATUS_ERROR_INVALID_QUEUE_CREATION,
          "HSA_STATUS_ERROR_INVALID_QUEUE_CREATION: The requested queue cannot "
          "be created." },
        { HSA_STATUS_ERROR_INVALID_ALLOCATION,
          "HSA_STATUS_ERROR_INVALID_ALLOCATION: The allocation is invalid." },
        { HSA_STATUS_ERROR_INVALID_AGENT,
          "HSA_STATUS_ERROR_INVALID_AGENT: The agent is invalid." },
        { HSA_STATUS_ERROR_INVALID_REGION,
          "HSA_STATUS_ERROR_INVALID_REGION: The memory region is invalid." },
        { HSA_STATUS_ERROR_INVALID_SIGNAL,
          "HSA_STATUS_ERROR_INVALID_SIGNAL: The signal is invalid." },
        { HSA_STATUS_ERROR_INVALID_QUEUE,
          "HSA_STATUS_ERROR_INVALID_QUEUE: The queue is invalid." },
        { HSA_STATUS_ERROR_OUT_OF_RESOURCES,
          "HSA_STATUS_ERROR_OUT_OF_RESOURCES: The runtime failed to allocate "
          "the necessary resources." },
        { HSA_STATUS_ERROR_INVALID_PACKET_FORMAT,
          "HSA_STATUS_ERROR_INVALID_PACKET_FORMAT: The AQL packet is "
          "malformed." },
        { HSA_STATUS_ERROR_RESOURCE_FREE,
          "HSA_STATUS_ERROR_RESOURCE_FREE: An error has been detected while "
          "releasing a resource." },
        { HSA_STATUS_ERROR_NOT_INITIALIZED,
          "HSA_STATUS_ERROR_NOT_INITIALIZED: The runtime has not been "
          "initialized." },
        { HSA_STATUS_ERROR_REFCOUNT_OVERFLOW,
          "HSA_STATUS_ERROR_REFCOUNT_OVERFLOW: The runtime reference count "
          "has overflowed." },
        { HSA_STATUS_ERROR_INCOMPATIBLE_ARGUMENTS,
          "HSA_STATUS_ERROR_INCOMPATIBLE_ARGUMENTS: The arguments are not "
          "compatible." },
        { HSA_STATUS_ERROR_INVALID_INDEX,
          "HSA_STATUS_ERROR_INVALID_INDEX: The index is invalid." },
        { HSA_STATUS_ERROR_INVALID_ISA,
          "HSA_STATUS_ERROR_INVALID_ISA: The instruction set architecture is "
          "invalid." },
        { HSA_STATUS_ERROR_INVALID_ISA_NAME,
          "HSA_STATUS_ERROR_INVALID_ISA_NAME: The instruction set "
          "architecture name is invalid." },
        { HSA_STATUS_ERROR_INVALID_CODE_OBJECT,
          "HSA_STATUS_ERROR_INVALID_CODE_OBJECT: The code object is invalid." },
        { HSA_STATUS_ERROR_INVALID_EXECUTABLE,
          "HSA_STATUS_ERROR_INVALID_EXECUTABLE: The executable is invalid." },
        { HSA_STATUS_ERROR_FROZEN_EXECUTABLE,
          "HSA_STATUS_ERROR_FROZEN_EXECUTABLE: The executable is frozen." },
        { HSA_STATUS_ERROR_INVALID_SYMBOL_NAME,
          "HSA_STATUS_ERROR_INVALID_SYMBOL_NAME: There is no symbol with the "
          "given name." },
        { HSA_STATUS_ERROR_VARIABLE_ALREADY_DEFINED,
          "HSA_STATUS_ERROR_VARIABLE_ALREADY_DEFINED: The variable is already "
          "defined." },
        { HSA_STATUS_ERROR_VARIABLE_UNDEFINED,
          "HSA_STATUS_ERROR_VARIABLE_UNDEFINED: The variable is undefined." },
        { HSA_STATUS_ERROR_EXCEPTION,
          "HSA_STATUS_ERROR_EXCEPTION: An HSAIL operation resulted in a "
          "hardware exception." },
        { HSA_STATUS_ERROR_INVALID_CODE_SYMBOL,
          "HSA_STATUS_ERROR_INVALID_CODE_SYMBOL: The code object symbol is "
          "invalid." },
        { HSA_STATUS_ERROR_INVALID_EXECUTABLE_SYMBOL,
          "HSA_STATUS_ERROR_INVALID_EXECUTABLE_SYMBOL: The executable symbol "
          "is invalid." },
        { HSA_STATUS_ERROR_INVALID_FILE,
          "HSA_STATUS_ERROR_INVALID_FILE: The file descriptor is invalid." },
        { HSA_STATUS_ERROR_INVALID_CODE_OBJECT_READER,
          "HSA_STATUS_ERROR_INVALID_CODE_OBJECT_READER: The code object "
          "reader is invalid." },
        { HSA_STATUS_ERROR_INVALID_RUNTIME_STATE,
          "HSA_STATUS_ERROR_INVALID_RUNTIME_STATE: The runtime is in an "
          "invalid state." },
        { HSA_STATUS_ERROR_FATAL,
          "HSA_STATUS_ERROR_FATAL: A fatal error has occurred." },
    };
    size_t i;

    if (s == NULL)
        return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    for (i = 0; i < sizeof(tab) / sizeof(tab[0]); i++) {
        if (tab[i].st == status) {
            *s = tab[i].text;
            return HSA_STATUS_SUCCESS;
        }
    }
    return HSA_STATUS_ERROR_INVALID_ARGUMENT;
}

/* ---- heap -------------------------------------------------------------- */

/* Blocks in address order; free blocks are merged with free neighbours. */
typedef struct heap_block {
    struct heap_block *next;
    u8 *addr;
    size_t size;
    int used;
} heap_block;

static heap_block *g_heap;
static u8 *g_heap_base;
static size_t g_heap_size;

static void heap_init(u8 *base, size_t size)
{
    g_heap = (heap_block *)calloc(1, sizeof(*g_heap));
    g_heap->addr = base;
    g_heap->size = size;
    g_heap_base = base;
    g_heap_size = size;
}

static void heap_fini(void)
{
    while (g_heap != NULL) {
        heap_block *n = g_heap->next;

        free(g_heap);
        g_heap = n;
    }
}

void *hsa_heap_alloc(size_t size, size_t align)
{
    heap_block *b;

    if (size == 0)
        size = 1;
    if (align < 256)
        align = 256;
    size = (size + 255) & ~(size_t)255;
    for (b = g_heap; b != NULL; b = b->next) {
        size_t pad;

        if (b->used)
            continue;
        pad = (align - ((size_t)b->addr & (align - 1))) & (align - 1);
        if (b->size < pad + size)
            continue;
        if (pad != 0) {     /* split off the alignment padding */
            heap_block *n = (heap_block *)calloc(1, sizeof(*n));

            n->addr = b->addr + pad;
            n->size = b->size - pad;
            n->next = b->next;
            b->size = pad;
            b->next = n;
            b = n;
        }
        if (b->size > size) {
            heap_block *n = (heap_block *)calloc(1, sizeof(*n));

            n->addr = b->addr + size;
            n->size = b->size - size;
            n->next = b->next;
            b->size = size;
            b->next = n;
        }
        b->used = 1;
        return b->addr;
    }
    return NULL;
}

int hsa_heap_owns(const void *p)
{
    return g_heap_base != NULL && (const u8 *)p >= g_heap_base &&
           (const u8 *)p < g_heap_base + g_heap_size;
}

int hsa_heap_free(void *p)
{
    heap_block *b, *prev = NULL;

    for (b = g_heap; b != NULL; prev = b, b = b->next) {
        if (b->addr != p)
            continue;
        if (!b->used)
            return -1;
        b->used = 0;
        if (b->next != NULL && !b->next->used) {
            heap_block *n = b->next;

            b->size += n->size;
            b->next = n->next;
            free(n);
        }
        if (prev != NULL && !prev->used) {
            prev->size += b->size;
            prev->next = b->next;
            free(b);
        }
        return 0;
    }
    return -1;
}

static size_t heap_largest_free(void)
{
    heap_block *b;
    size_t best = 0;

    for (b = g_heap; b != NULL; b = b->next)
        if (!b->used && b->size > best)
            best = b->size;
    return best;
}

/* ---- initialisation ---------------------------------------------------- */

typedef struct {
    hsa_device_type_t type;
    const char *name;
} agent_t;

typedef struct {
    hsa_region_segment_t segment;
    u32 flags;
    int host_accessible;
} region_t;

static agent_t g_cpu = { HSA_DEVICE_TYPE_CPU, "gcn4xp host CPU" };
static agent_t g_gpu = { HSA_DEVICE_TYPE_GPU, HSA_AGENT_NAME };
static region_t g_fine = { HSA_REGION_SEGMENT_GLOBAL,
                           HSA_REGION_GLOBAL_FLAG_KERNARG |
                           HSA_REGION_GLOBAL_FLAG_FINE_GRAINED, 1 };
static region_t g_coarse = { HSA_REGION_SEGMENT_GLOBAL,
                             HSA_REGION_GLOBAL_FLAG_COARSE_GRAINED, 1 };
static region_t g_group = { HSA_REGION_SEGMENT_GROUP, 0, 0 };

static int g_refcount;
static u8 *g_map;               /* CPU view of the compute region */

/* indirect buffers, reused in turn; each remembers the work it carried */
#define SLOTS      512
#define SLOT_BYTES 512
typedef struct {
    u8 *mem;                    /* IB, then a copy of the AQL packet */
    u32 seq;
} slot_t;
static slot_t g_slot[SLOTS];
static u32 g_slot_next;

static void progress(void);

int hsa_rt_ready(void)
{
    return g_refcount > 0;
}

static const char *firmware_dir(void)
{
    static char dir[MAX_PATH];
    const char *env = getenv("GCN4XP_FIRMWARE");
    char *slash;

    if (env != NULL && env[0] != 0)
        return env;
    /* next to the program, then next to the program's parent directory */
    GetModuleFileNameA(NULL, dir, sizeof(dir) - 32);
    slash = strrchr(dir, '\\');
    if (slash != NULL)
        *slash = 0;
    strcat(dir, "\\firmware");
    if (GetFileAttributesA(dir) == INVALID_FILE_ATTRIBUTES) {
        *strrchr(dir, '\\') = 0;
        slash = strrchr(dir, '\\');
        if (slash != NULL) {
            strcpy(slash, "\\firmware");
        }
    }
    return dir;
}

hsa_status_t hsa_init(void)
{
    const GCN_INFO *info;
    u64 region;
    u32 len, i;

    if (g_refcount > 0) {
        g_refcount++;
        return HSA_STATUS_SUCCESS;
    }
    g_hsa_trace = getenv("GCN4XP_TRACE") != NULL &&
                  atoi(getenv("GCN4XP_TRACE")) != 0;
    if (gcn_dev_open() != 0) {
        hsa_complain("%s\n", gcn_dev_error());
        return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
    }
    if (gcn_dev_lock(10000) != 0) {
        hsa_complain("%s\n", gcn_dev_error());
        gcn_dev_close();
        return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
    }
    info = gcn_dev_info();
    region = gcn_region_off();
    len = (u32)(info->FbApertureLen - region);
    g_map = (u8 *)gcn_dev_map(region, len);
    if (g_map == NULL) {
        hsa_complain("%s (the driver must be a debug build of version 0.1.1 "
                     "or later)\n", gcn_dev_error());
        goto fail;
    }
    if (gcn_gpu_init(firmware_dir(), 0, g_hsa_trace ? stderr : NULL) != 0) {
        hsa_complain("cannot start the GPU: %s\n", gcn_gpu_error());
        goto fail;
    }
    gcn_vm_map((u64)(ULONG_PTR)(g_map + GCN_HEAP_OFF),
               (u32)(gcn_heap_bytes() / GCN_GPU_PAGE), 0);
    heap_init(g_map + GCN_HEAP_OFF, (size_t)gcn_heap_bytes());
    for (i = 0; i < SLOTS; i++) {
        g_slot[i].mem = (u8 *)hsa_heap_alloc(SLOT_BYTES, 256);
        g_slot[i].seq = 0;
    }
    g_slot_next = 0;
    hsa_trace("compute region mapped at %p, %u MB of heap\n", (void *)g_map,
              (unsigned)(gcn_heap_bytes() >> 20));
    g_refcount = 1;
    return HSA_STATUS_SUCCESS;

fail:
    if (g_map != NULL)
        gcn_dev_unmap(g_map);
    g_map = NULL;
    gcn_dev_unlock();
    gcn_dev_close();
    return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
}

static void queues_destroy_all(void);

hsa_status_t hsa_shut_down(void)
{
    if (g_refcount == 0)
        return HSA_STATUS_ERROR_NOT_INITIALIZED;
    if (--g_refcount > 0)
        return HSA_STATUS_SUCCESS;
    progress();
    queues_destroy_all();
    heap_fini();
    gcn_dev_unmap(g_map);
    g_map = NULL;
    gcn_dev_unlock();
    gcn_dev_close();
    return HSA_STATUS_SUCCESS;
}

#define CHECK_INIT()                                                         \
    do {                                                                     \
        if (g_refcount == 0)                                                 \
            return HSA_STATUS_ERROR_NOT_INITIALIZED;                         \
    } while (0)

/* ---- system ------------------------------------------------------------ */

static u64 timestamp(void)
{
    LARGE_INTEGER t;

    QueryPerformanceCounter(&t);
    return (u64)t.QuadPart;
}

static u64 timestamp_freq(void)
{
    LARGE_INTEGER f;

    QueryPerformanceFrequency(&f);
    return (u64)f.QuadPart;
}

hsa_status_t hsa_system_get_info(hsa_system_info_t attribute, void *value)
{
    CHECK_INIT();
    if (value == NULL)
        return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    switch (attribute) {
    case HSA_SYSTEM_INFO_VERSION_MAJOR:
        *(uint16_t *)value = 1;
        break;
    case HSA_SYSTEM_INFO_VERSION_MINOR:
        *(uint16_t *)value = 2;
        break;
    case HSA_SYSTEM_INFO_TIMESTAMP:
        *(uint64_t *)value = timestamp();
        break;
    case HSA_SYSTEM_INFO_TIMESTAMP_FREQUENCY:
        *(uint64_t *)value = timestamp_freq();
        break;
    case HSA_SYSTEM_INFO_SIGNAL_MAX_WAIT:
        *(uint64_t *)value = ~0ull;
        break;
    case HSA_SYSTEM_INFO_ENDIANNESS:
        *(hsa_endianness_t *)value = HSA_ENDIANNESS_LITTLE;
        break;
    case HSA_SYSTEM_INFO_MACHINE_MODEL:
#ifdef HSA_LARGE_MODEL
        *(hsa_machine_model_t *)value = HSA_MACHINE_MODEL_LARGE;
#else
        *(hsa_machine_model_t *)value = HSA_MACHINE_MODEL_SMALL;
#endif
        break;
    case HSA_SYSTEM_INFO_EXTENSIONS:
        memset(value, 0, 128);
        break;
    default:
        return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    }
    return HSA_STATUS_SUCCESS;
}

hsa_status_t hsa_system_extension_supported(uint16_t extension,
                                            uint16_t version_major,
                                            uint16_t version_minor,
                                            bool *result)
{
    (void)extension;
    (void)version_major;
    (void)version_minor;
    CHECK_INIT();
    if (result == NULL)
        return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    *result = false;
    return HSA_STATUS_SUCCESS;
}

hsa_status_t hsa_extension_get_name(uint16_t extension, const char **name)
{
    static const char *const names[] = { "HSA_EXTENSION_FINALIZER",
                                         "HSA_EXTENSION_IMAGES",
                                         "HSA_EXTENSION_PERFORMANCE_COUNTERS",
                                         "HSA_EXTENSION_PROFILING_EVENTS" };

    if (name == NULL || extension > HSA_EXTENSION_STD_LAST)
        return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    *name = names[extension];
    return HSA_STATUS_SUCCESS;
}

hsa_status_t hsa_agent_extension_supported(uint16_t extension,
                                           hsa_agent_t agent,
                                           uint16_t version_major,
                                           uint16_t version_minor,
                                           bool *result)
{
    (void)agent;
    return hsa_system_extension_supported(extension, version_major,
                                          version_minor, result);
}

/* ---- agents ------------------------------------------------------------ */

static agent_t *agent_of(hsa_agent_t a)
{
    agent_t *p = (agent_t *)(ULONG_PTR)a.handle;

    return p == &g_cpu || p == &g_gpu ? p : NULL;
}

hsa_status_t hsa_iterate_agents(hsa_status_t (*cb)(hsa_agent_t, void *),
                                void *data)
{
    hsa_agent_t a;
    hsa_status_t st;

    CHECK_INIT();
    if (cb == NULL)
        return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    a.handle = (u64)(ULONG_PTR)&g_cpu;
    st = cb(a, data);
    if (st != HSA_STATUS_SUCCESS)
        return st;
    a.handle = (u64)(ULONG_PTR)&g_gpu;
    return cb(a, data);
}

static const char *product_name(void)
{
    switch (gcn_dev_info()->DeviceId) {
    case 0x67df:
        return "Radeon RX 470/480/570/580";
    case 0x67ef:
        return "Radeon RX 460/560";
    case 0x67ff:
        return "Radeon RX 550/560";
    default:
        return "Radeon (Polaris)";
    }
}

hsa_status_t hsa_agent_get_info(hsa_agent_t agent, hsa_agent_info_t attribute,
                                void *value)
{
    agent_t *a;
    int gpu;

    CHECK_INIT();
    a = agent_of(agent);
    if (a == NULL)
        return HSA_STATUS_ERROR_INVALID_AGENT;
    if (value == NULL)
        return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    gpu = a == &g_gpu;
    switch ((int)attribute) {
    case HSA_AGENT_INFO_NAME:
        memset(value, 0, 64);
        strncpy((char *)value, a->name, 63);
        break;
    case HSA_AGENT_INFO_VENDOR_NAME:
        memset(value, 0, 64);
        strcpy((char *)value, gpu ? "AMD" : "CPU");
        break;
    case HSA_AGENT_INFO_FEATURE:
        *(hsa_agent_feature_t *)value =
            gpu ? HSA_AGENT_FEATURE_KERNEL_DISPATCH : (hsa_agent_feature_t)0;
        break;
    case HSA_AGENT_INFO_MACHINE_MODEL:
        return hsa_system_get_info(HSA_SYSTEM_INFO_MACHINE_MODEL, value);
    case HSA_AGENT_INFO_PROFILE:
        *(hsa_profile_t *)value = HSA_PROFILE_BASE;
        break;
    case HSA_AGENT_INFO_DEFAULT_FLOAT_ROUNDING_MODE:
        *(hsa_default_float_rounding_mode_t *)value =
            HSA_DEFAULT_FLOAT_ROUNDING_MODE_NEAR;
        break;
    case HSA_AGENT_INFO_BASE_PROFILE_DEFAULT_FLOAT_ROUNDING_MODES:
        *(hsa_default_float_rounding_mode_t *)value =
            HSA_DEFAULT_FLOAT_ROUNDING_MODE_NEAR;
        break;
    case HSA_AGENT_INFO_FAST_F16_OPERATION:
        *(bool *)value = gpu;
        break;
    case HSA_AGENT_INFO_WAVEFRONT_SIZE:
        *(uint32_t *)value = gpu ? 64 : 0;
        break;
    case HSA_AGENT_INFO_WORKGROUP_MAX_DIM: {
        uint16_t *d = (uint16_t *)value;

        d[0] = d[1] = d[2] = gpu ? 1024 : 0;
        break;
    }
    case HSA_AGENT_INFO_WORKGROUP_MAX_SIZE:
        *(uint32_t *)value = gpu ? 1024 : 0;
        break;
    case HSA_AGENT_INFO_GRID_MAX_DIM: {
        hsa_dim3_t *d = (hsa_dim3_t *)value;

        d->x = d->y = d->z = gpu ? 0xffffffffu : 0;
        break;
    }
    case HSA_AGENT_INFO_GRID_MAX_SIZE:
        *(uint32_t *)value = gpu ? 0xffffffffu : 0;
        break;
    case HSA_AGENT_INFO_FBARRIER_MAX_SIZE:
        *(uint32_t *)value = gpu ? 32 : 0;
        break;
    case HSA_AGENT_INFO_QUEUES_MAX:
        *(uint32_t *)value = gpu ? 16 : 0;
        break;
    case HSA_AGENT_INFO_QUEUE_MIN_SIZE:
        *(uint32_t *)value = gpu ? 64 : 0;
        break;
    case HSA_AGENT_INFO_QUEUE_MAX_SIZE:
        *(uint32_t *)value = gpu ? 4096 : 0;
        break;
    case HSA_AGENT_INFO_QUEUE_TYPE:
        *(hsa_queue_type32_t *)value = HSA_QUEUE_TYPE_MULTI;
        break;
    case HSA_AGENT_INFO_NODE:
        *(uint32_t *)value = gpu ? 1 : 0;
        break;
    case HSA_AGENT_INFO_DEVICE:
        *(hsa_device_type_t *)value = a->type;
        break;
    case HSA_AGENT_INFO_CACHE_SIZE: {
        uint32_t *c = (uint32_t *)value;

        c[0] = gpu ? 16 * 1024 : 0;
        c[1] = gpu ? 2048 * 1024 : 0;
        c[2] = c[3] = 0;
        break;
    }
    case HSA_AGENT_INFO_ISA:
        if (!gpu)
            return HSA_STATUS_ERROR_INVALID_AGENT;
        ((hsa_isa_t *)value)->handle = 1;
        break;
    case HSA_AGENT_INFO_EXTENSIONS:
        memset(value, 0, 128);
        break;
    case HSA_AGENT_INFO_VERSION_MAJOR:
        *(uint16_t *)value = 1;
        break;
    case HSA_AGENT_INFO_VERSION_MINOR:
        *(uint16_t *)value = 2;
        break;
    case HSA_AMD_AGENT_INFO_CHIP_ID:
        *(uint32_t *)value = gpu ? gcn_dev_info()->DeviceId : 0;
        break;
    case HSA_AMD_AGENT_INFO_CACHELINE_SIZE:
        *(uint32_t *)value = 64;
        break;
    case HSA_AMD_AGENT_INFO_PRODUCT_NAME:
        memset(value, 0, 64);
        strcpy((char *)value, gpu ? product_name() : a->name);
        break;
    case HSA_AMD_AGENT_INFO_MAX_WAVES_PER_CU:
        *(uint32_t *)value = gpu ? 40 : 0;
        break;
    case HSA_AMD_AGENT_INFO_NUM_SIMDS_PER_CU:
        *(uint32_t *)value = gpu ? 4 : 0;
        break;
    default:
        return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    }
    return HSA_STATUS_SUCCESS;
}

/* ---- ISA --------------------------------------------------------------- */

hsa_status_t hsa_isa_from_name(const char *name, hsa_isa_t *isa)
{
    CHECK_INIT();
    if (name == NULL || isa == NULL)
        return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    if (strcmp(name, HSA_ISA_NAME) != 0)
        return HSA_STATUS_ERROR_INVALID_ISA_NAME;
    isa->handle = 1;
    return HSA_STATUS_SUCCESS;
}

hsa_status_t hsa_agent_iterate_isas(hsa_agent_t agent,
                                    hsa_status_t (*cb)(hsa_isa_t, void *),
                                    void *data)
{
    hsa_isa_t isa;

    CHECK_INIT();
    if (agent_of(agent) != &g_gpu)
        return HSA_STATUS_ERROR_INVALID_AGENT;
    if (cb == NULL)
        return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    isa.handle = 1;
    return cb(isa, data);
}

hsa_status_t hsa_isa_get_info_alt(hsa_isa_t isa, hsa_isa_info_t attribute,
                                  void *value)
{
    CHECK_INIT();
    if (isa.handle != 1)
        return HSA_STATUS_ERROR_INVALID_ISA;
    if (value == NULL)
        return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    switch (attribute) {
    case HSA_ISA_INFO_NAME_LENGTH:
        *(uint32_t *)value = (uint32_t)strlen(HSA_ISA_NAME);
        break;
    case HSA_ISA_INFO_NAME:
        strcpy((char *)value, HSA_ISA_NAME);
        break;
    case HSA_ISA_INFO_MACHINE_MODELS: {
        bool *m = (bool *)value;

        m[HSA_MACHINE_MODEL_SMALL] = true;
        m[HSA_MACHINE_MODEL_LARGE] = true;
        break;
    }
    case HSA_ISA_INFO_PROFILES: {
        bool *p = (bool *)value;

        p[HSA_PROFILE_BASE] = true;
        p[HSA_PROFILE_FULL] = false;
        break;
    }
    case HSA_ISA_INFO_FAST_F16_OPERATION:
        *(bool *)value = true;
        break;
    case HSA_ISA_INFO_WORKGROUP_MAX_SIZE:
        *(uint32_t *)value = 1024;
        break;
    default:
        return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    }
    return HSA_STATUS_SUCCESS;
}

/* ---- regions and memory ------------------------------------------------ */

static region_t *region_of(hsa_region_t r)
{
    region_t *p = (region_t *)(ULONG_PTR)r.handle;

    return p == &g_fine || p == &g_coarse || p == &g_group ? p : NULL;
}

static hsa_status_t visit_region(region_t *r,
                                 hsa_status_t (*cb)(hsa_region_t, void *),
                                 void *data)
{
    hsa_region_t h;

    h.handle = (u64)(ULONG_PTR)r;
    return cb(h, data);
}

hsa_status_t hsa_agent_iterate_regions(hsa_agent_t agent,
                                       hsa_status_t (*cb)(hsa_region_t,
                                                          void *),
                                       void *data)
{
    agent_t *a;
    hsa_status_t st;

    CHECK_INIT();
    a = agent_of(agent);
    if (a == NULL)
        return HSA_STATUS_ERROR_INVALID_AGENT;
    if (cb == NULL)
        return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    st = visit_region(&g_fine, cb, data);
    if (st != HSA_STATUS_SUCCESS || a != &g_gpu)
        return st;
    st = visit_region(&g_coarse, cb, data);
    if (st != HSA_STATUS_SUCCESS)
        return st;
    return visit_region(&g_group, cb, data);
}

hsa_status_t hsa_region_get_info(hsa_region_t region,
                                 hsa_region_info_t attribute, void *value)
{
    region_t *r;
    int global;

    CHECK_INIT();
    r = region_of(region);
    if (r == NULL)
        return HSA_STATUS_ERROR_INVALID_REGION;
    if (value == NULL)
        return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    global = r->segment == HSA_REGION_SEGMENT_GLOBAL;
    switch ((int)attribute) {
    case HSA_REGION_INFO_SEGMENT:
        *(hsa_region_segment_t *)value = r->segment;
        break;
    case HSA_REGION_INFO_GLOBAL_FLAGS:
        *(uint32_t *)value = r->flags;
        break;
    case HSA_REGION_INFO_SIZE:
        *(size_t *)value = global ? g_heap_size : 65536;
        break;
    case HSA_REGION_INFO_ALLOC_MAX_SIZE:
        *(size_t *)value = global ? heap_largest_free() : 0;
        break;
    case HSA_REGION_INFO_ALLOC_MAX_PRIVATE_WORKGROUP_SIZE:
        *(uint32_t *)value = 0;
        break;
    case HSA_REGION_INFO_RUNTIME_ALLOC_ALLOWED:
        *(bool *)value = global;
        break;
    case HSA_REGION_INFO_RUNTIME_ALLOC_GRANULE:
    case HSA_REGION_INFO_RUNTIME_ALLOC_ALIGNMENT:
        *(size_t *)value = global ? 4096 : 0;
        break;
    case HSA_AMD_REGION_INFO_HOST_ACCESSIBLE:
        *(bool *)value = r->host_accessible != 0;
        break;
    case HSA_AMD_REGION_INFO_BASE:
        *(void **)value = global ? (void *)g_heap_base : NULL;
        break;
    default:
        return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    }
    return HSA_STATUS_SUCCESS;
}

hsa_status_t hsa_memory_allocate(hsa_region_t region, size_t size, void **ptr)
{
    region_t *r;
    void *p;

    CHECK_INIT();
    r = region_of(region);
    if (r == NULL)
        return HSA_STATUS_ERROR_INVALID_REGION;
    if (ptr == NULL || size == 0)
        return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    if (r->segment != HSA_REGION_SEGMENT_GLOBAL)
        return HSA_STATUS_ERROR_INVALID_ALLOCATION;
    p = hsa_heap_alloc(size, size >= 4096 ? 4096 : 256);
    if (p == NULL)
        return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
    /*
     * Kernel arguments start out zeroed: a 32-bit program that stores a
     * pointer as 4 bytes then still hands the kernel a valid 64-bit one.
     */
    if (r->flags & HSA_REGION_GLOBAL_FLAG_KERNARG)
        memset(p, 0, size);
    *ptr = p;
    return HSA_STATUS_SUCCESS;
}

hsa_status_t hsa_memory_free(void *ptr)
{
    CHECK_INIT();
    if (ptr == NULL)
        return HSA_STATUS_SUCCESS;
    return hsa_heap_free(ptr) == 0 ? HSA_STATUS_SUCCESS
                                   : HSA_STATUS_ERROR_INVALID_ARGUMENT;
}

hsa_status_t hsa_memory_copy(void *dst, const void *src, size_t size)
{
    CHECK_INIT();
    if (dst == NULL || src == NULL)
        return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    if (hsa_heap_owns(src))
        gcn_dev_invalidate_reads();
    memmove(dst, src, size);
    if (hsa_heap_owns(dst))
        gcn_dev_flush_writes();
    return HSA_STATUS_SUCCESS;
}

hsa_status_t hsa_memory_assign_agent(void *ptr, hsa_agent_t agent,
                                     hsa_access_permission_t access)
{
    (void)access;
    CHECK_INIT();
    if (ptr == NULL)
        return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    if (agent_of(agent) == NULL)
        return HSA_STATUS_ERROR_INVALID_AGENT;
    return HSA_STATUS_SUCCESS;
}

hsa_status_t hsa_memory_register(void *ptr, size_t size)
{
    (void)ptr;
    (void)size;
    CHECK_INIT();
    return HSA_STATUS_SUCCESS;
}

hsa_status_t hsa_memory_deregister(void *ptr, size_t size)
{
    (void)ptr;
    (void)size;
    CHECK_INIT();
    return HSA_STATUS_SUCCESS;
}

/* ---- signals ----------------------------------------------------------- */

#define SIGNAL_MAGIC 0x5347494eu

struct queue_s;

typedef struct {
    volatile long long value;
    u32 magic;
    struct queue_s *doorbell_of;    /* doorbell of this queue, if any */
} signal_t;

static signal_t *signal_of(hsa_signal_t s)
{
    signal_t *p = (signal_t *)(ULONG_PTR)s.handle;

    return p != NULL && p->magic == SIGNAL_MAGIC ? p : NULL;
}

static signal_t *signal_new(long long v)
{
    signal_t *s = (signal_t *)calloc(1, sizeof(*s));

    if (s != NULL) {
        s->value = v;
        s->magic = SIGNAL_MAGIC;
    }
    return s;
}

hsa_status_t hsa_signal_create(hsa_signal_value_t initial_value,
                               uint32_t num_consumers,
                               const hsa_agent_t *consumers,
                               hsa_signal_t *signal)
{
    signal_t *s;

    (void)num_consumers;
    (void)consumers;
    CHECK_INIT();
    if (signal == NULL)
        return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    s = signal_new(initial_value);
    if (s == NULL)
        return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
    signal->handle = (u64)(ULONG_PTR)s;
    return HSA_STATUS_SUCCESS;
}

hsa_status_t hsa_signal_destroy(hsa_signal_t signal)
{
    signal_t *s = signal_of(signal);

    CHECK_INIT();
    if (s == NULL || s->doorbell_of != NULL)
        return HSA_STATUS_ERROR_INVALID_SIGNAL;
    s->magic = 0;
    free(s);
    return HSA_STATUS_SUCCESS;
}

static void doorbell_rung(signal_t *s);

static long long sig_load(hsa_signal_t h)
{
    signal_t *s = signal_of(h);

    if (s == NULL)
        return 0;
    progress();
    __sync_synchronize();
    return s->value;
}

static void sig_store(hsa_signal_t h, long long v)
{
    signal_t *s = signal_of(h);

    if (s == NULL)
        return;
    __sync_synchronize();
    s->value = v;
    __sync_synchronize();
    if (s->doorbell_of != NULL)
        doorbell_rung(s);
}

hsa_signal_value_t hsa_signal_load_scacquire(hsa_signal_t s)
{
    return (hsa_signal_value_t)sig_load(s);
}
hsa_signal_value_t hsa_signal_load_relaxed(hsa_signal_t s)
{
    return (hsa_signal_value_t)sig_load(s);
}
hsa_signal_value_t hsa_signal_load_acquire(hsa_signal_t s)
{
    return (hsa_signal_value_t)sig_load(s);
}
void hsa_signal_store_relaxed(hsa_signal_t s, hsa_signal_value_t v)
{
    sig_store(s, v);
}
void hsa_signal_store_screlease(hsa_signal_t s, hsa_signal_value_t v)
{
    sig_store(s, v);
}
void hsa_signal_store_release(hsa_signal_t s, hsa_signal_value_t v)
{
    sig_store(s, v);
}
void hsa_signal_silent_store_relaxed(hsa_signal_t s, hsa_signal_value_t v)
{
    sig_store(s, v);
}
void hsa_signal_silent_store_screlease(hsa_signal_t s, hsa_signal_value_t v)
{
    sig_store(s, v);
}

/* read-modify-write: one operation, every memory-order spelling of it */
static long long sig_rmw(hsa_signal_t h, int op, long long v, long long cmp)
{
    signal_t *s = signal_of(h);
    long long old;

    if (s == NULL)
        return 0;
    switch (op) {
    case 0:
        old = __sync_lock_test_and_set(&s->value, v);
        __sync_synchronize();
        break;
    case 1:
        old = __sync_val_compare_and_swap(&s->value, cmp, v);
        break;
    case 2:
        old = __sync_fetch_and_add(&s->value, v);
        break;
    case 3:
        old = __sync_fetch_and_sub(&s->value, v);
        break;
    case 4:
        old = __sync_fetch_and_and(&s->value, v);
        break;
    case 5:
        old = __sync_fetch_and_or(&s->value, v);
        break;
    default:
        old = __sync_fetch_and_xor(&s->value, v);
        break;
    }
    if (s->doorbell_of != NULL)
        doorbell_rung(s);
    return old;
}

#define RMW_RET(name, op)                                                     \
    hsa_signal_value_t hsa_signal_##name(hsa_signal_t s,                     \
                                         hsa_signal_value_t v)               \
    {                                                                         \
        return (hsa_signal_value_t)sig_rmw(s, op, v, 0);                      \
    }
#define RMW_VOID(name, op)                                                    \
    void hsa_signal_##name(hsa_signal_t s, hsa_signal_value_t v)             \
    {                                                                         \
        sig_rmw(s, op, v, 0);                                                 \
    }
#define RMW_ALL(mac, op, base)                                                \
    mac(base##_scacq_screl, op) mac(base##_scacquire, op)                     \
    mac(base##_relaxed, op) mac(base##_screlease, op)                         \
    mac(base##_acq_rel, op) mac(base##_acquire, op) mac(base##_release, op)

RMW_ALL(RMW_RET, 0, exchange)
RMW_ALL(RMW_VOID, 2, add)
RMW_ALL(RMW_VOID, 3, subtract)
RMW_ALL(RMW_VOID, 4, and)
RMW_ALL(RMW_VOID, 5, or)
RMW_ALL(RMW_VOID, 6, xor)

#define CAS(name)                                                             \
    hsa_signal_value_t hsa_signal_##name(hsa_signal_t s,                     \
                                         hsa_signal_value_t e,               \
                                         hsa_signal_value_t v)               \
    {                                                                         \
        return (hsa_signal_value_t)sig_rmw(s, 1, v, e);                       \
    }
CAS(cas_scacq_screl)
CAS(cas_scacquire)
CAS(cas_relaxed)
CAS(cas_screlease)
CAS(cas_acq_rel)

static int cond_met(long long v, hsa_signal_condition_t c, long long cmp)
{
    switch (c) {
    case HSA_SIGNAL_CONDITION_EQ:
        return v == cmp;
    case HSA_SIGNAL_CONDITION_NE:
        return v != cmp;
    case HSA_SIGNAL_CONDITION_LT:
        return v < cmp;
    default:
        return v >= cmp;
    }
}

static hsa_signal_value_t sig_wait(hsa_signal_t h, hsa_signal_condition_t c,
                                   hsa_signal_value_t cmp, uint64_t timeout,
                                   hsa_wait_state_t state)
{
    signal_t *s = signal_of(h);
    u64 start = timestamp();
    u32 spins = 0;

    if (s == NULL)
        return 0;
    for (;;) {
        long long v;

        progress();
        __sync_synchronize();
        v = s->value;
        if (cond_met(v, c, cmp))
            return (hsa_signal_value_t)v;
        if (timeout != ~0ull && timestamp() - start >= timeout)
            return (hsa_signal_value_t)v;
        /* each poll costs a few register accesses; back off a little */
        if (++spins > 64 || state == HSA_WAIT_STATE_BLOCKED)
            Sleep(spins > 1000 ? 1 : 0);
    }
}

hsa_signal_value_t hsa_signal_wait_scacquire(hsa_signal_t s,
                                             hsa_signal_condition_t c,
                                             hsa_signal_value_t v,
                                             uint64_t t, hsa_wait_state_t w)
{
    return sig_wait(s, c, v, t, w);
}
hsa_signal_value_t hsa_signal_wait_relaxed(hsa_signal_t s,
                                           hsa_signal_condition_t c,
                                           hsa_signal_value_t v, uint64_t t,
                                           hsa_wait_state_t w)
{
    return sig_wait(s, c, v, t, w);
}
hsa_signal_value_t hsa_signal_wait_acquire(hsa_signal_t s,
                                           hsa_signal_condition_t c,
                                           hsa_signal_value_t v, uint64_t t,
                                           hsa_wait_state_t w)
{
    return sig_wait(s, c, v, t, w);
}

/* ---- queues ------------------------------------------------------------ */

/* GPU work whose completion still has to be reported */
typedef struct pending {
    struct pending *next;
    u32 seq;
    signal_t *signal;
} pending_t;

typedef struct queue_s {
    hsa_queue_t pub;                /* first: hsa_queue_t * is queue_t * */
    volatile u64 write_index;
    volatile u64 read_index;
    struct queue_s *next;
    hsa_queue_t *gpu_copy;          /* what the queue pointer SGPRs see */
    u32 last_seq;                   /* last submission from this queue */
    int busy;                       /* processor running (no recursion) */
    int broken;                     /* a packet failed; stop processing */
    void (*callback)(hsa_status_t, hsa_queue_t *, void *);
    void *cb_data;
} queue_t;

/* malloc with 64-byte alignment; the original pointer sits just below */
static void *_aligned_malloc_compat(size_t size)
{
    u8 *raw = (u8 *)malloc(size + 64 + sizeof(void *)), *p;

    if (raw == NULL)
        return NULL;
    p = (u8 *)(((ULONG_PTR)raw + sizeof(void *) + 63) & ~(ULONG_PTR)63);
    ((void **)p)[-1] = raw;
    return p;
}

static void free_aligned_compat(void *p)
{
    if (p != NULL)
        free(((void **)p)[-1]);
}

static queue_t *g_queues;
static pending_t *g_pending;
static u64 g_queue_id;

static queue_t *queue_of(const hsa_queue_t *q)
{
    queue_t *p;

    for (p = g_queues; p != NULL; p = p->next)
        if (&p->pub == q)
            return p;
    return NULL;
}

hsa_status_t hsa_queue_create(hsa_agent_t agent, uint32_t size,
                              hsa_queue_type32_t type,
                              void (*callback)(hsa_status_t, hsa_queue_t *,
                                               void *),
                              void *data, uint32_t private_segment_size,
                              uint32_t group_segment_size,
                              hsa_queue_t **queue)
{
    queue_t *q;
    signal_t *bell;
    u32 i;

    (void)private_segment_size;
    (void)group_segment_size;
    CHECK_INIT();
    if (agent_of(agent) != &g_gpu)
        return HSA_STATUS_ERROR_INVALID_AGENT;
    if (queue == NULL || size < 64 || size > 4096 || (size & (size - 1)) ||
        type > HSA_QUEUE_TYPE_SINGLE)
        return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    q = (queue_t *)calloc(1, sizeof(*q));
    bell = signal_new(0);
    if (q == NULL || bell == NULL)
        goto oom;
    q->pub.base_address = NULL;
    /* aligned to the packet size, as the specification requires */
    q->pub.base_address = _aligned_malloc_compat(size * 64);
    if (q->pub.base_address == NULL)
        goto oom;
    for (i = 0; i < size; i++)
        ((hsa_kernel_dispatch_packet_t *)q->pub.base_address)[i].header =
            HSA_PACKET_TYPE_INVALID;
    q->gpu_copy = (hsa_queue_t *)hsa_heap_alloc(sizeof(hsa_queue_t), 256);
    if (q->gpu_copy == NULL)
        goto oom;
    bell->doorbell_of = q;
    q->pub.type = type;
    q->pub.features = HSA_QUEUE_FEATURE_KERNEL_DISPATCH;
    q->pub.doorbell_signal.handle = (u64)(ULONG_PTR)bell;
    q->pub.size = size;
    q->pub.id = ++g_queue_id;
    q->callback = callback;
    q->cb_data = data;
    *q->gpu_copy = q->pub;
    q->next = g_queues;
    g_queues = q;
    *queue = &q->pub;
    return HSA_STATUS_SUCCESS;

oom:
    if (q != NULL)
        free_aligned_compat(q->pub.base_address);
    free(q);
    free(bell);
    return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
}

hsa_status_t hsa_soft_queue_create(hsa_region_t region, uint32_t size,
                                   hsa_queue_type32_t type, uint32_t features,
                                   hsa_signal_t doorbell_signal,
                                   hsa_queue_t **queue)
{
    (void)region;
    (void)size;
    (void)type;
    (void)features;
    (void)doorbell_signal;
    (void)queue;
    CHECK_INIT();
    return HSA_STATUS_ERROR_INVALID_QUEUE_CREATION;
}

static void queue_free(queue_t *q)
{
    signal_t *bell = signal_of(q->pub.doorbell_signal);

    if (bell != NULL) {
        bell->magic = 0;
        free(bell);
    }
    hsa_heap_free(q->gpu_copy);
    free_aligned_compat(q->pub.base_address);
    free(q);
}

hsa_status_t hsa_queue_destroy(hsa_queue_t *queue)
{
    queue_t *q, **pp;

    CHECK_INIT();
    q = queue_of(queue);
    if (q == NULL)
        return HSA_STATUS_ERROR_INVALID_QUEUE;
    /* let the GPU finish with anything the queue started */
    if (q->last_seq != 0)
        gcn_gpu_wait(q->last_seq, 10000);
    progress();
    for (pp = &g_queues; *pp != q; pp = &(*pp)->next)
        ;
    *pp = q->next;
    queue_free(q);
    return HSA_STATUS_SUCCESS;
}

static void queues_destroy_all(void)
{
    while (g_queues != NULL) {
        queue_t *q = g_queues;

        g_queues = q->next;
        if (q->last_seq != 0)
            gcn_gpu_wait(q->last_seq, 10000);
        queue_free(q);
    }
    while (g_pending != NULL) {
        pending_t *p = g_pending;

        g_pending = p->next;
        free(p);
    }
}

hsa_status_t hsa_queue_inactivate(hsa_queue_t *queue)
{
    queue_t *q;

    CHECK_INIT();
    q = queue_of(queue);
    if (q == NULL)
        return HSA_STATUS_ERROR_INVALID_QUEUE;
    q->broken = 1;
    return HSA_STATUS_SUCCESS;
}

#define QI(queue) ((queue_t *)(queue))

uint64_t hsa_queue_load_read_index_scacquire(const hsa_queue_t *q)
{
    __sync_synchronize();
    return QI(q)->read_index;
}
uint64_t hsa_queue_load_read_index_relaxed(const hsa_queue_t *q)
{
    return QI(q)->read_index;
}
uint64_t hsa_queue_load_read_index_acquire(const hsa_queue_t *q)
{
    return hsa_queue_load_read_index_scacquire(q);
}
uint64_t hsa_queue_load_write_index_scacquire(const hsa_queue_t *q)
{
    __sync_synchronize();
    return QI(q)->write_index;
}
uint64_t hsa_queue_load_write_index_relaxed(const hsa_queue_t *q)
{
    return QI(q)->write_index;
}
uint64_t hsa_queue_load_write_index_acquire(const hsa_queue_t *q)
{
    return hsa_queue_load_write_index_scacquire(q);
}
void hsa_queue_store_write_index_relaxed(const hsa_queue_t *q, uint64_t v)
{
    QI(q)->write_index = v;
}
void hsa_queue_store_write_index_screlease(const hsa_queue_t *q, uint64_t v)
{
    __sync_synchronize();
    QI(q)->write_index = v;
}
void hsa_queue_store_write_index_release(const hsa_queue_t *q, uint64_t v)
{
    hsa_queue_store_write_index_screlease(q, v);
}
void hsa_queue_store_read_index_relaxed(const hsa_queue_t *q, uint64_t v)
{
    QI(q)->read_index = v;
}
void hsa_queue_store_read_index_screlease(const hsa_queue_t *q, uint64_t v)
{
    __sync_synchronize();
    QI(q)->read_index = v;
}
void hsa_queue_store_read_index_release(const hsa_queue_t *q, uint64_t v)
{
    hsa_queue_store_read_index_screlease(q, v);
}

#define ADDW(name)                                                            \
    uint64_t hsa_queue_##name(const hsa_queue_t *q, uint64_t v)              \
    {                                                                         \
        return __sync_fetch_and_add(&QI(q)->write_index, v);                  \
    }
ADDW(add_write_index_scacq_screl)
ADDW(add_write_index_scacquire)
ADDW(add_write_index_relaxed)
ADDW(add_write_index_screlease)
ADDW(add_write_index_acq_rel)
ADDW(add_write_index_acquire)
ADDW(add_write_index_release)

#define CASW(name)                                                            \
    uint64_t hsa_queue_##name(const hsa_queue_t *q, uint64_t e, uint64_t v)  \
    {                                                                         \
        return __sync_val_compare_and_swap(&QI(q)->write_index, e, v);        \
    }
CASW(cas_write_index_scacq_screl)
CASW(cas_write_index_scacquire)
CASW(cas_write_index_relaxed)
CASW(cas_write_index_screlease)
CASW(cas_write_index_acq_rel)

/* ---- packet processor -------------------------------------------------- */

static void queue_error(queue_t *q, hsa_status_t st)
{
    const char *text = "unknown error";

    q->broken = 1;
    if (q->callback != NULL) {
        q->callback(st, &q->pub, q->cb_data);
        return;
    }
    /* nobody to tell, and the packet's signal would never complete */
    hsa_status_string(st, &text);
    hsa_complain("queue %llu failed: %s\n", (unsigned long long)q->pub.id,
                 text);
    abort();
}

static void add_pending(u32 seq, signal_t *s)
{
    pending_t *p = (pending_t *)calloc(1, sizeof(*p)), **pp;

    p->seq = seq;
    p->signal = s;
    /* keep the list in submission order */
    for (pp = &g_pending; *pp != NULL; pp = &(*pp)->next)
        ;
    *pp = p;
}

static u32 completed_seq(void)
{
    return gcn_gpu_completed();
}

static int seq_done(u32 seq, u32 done)
{
    return (int)(done - seq) >= 0;
}

/* a slot whose previous dispatch has finished */
static slot_t *slot_get(void)
{
    slot_t *s = &g_slot[g_slot_next];

    g_slot_next = (g_slot_next + 1) % SLOTS;
    if (s->seq != 0 && !seq_done(s->seq, completed_seq()))
        gcn_gpu_wait(s->seq, 10000);
    return s;
}

/*
 * Builds the indirect buffer for one kernel dispatch and queues it.
 * Returns the submission's sequence number, or 0 when the packet cannot
 * be run (reported through the queue's error callback).
 */
static u32 dispatch(queue_t *q, const hsa_kernel_dispatch_packet_t *pkt,
                    u64 index)
{
    const hsa_kernel_descriptor *kd =
        (const hsa_kernel_descriptor *)(ULONG_PTR)pkt->kernel_object;
    slot_t *slot;
    u32 *ib, n = 0, user[16], nuser = 0, i;
    u32 rsrc2, lds, props, wg[3], grid[3], groups[3], partial = 0;
    u64 entry;
    hsa_kernel_dispatch_packet_t *pkt_copy;
    int dims = (pkt->setup >> HSA_KERNEL_DISPATCH_PACKET_SETUP_DIMENSIONS) & 3;

    if (kd == NULL || !hsa_heap_owns(kd)) {
        hsa_complain("dispatch: kernel_object %llx is not a loaded kernel\n",
                     (unsigned long long)pkt->kernel_object);
        queue_error(q, HSA_STATUS_ERROR_INVALID_PACKET_FORMAT);
        return 0;
    }
    if (kd->private_segment_fixed_size != 0 || pkt->private_segment_size != 0 ||
        (kd->kernel_code_properties & KD_USES_DYNAMIC_STACK)) {
        hsa_complain("dispatch: kernels using private (scratch) memory are "
                     "not supported yet\n");
        queue_error(q, HSA_STATUS_ERROR_OUT_OF_RESOURCES);
        return 0;
    }
    entry = pkt->kernel_object + (u64)kd->kernel_code_entry_byte_offset;
    if (entry & 0xff) {
        hsa_complain("dispatch: kernel code at %llx is not 256-byte aligned\n",
                     (unsigned long long)entry);
        queue_error(q, HSA_STATUS_ERROR_INVALID_CODE_OBJECT);
        return 0;
    }

    slot = slot_get();
    ib = (u32 *)slot->mem;
    pkt_copy = (hsa_kernel_dispatch_packet_t *)(slot->mem + 256);
    *pkt_copy = *pkt;

    /* user SGPRs, in the order the hardware ABI defines */
    props = kd->kernel_code_properties;
    if (props & KD_PRIVATE_SEGMENT_BUFFER)
        for (i = 0; i < 4; i++)
            user[nuser++] = 0;
    if (props & KD_DISPATCH_PTR) {
        user[nuser++] = (u32)(ULONG_PTR)pkt_copy;
        user[nuser++] = 0;
    }
    if (props & KD_QUEUE_PTR) {
        user[nuser++] = (u32)(ULONG_PTR)q->gpu_copy;
        user[nuser++] = 0;
    }
    if (props & KD_KERNARG_SEGMENT_PTR) {
        user[nuser++] = (u32)(ULONG_PTR)pkt->kernarg_address;
        user[nuser++] = 0;
    }
    if (props & KD_DISPATCH_ID) {
        user[nuser++] = (u32)index;
        user[nuser++] = (u32)(index >> 32);
    }
    if (props & KD_FLAT_SCRATCH_INIT) {
        user[nuser++] = 0;
        user[nuser++] = 0;
    }
    if (props & KD_PRIVATE_SEGMENT_SIZE)
        user[nuser++] = 0;

    rsrc2 = kd->compute_pgm_rsrc2;
    if (((rsrc2 & RSRC2_USER_SGPR_MASK) >> 1) < nuser) {
        hsa_complain("dispatch: kernel asks for %u user SGPRs but its "
                     "properties need %u\n",
                     (rsrc2 & RSRC2_USER_SGPR_MASK) >> 1, nuser);
        queue_error(q, HSA_STATUS_ERROR_INVALID_CODE_OBJECT);
        return 0;
    }
    lds = pkt->group_segment_size;
    if (lds > 65536) {
        /* garbage, typically an uninitialised field; the hardware limit
         * leaves no sensible reading of it */
        static int warned;

        if (!warned++)
            hsa_complain("dispatch: group segment size %u is beyond the "
                         "64 KB of LDS; using the kernel's static size\n",
                         lds);
        lds = 0;
    }
    if (lds < kd->group_segment_fixed_size)
        lds = kd->group_segment_fixed_size;
    /* LDS is allocated in blocks of 128 dwords */
    rsrc2 = (rsrc2 & ~RSRC2_LDS_SIZE_MASK) | RSRC2_LDS_SIZE((lds + 511) / 512);

    wg[0] = pkt->workgroup_size_x ? pkt->workgroup_size_x : 1;
    wg[1] = dims >= 2 && pkt->workgroup_size_y ? pkt->workgroup_size_y : 1;
    wg[2] = dims >= 3 && pkt->workgroup_size_z ? pkt->workgroup_size_z : 1;
    grid[0] = pkt->grid_size_x ? pkt->grid_size_x : 1;
    grid[1] = dims >= 2 && pkt->grid_size_y ? pkt->grid_size_y : 1;
    grid[2] = dims >= 3 && pkt->grid_size_z ? pkt->grid_size_z : 1;
    for (i = 0; i < 3; i++) {
        groups[i] = (grid[i] + wg[i] - 1) / wg[i];
        if (grid[i] % wg[i])
            partial = 1;
    }

#define SH(reg, cnt)                                                          \
    do {                                                                      \
        ib[n++] = PACKET3(PACKET3_SET_SH_REG, cnt);                           \
        ib[n++] = (reg) - SET_SH_REG_START;                                   \
    } while (0)
    if (pkt->header & (1u << HSA_PACKET_HEADER_BARRIER)) {
        /* everything before this packet must have finished */
        ib[n++] = PACKET3(PACKET3_EVENT_WRITE, 0);
        ib[n++] = EVENT_TYPE(CS_PARTIAL_FLUSH) | EVENT_INDEX(4);
    }
    SH(mmCOMPUTE_STATIC_THREAD_MGMT_SE0, 2);
    ib[n++] = 0xffffffff;
    ib[n++] = 0xffffffff;
    SH(mmCOMPUTE_STATIC_THREAD_MGMT_SE2, 2);
    ib[n++] = 0xffffffff;
    ib[n++] = 0xffffffff;
    SH(mmCOMPUTE_RESOURCE_LIMITS, 1);
    ib[n++] = 0;
    SH(mmCOMPUTE_START_X, 3);
    ib[n++] = 0;
    ib[n++] = 0;
    ib[n++] = 0;
    SH(mmCOMPUTE_NUM_THREAD_X, 3);
    for (i = 0; i < 3; i++)
        ib[n++] = wg[i] | ((grid[i] % wg[i]) << 16);
    SH(mmCOMPUTE_PGM_LO, 2);
    ib[n++] = (u32)(entry >> 8);
    ib[n++] = (u32)(entry >> 40);
    SH(mmCOMPUTE_PGM_RSRC1, 2);
    ib[n++] = kd->compute_pgm_rsrc1;
    ib[n++] = rsrc2;
    SH(mmCOMPUTE_TMPRING_SIZE, 1);
    ib[n++] = 0;
    if (nuser != 0) {
        SH(mmCOMPUTE_USER_DATA_0, nuser);
        for (i = 0; i < nuser; i++)
            ib[n++] = user[i];
    }
    ib[n++] = PACKET3_COMPUTE(PACKET3_DISPATCH_DIRECT, 3);
    ib[n++] = groups[0];
    ib[n++] = groups[1];
    ib[n++] = groups[2];
    ib[n++] = COMPUTE_SHADER_EN | FORCE_START_AT_000 |
              (partial ? PARTIAL_TG_EN : 0);
#undef SH

    hsa_trace("dispatch #%llu: kernel %p code %llx, grid %ux%ux%u, "
              "workgroup %ux%ux%u, lds %u, %u user sgprs\n",
              (unsigned long long)index, (void *)kd,
              (unsigned long long)entry, grid[0], grid[1], grid[2], wg[0],
              wg[1], wg[2], lds, nuser);
    slot->seq = gcn_gpu_submit((u64)(ULONG_PTR)ib, n, GCN_USER_VMID, 1);
    return slot->seq;
}

/* dependencies of a barrier packet satisfied? */
static int barrier_ready(const hsa_barrier_and_packet_t *b, int is_or)
{
    int i, any = 0, all = 1, used = 0;

    for (i = 0; i < 5; i++) {
        signal_t *s;

        if (b->dep_signal[i].handle == 0)
            continue;
        s = signal_of(b->dep_signal[i]);
        if (s == NULL)
            continue;
        used = 1;
        if (s->value == 0)
            any = 1;
        else
            all = 0;
    }
    return is_or ? (!used || any) : all;
}

static void process_queue(queue_t *q)
{
    u32 mask = q->pub.size - 1;

    if (q->busy || q->broken)
        return;
    q->busy = 1;
    for (;;) {
        u64 idx = q->read_index;
        hsa_kernel_dispatch_packet_t *pkt;
        u16 header;
        u32 type;
        signal_t *done;

        __sync_synchronize();
        if (idx >= q->write_index)
            break;
        pkt = (hsa_kernel_dispatch_packet_t *)q->pub.base_address + (idx & mask);
        header = *(volatile u16 *)&pkt->header;
        type = header & 0xff;
        hsa_trace("queue %llu: packet %llu, header %04x\n",
                  (unsigned long long)q->pub.id, (unsigned long long)idx,
                  header);
        if (type == HSA_PACKET_TYPE_INVALID)
            break;      /* reserved but not yet written */
        __sync_synchronize();
        done = signal_of(pkt->completion_signal);

        if (type == HSA_PACKET_TYPE_KERNEL_DISPATCH) {
            u32 seq = dispatch(q, pkt, idx);

            if (seq == 0)
                break;
            q->last_seq = seq;
            if (done != NULL)
                add_pending(seq, done);
        } else if (type == HSA_PACKET_TYPE_BARRIER_AND ||
                   type == HSA_PACKET_TYPE_BARRIER_OR) {
            const hsa_barrier_and_packet_t *b =
                (const hsa_barrier_and_packet_t *)pkt;

            if (!barrier_ready(b, type == HSA_PACKET_TYPE_BARRIER_OR))
                break;  /* try again on the next poll */
            if ((header & (1u << HSA_PACKET_HEADER_BARRIER)) &&
                q->last_seq != 0 && !seq_done(q->last_seq, completed_seq()))
                break;
            /* complete in order with the work queued before it */
            if (done != NULL) {
                if (q->last_seq != 0 && !seq_done(q->last_seq, completed_seq()))
                    add_pending(q->last_seq, done);
                else
                    __sync_fetch_and_sub(&done->value, 1);
            }
        } else {
            hsa_complain("queue %llu: packet type %u is not supported\n",
                         (unsigned long long)q->pub.id, type);
            queue_error(q, HSA_STATUS_ERROR_INVALID_PACKET_FORMAT);
            break;
        }
        /* the slot is ours to free: hand it back to the producer */
        *(volatile u16 *)&pkt->header = HSA_PACKET_TYPE_INVALID;
        __sync_synchronize();
        q->read_index = idx + 1;
    }
    q->busy = 0;
}

static void doorbell_rung(signal_t *s)
{
    hsa_trace("doorbell of queue %llu: %lld\n",
              (unsigned long long)s->doorbell_of->pub.id, s->value);
    if (g_refcount > 0)
        process_queue(s->doorbell_of);
}

/* Reports finished work and moves the queues on. */
static void progress(void)
{
    queue_t *q;
    u32 done;
    int invalidated = 0;

    if (g_refcount == 0)
        return;
    for (q = g_queues; q != NULL; q = q->next)
        process_queue(q);
    if (g_pending == NULL)
        return;
    done = completed_seq();
    while (g_pending != NULL && seq_done(g_pending->seq, done)) {
        pending_t *p = g_pending;

        if (!invalidated) {
            /* results are in memory; make sure the CPU reads them fresh */
            gcn_dev_invalidate_reads();
            invalidated = 1;
        }
        g_pending = p->next;
        __sync_fetch_and_sub(&p->signal->value, 1);
        free(p);
    }
}

hsa_status_t hsa_amd_profiling_set_profiler_enabled(hsa_queue_t *queue,
                                                    int enable)
{
    (void)enable;
    CHECK_INIT();
    return queue_of(queue) != NULL ? HSA_STATUS_SUCCESS
                                   : HSA_STATUS_ERROR_INVALID_QUEUE;
}
