/*
 * hsa_ext_amd.h - the AMD vendor extensions the gcn4xp runtime provides:
 * extra agent and region queries.
 */
#ifndef HSA_RUNTIME_EXT_AMD_H_
#define HSA_RUNTIME_EXT_AMD_H_

#include "hsa.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HSA_AMD_AGENT_INFO_CHIP_ID = 0xA000,
    HSA_AMD_AGENT_INFO_CACHELINE_SIZE = 0xA001,
    HSA_AMD_AGENT_INFO_COMPUTE_UNIT_COUNT = 0xA002,
    HSA_AMD_AGENT_INFO_MAX_CLOCK_FREQUENCY = 0xA003,
    HSA_AMD_AGENT_INFO_DRIVER_NODE_ID = 0xA004,
    HSA_AMD_AGENT_INFO_MAX_ADDRESS_WATCH_POINTS = 0xA005,
    HSA_AMD_AGENT_INFO_BDFID = 0xA006,
    HSA_AMD_AGENT_INFO_MEMORY_WIDTH = 0xA007,
    HSA_AMD_AGENT_INFO_MEMORY_MAX_FREQUENCY = 0xA008,
    HSA_AMD_AGENT_INFO_PRODUCT_NAME = 0xA009,
    HSA_AMD_AGENT_INFO_MAX_WAVES_PER_CU = 0xA00A,
    HSA_AMD_AGENT_INFO_NUM_SIMDS_PER_CU = 0xA00B,
    HSA_AMD_AGENT_INFO_NUM_SHADER_ENGINES = 0xA00C,
    HSA_AMD_AGENT_INFO_NUM_SHADER_ARRAYS_PER_SE = 0xA00D
} hsa_amd_agent_info_t;

typedef enum {
    HSA_AMD_REGION_INFO_HOST_ACCESSIBLE = 0xA000,
    HSA_AMD_REGION_INFO_BASE = 0xA001,
    HSA_AMD_REGION_INFO_BUS_WIDTH = 0xA002,
    HSA_AMD_REGION_INFO_MAX_CLOCK_FREQUENCY = 0xA003
} hsa_amd_region_info_t;

typedef struct hsa_amd_dispatch_time_s {
    uint64_t start;
    uint64_t end;
} hsa_amd_profiling_dispatch_time_t;

/* Accepted for compatibility; dispatch times are not recorded. */
hsa_status_t HSA_API hsa_amd_profiling_set_profiler_enabled(hsa_queue_t *queue,
                                                            int enable);

#ifdef __cplusplus
}
#endif

#endif
