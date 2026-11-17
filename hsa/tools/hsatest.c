/*
 * hsatest - smoke test of the HSA runtime through its public API.
 *
 *   hsatest <code object> [kernel]
 *
 * Loads the code object, runs the kernel (default "hello_world") once on a
 * 64-item grid with a single argument pointing at a zeroed 4 KB buffer, and
 * prints the first words of the buffer afterwards.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <hsa/hsa.h>
#include <hsa/hsa_ext_amd.h>
#include "gcn_gpu.h"

static hsa_agent_t g_agent;
static hsa_region_t g_kernarg;

static hsa_status_t find_gpu(hsa_agent_t a, void *d)
{
    hsa_device_type_t t;

    (void)d;
    hsa_agent_get_info(a, HSA_AGENT_INFO_DEVICE, &t);
    if (t == HSA_DEVICE_TYPE_GPU)
        g_agent = a;
    return HSA_STATUS_SUCCESS;
}

static hsa_status_t find_kernarg(hsa_region_t r, void *d)
{
    uint32_t flags = 0;

    (void)d;
    hsa_region_get_info(r, HSA_REGION_INFO_GLOBAL_FLAGS, &flags);
    if (flags & HSA_REGION_GLOBAL_FLAG_KERNARG)
        g_kernarg = r;
    return HSA_STATUS_SUCCESS;
}

static int check(hsa_status_t st, const char *what)
{
    const char *s = "?";

    if (st == HSA_STATUS_SUCCESS)
        return 0;
    hsa_status_string(st, &s);
    fprintf(stderr, "%s: %s\n", what, s);
    exit(1);
}

int main(int argc, char **argv)
{
    const char *kname = argc > 2 ? argv[2] : "hello_world";
    FILE *f;
    long size;
    void *file;
    hsa_code_object_reader_t reader;
    hsa_executable_t exe;
    hsa_executable_symbol_t sym;
    hsa_queue_t *q;
    hsa_signal_t done;
    hsa_kernel_dispatch_packet_t *pkt;
    uint64_t kobj, idx;
    uint32_t *out, i;
    void **args;

    if (argc < 2) {
        fprintf(stderr, "usage: hsatest <code object> [kernel]\n");
        return 2;
    }
    f = fopen(argv[1], "rb");
    if (f == NULL) {
        perror(argv[1]);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    file = malloc((size_t)size);
    if (fread(file, 1, (size_t)size, f) != (size_t)size)
        return 1;
    fclose(f);

    check(hsa_init(), "hsa_init");
    hsa_iterate_agents(find_gpu, NULL);
    hsa_agent_iterate_regions(g_agent, find_kernarg, NULL);
    check(hsa_code_object_reader_create_from_memory(file, (size_t)size,
                                                    &reader), "reader");
    check(hsa_executable_create_alt(HSA_PROFILE_BASE,
                                    HSA_DEFAULT_FLOAT_ROUNDING_MODE_DEFAULT,
                                    NULL, &exe), "executable");
    check(hsa_executable_load_agent_code_object(exe, g_agent, reader, NULL,
                                                NULL), "load");
    check(hsa_executable_freeze(exe, NULL), "freeze");
    check(hsa_executable_get_symbol_by_name(exe, kname, &g_agent, &sym),
          kname);
    check(hsa_executable_symbol_get_info(
              sym, HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_OBJECT, &kobj),
          "kernel object");
    check(hsa_queue_create(g_agent, 64, HSA_QUEUE_TYPE_SINGLE, NULL, NULL,
                           0, 0, &q), "queue");
    check(hsa_signal_create(1, 0, NULL, &done), "signal");
    check(hsa_memory_allocate(g_kernarg, 4096, (void **)&out), "buffer");
    check(hsa_memory_allocate(g_kernarg, 256, (void **)&args), "kernargs");
    memset(out, 0, 4096);
    memset(args, 0, 256);
    args[0] = out;

    idx = hsa_queue_add_write_index_relaxed(q, 1);
    pkt = (hsa_kernel_dispatch_packet_t *)q->base_address + (idx % q->size);
    memset((char *)pkt + 4, 0, sizeof(*pkt) - 4);
    pkt->workgroup_size_x = 64;
    pkt->workgroup_size_y = pkt->workgroup_size_z = 1;
    pkt->grid_size_x = 64;
    pkt->grid_size_y = pkt->grid_size_z = 1;
    pkt->kernel_object = kobj;
    pkt->kernarg_address = args;
    pkt->completion_signal = done;
    __atomic_store_n((uint32_t *)pkt,
                     (HSA_PACKET_TYPE_KERNEL_DISPATCH << HSA_PACKET_HEADER_TYPE) |
                     (1 << HSA_PACKET_HEADER_BARRIER) |
                     (HSA_FENCE_SCOPE_SYSTEM << HSA_PACKET_HEADER_ACQUIRE_FENCE_SCOPE) |
                     (HSA_FENCE_SCOPE_SYSTEM << HSA_PACKET_HEADER_RELEASE_FENCE_SCOPE) |
                     (1u << 16), __ATOMIC_RELEASE);
    hsa_signal_store_relaxed(q->doorbell_signal, (hsa_signal_value_t)idx);

    if (hsa_signal_wait_scacquire(done, HSA_SIGNAL_CONDITION_EQ, 0,
                                  5ull * 1000 * 1000 * 1000,
                                  HSA_WAIT_STATE_ACTIVE) != 0) {
        printf("timed out\n");
        gcn_gpu_status(stdout);
        return 1;
    }
    printf("buffer at %p, kernargs at %p:", (void *)out, (void *)args);
    for (i = 0; i < 8; i++)
        printf(" %08x", out[i]);
    printf("\n");
    if (gcn_vm_fault(stdout))
        return 1;
    hsa_shut_down();
    return 0;
}
