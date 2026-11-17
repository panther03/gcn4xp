/*
 * hsa_loader.c - AMDGPU code objects (HSACO) and executables.
 *
 * Takes the ELF code objects LLVM produces for amdgcn-amd-amdhsa, code
 * object versions 3 to 6.  The loadable segments are copied as one image
 * into GPU memory and relocated there; each kernel is found through its
 * 64-byte descriptor symbol ("name.kd"), whose address in the loaded image
 * is the kernel object handle.  Version 2 code objects (from the old
 * .hsa_code_object_version directives) are not supported.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "hsa_impl.h"
#include "elf64.h"

#define CO_MAGIC   0x434f4445u
#define EXE_MAGIC  0x45584543u
#define SYM_MAGIC  0x53594d42u

typedef struct {
    u32 magic;
    u8 *data;           /* private copy of the ELF file */
    size_t size;
} code_object_t;

typedef struct image {
    struct image *next;
    u8 *mem;            /* loaded segments, in GPU memory */
} image_t;

typedef struct symbol {
    u32 magic;
    struct symbol *next;
    char *name;         /* as in the ELF symbol table */
    int is_kernel;
    u64 address;        /* kernel descriptor or variable */
    u64 size;
    struct executable *exe;
} symbol_t;

typedef struct executable {
    u32 magic;
    hsa_profile_t profile;
    hsa_executable_state_t state;
    image_t *images;
    symbol_t *symbols;
} executable_t;

static hsa_agent_t g_gpu_agent;     /* found on first use */

static int is_gpu(hsa_agent_t a)
{
    hsa_device_type_t t;

    return hsa_agent_get_info(a, HSA_AGENT_INFO_DEVICE, &t) ==
           HSA_STATUS_SUCCESS && t == HSA_DEVICE_TYPE_GPU;
}

/* ---- code objects ------------------------------------------------------ */

static code_object_t *co_new(const void *data, size_t size)
{
    code_object_t *co = (code_object_t *)calloc(1, sizeof(*co));

    if (co == NULL)
        return NULL;
    co->data = (u8 *)malloc(size ? size : 1);
    if (co->data == NULL) {
        free(co);
        return NULL;
    }
    /* the source may be GPU memory: read it only once */
    if (hsa_heap_owns(data))
        gcn_dev_invalidate_reads();
    memcpy(co->data, data, size);
    co->size = size;
    co->magic = CO_MAGIC;
    return co;
}

static code_object_t *co_of(u64 h)
{
    code_object_t *co = (code_object_t *)(ULONG_PTR)h;

    return co != NULL && co->magic == CO_MAGIC ? co : NULL;
}

static void co_free(code_object_t *co)
{
    co->magic = 0;
    free(co->data);
    free(co);
}

hsa_status_t hsa_code_object_deserialize(void *ptr, size_t size,
                                         const char *options,
                                         hsa_code_object_t *code_object)
{
    code_object_t *co;

    (void)options;
    if (!hsa_rt_ready())
        return HSA_STATUS_ERROR_NOT_INITIALIZED;
    if (ptr == NULL || size == 0 || code_object == NULL)
        return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    co = co_new(ptr, size);
    if (co == NULL)
        return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
    code_object->handle = (u64)(ULONG_PTR)co;
    return HSA_STATUS_SUCCESS;
}

hsa_status_t hsa_code_object_destroy(hsa_code_object_t code_object)
{
    code_object_t *co = co_of(code_object.handle);

    if (co == NULL)
        return HSA_STATUS_ERROR_INVALID_CODE_OBJECT;
    co_free(co);
    return HSA_STATUS_SUCCESS;
}

hsa_status_t hsa_code_object_reader_create_from_memory(
    const void *code_object, size_t size, hsa_code_object_reader_t *reader)
{
    code_object_t *co;

    if (!hsa_rt_ready())
        return HSA_STATUS_ERROR_NOT_INITIALIZED;
    if (code_object == NULL || size == 0 || reader == NULL)
        return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    co = co_new(code_object, size);
    if (co == NULL)
        return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
    reader->handle = (u64)(ULONG_PTR)co;
    return HSA_STATUS_SUCCESS;
}

hsa_status_t hsa_code_object_reader_create_from_file(
    hsa_file_t file, hsa_code_object_reader_t *reader)
{
    off_t end;
    size_t size, got = 0;
    u8 *buf;
    hsa_status_t st;

    if (!hsa_rt_ready())
        return HSA_STATUS_ERROR_NOT_INITIALIZED;
    if (reader == NULL)
        return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    end = lseek(file, 0, SEEK_END);
    if (end <= 0 || lseek(file, 0, SEEK_SET) != 0)
        return HSA_STATUS_ERROR_INVALID_FILE;
    size = (size_t)end;
    buf = (u8 *)malloc(size);
    if (buf == NULL)
        return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
    while (got < size) {
        ssize_t r = read(file, buf + got, size - got);

        if (r <= 0) {
            free(buf);
            return HSA_STATUS_ERROR_INVALID_FILE;
        }
        got += (size_t)r;
    }
    st = hsa_code_object_reader_create_from_memory(buf, size, reader);
    free(buf);
    return st;
}

hsa_status_t hsa_code_object_reader_destroy(hsa_code_object_reader_t reader)
{
    code_object_t *co = co_of(reader.handle);

    if (co == NULL)
        return HSA_STATUS_ERROR_INVALID_CODE_OBJECT_READER;
    co_free(co);
    return HSA_STATUS_SUCCESS;
}

/* ---- executables ------------------------------------------------------- */

static executable_t *exe_of(hsa_executable_t e)
{
    executable_t *x = (executable_t *)(ULONG_PTR)e.handle;

    return x != NULL && x->magic == EXE_MAGIC ? x : NULL;
}

hsa_status_t hsa_executable_create(hsa_profile_t profile,
                                   hsa_executable_state_t state,
                                   const char *options,
                                   hsa_executable_t *executable)
{
    executable_t *x;

    (void)options;
    if (!hsa_rt_ready())
        return HSA_STATUS_ERROR_NOT_INITIALIZED;
    if (executable == NULL)
        return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    x = (executable_t *)calloc(1, sizeof(*x));
    if (x == NULL)
        return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
    x->magic = EXE_MAGIC;
    x->profile = profile;
    x->state = state;
    executable->handle = (u64)(ULONG_PTR)x;
    return HSA_STATUS_SUCCESS;
}

hsa_status_t hsa_executable_create_alt(
    hsa_profile_t profile, hsa_default_float_rounding_mode_t rounding,
    const char *options, hsa_executable_t *executable)
{
    (void)rounding;
    return hsa_executable_create(profile, HSA_EXECUTABLE_STATE_UNFROZEN,
                                 options, executable);
}

hsa_status_t hsa_executable_destroy(hsa_executable_t executable)
{
    executable_t *x = exe_of(executable);

    if (x == NULL)
        return HSA_STATUS_ERROR_INVALID_EXECUTABLE;
    while (x->symbols != NULL) {
        symbol_t *s = x->symbols;

        x->symbols = s->next;
        s->magic = 0;
        free(s->name);
        free(s);
    }
    while (x->images != NULL) {
        image_t *i = x->images;

        x->images = i->next;
        hsa_heap_free(i->mem);
        free(i);
    }
    x->magic = 0;
    free(x);
    return HSA_STATUS_SUCCESS;
}

/* bounds-checked view of the file */
#define IN_FILE(co, off, len)                                                 \
    ((u64)(off) <= (co)->size && (u64)(len) <= (co)->size - (u64)(off))

static hsa_status_t bad(const char *why)
{
    hsa_complain("code object: %s\n", why);
    return HSA_STATUS_ERROR_INVALID_CODE_OBJECT;
}

static hsa_status_t check_header(const code_object_t *co, const elf_ehdr **out)
{
    const elf_ehdr *eh = (const elf_ehdr *)co->data;
    u32 mach;

    if (co->size < sizeof(*eh) || memcmp(eh->e_ident, "\177ELF", 4) != 0)
        return bad("not an ELF file");
    if (eh->e_ident[4] != 2 || eh->e_ident[5] != 1)
        return bad("not a 64-bit little-endian ELF file");
    if (eh->e_machine != ELF_EM_AMDGPU)
        return bad("not an AMDGPU code object");
    if (eh->e_ident[7] != ELF_OSABI_AMDHSA)
        return bad("not an amdhsa code object");
    if (eh->e_ident[8] == 0)
        return bad("code object version 2 is not supported; reassemble "
                   "with .amdhsa_kernel directives (code object v3+)");
    if (eh->e_type != ELF_ET_DYN)
        return bad("not a shared object (link it with ld.lld -shared)");
    mach = eh->e_flags & 0xff;
    if (mach != EF_AMDGPU_MACH_GFX803) {
        hsa_complain("code object: built for processor 0x%02x, this GPU "
                     "is gfx803 (0x%02x)\n", mach, EF_AMDGPU_MACH_GFX803);
        return HSA_STATUS_ERROR_INVALID_ISA;
    }
    if (eh->e_phentsize != sizeof(elf_phdr) ||
        !IN_FILE(co, eh->e_phoff, (u64)eh->e_phnum * sizeof(elf_phdr)) ||
        (eh->e_shnum != 0 &&
         (eh->e_shentsize != sizeof(elf_shdr) ||
          !IN_FILE(co, eh->e_shoff, (u64)eh->e_shnum * sizeof(elf_shdr)))))
        return bad("damaged program or section headers");
    *out = eh;
    return HSA_STATUS_SUCCESS;
}

static void add_symbol(executable_t *x, const char *name, int kernel,
                       u64 addr, u64 size)
{
    symbol_t *s = (symbol_t *)calloc(1, sizeof(*s));

    s->magic = SYM_MAGIC;
    s->name = strdup(name);
    s->is_kernel = kernel;
    s->address = addr;
    s->size = size;
    s->exe = x;
    s->next = x->symbols;
    x->symbols = s;
}

static hsa_status_t load(executable_t *x, const code_object_t *co)
{
    const elf_ehdr *eh;
    const elf_phdr *ph;
    const elf_shdr *sh;
    u64 lo = ~0ull, hi = 0, align = 4096;
    u8 *mem;
    image_t *img;
    hsa_status_t st;
    u32 i, j;
    int symtab = -1;

    st = check_header(co, &eh);
    if (st != HSA_STATUS_SUCCESS)
        return st;
    ph = (const elf_phdr *)(co->data + eh->e_phoff);
    sh = (const elf_shdr *)(co->data + eh->e_shoff);

    /* one block of GPU memory spanning every loadable segment */
    for (i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != ELF_PT_LOAD || ph[i].p_memsz == 0)
            continue;
        if (ph[i].p_filesz > ph[i].p_memsz ||
            !IN_FILE(co, ph[i].p_offset, ph[i].p_filesz))
            return bad("damaged segment");
        if (ph[i].p_vaddr < lo)
            lo = ph[i].p_vaddr;
        if (ph[i].p_vaddr + ph[i].p_memsz > hi)
            hi = ph[i].p_vaddr + ph[i].p_memsz;
        if (ph[i].p_align > align)
            align = ph[i].p_align;
    }
    if (hi <= lo || hi - lo > 64u * 1024 * 1024 || align > 65536)
        return bad("no loadable segments, or implausibly large ones");
    lo &= ~(align - 1);
    mem = (u8 *)hsa_heap_alloc((size_t)(hi - lo), (size_t)align);
    if (mem == NULL)
        return HSA_STATUS_ERROR_OUT_OF_RESOURCES;
    memset(mem, 0, (size_t)(hi - lo));
    for (i = 0; i < eh->e_phnum; i++)
        if (ph[i].p_type == ELF_PT_LOAD && ph[i].p_filesz != 0)
            memcpy(mem + (ph[i].p_vaddr - lo), co->data + ph[i].p_offset,
                   (size_t)ph[i].p_filesz);
#define LOADED(vaddr) ((u64)(ULONG_PTR)mem + ((vaddr) - lo))

    /* the dynamic symbol table if there is one, else the full one */
    for (i = 0; i < eh->e_shnum; i++)
        if (sh[i].sh_type == ELF_SHT_DYNSYM)
            symtab = (int)i;
    for (i = 0; symtab < 0 && i < eh->e_shnum; i++)
        if (sh[i].sh_type == ELF_SHT_SYMTAB)
            symtab = (int)i;

    /* relocations */
    for (i = 0; i < eh->e_shnum; i++) {
        const elf_rela *r;
        const elf_shdr *ss;
        u64 nrel;

        if (sh[i].sh_type != ELF_SHT_RELA)
            continue;
        if (!IN_FILE(co, sh[i].sh_offset, sh[i].sh_size) ||
            sh[i].sh_link >= eh->e_shnum)
            goto damaged;
        ss = &sh[sh[i].sh_link];
        if (!IN_FILE(co, ss->sh_offset, ss->sh_size))
            goto damaged;
        r = (const elf_rela *)(co->data + sh[i].sh_offset);
        nrel = sh[i].sh_size / sizeof(elf_rela);
        for (j = 0; j < nrel; j++) {
            u32 type = (u32)r[j].r_info, si = (u32)(r[j].r_info >> 32);
            u64 where = r[j].r_offset, S = 0, v;
            const elf_sym *sym;

            if (where < lo || where + 8 > hi)
                goto damaged;
            if (si != 0) {
                if ((u64)si * sizeof(elf_sym) >= ss->sh_size)
                    goto damaged;
                sym = (const elf_sym *)(co->data + ss->sh_offset) + si;
                if (sym->st_shndx == ELF_SHN_UNDEF) {
                    hsa_complain("code object: undefined symbol in a "
                                 "relocation\n");
                    hsa_heap_free(mem);
                    return HSA_STATUS_ERROR_VARIABLE_UNDEFINED;
                }
                S = LOADED(sym->st_value);
            }
            v = S + (u64)r[j].r_addend;
            switch (type) {
            case R_AMDGPU_RELATIVE64:
                v = LOADED((u64)r[j].r_addend);
                /* fall through */
            case R_AMDGPU_ABS64:
                memcpy(mem + (where - lo), &v, 8);
                break;
            case R_AMDGPU_ABS32:
            case R_AMDGPU_ABS32_LO: {
                u32 w = (u32)v;
                memcpy(mem + (where - lo), &w, 4);
                break;
            }
            case R_AMDGPU_ABS32_HI: {
                u32 w = (u32)(v >> 32);
                memcpy(mem + (where - lo), &w, 4);
                break;
            }
            case R_AMDGPU_REL32:
            case R_AMDGPU_REL32_LO:
            case R_AMDGPU_REL32_HI:
            case R_AMDGPU_REL64: {
                u64 d = v - LOADED(where);
                u32 w = type == R_AMDGPU_REL32_HI ? (u32)(d >> 32) : (u32)d;

                if (type == R_AMDGPU_REL64)
                    memcpy(mem + (where - lo), &d, 8);
                else
                    memcpy(mem + (where - lo), &w, 4);
                break;
            }
            case 0:
                break;
            default:
                hsa_complain("code object: relocation type %u is not "
                             "supported\n", type);
                hsa_heap_free(mem);
                return HSA_STATUS_ERROR_INVALID_CODE_OBJECT;
            }
        }
    }

    /* kernels and variables */
    if (symtab >= 0) {
        const elf_shdr *ss = &sh[symtab];
        const elf_shdr *strs;
        const elf_sym *sym;
        u64 nsym;

        if (!IN_FILE(co, ss->sh_offset, ss->sh_size) ||
            ss->sh_link >= eh->e_shnum)
            goto damaged;
        strs = &sh[ss->sh_link];
        if (!IN_FILE(co, strs->sh_offset, strs->sh_size) || strs->sh_size == 0)
            goto damaged;
        sym = (const elf_sym *)(co->data + ss->sh_offset);
        nsym = ss->sh_size / sizeof(elf_sym);
        for (j = 1; j < nsym; j++) {
            const char *name;
            size_t len;

            if ((sym[j].st_info & 0xf) != ELF_STT_OBJECT ||
                (sym[j].st_info >> 4) == ELF_STB_LOCAL ||
                sym[j].st_shndx == ELF_SHN_UNDEF ||
                sym[j].st_name >= strs->sh_size)
                continue;
            name = (const char *)co->data + strs->sh_offset + sym[j].st_name;
            len = strnlen(name, (size_t)(strs->sh_size - sym[j].st_name));
            if (len == (size_t)(strs->sh_size - sym[j].st_name))
                continue;   /* not terminated */
            if (sym[j].st_value < lo || sym[j].st_value + sym[j].st_size > hi)
                goto damaged;
            if (len > 3 && strcmp(name + len - 3, ".kd") == 0 &&
                sym[j].st_size == sizeof(hsa_kernel_descriptor)) {
                hsa_trace("kernel %s at %llx\n", name,
                          (unsigned long long)LOADED(sym[j].st_value));
                add_symbol(x, name, 1, LOADED(sym[j].st_value), 64);
            } else {
                add_symbol(x, name, 0, LOADED(sym[j].st_value),
                           sym[j].st_size);
            }
        }
    }
#undef LOADED

    gcn_dev_flush_writes();
    img = (image_t *)calloc(1, sizeof(*img));
    img->mem = mem;
    img->next = x->images;
    x->images = img;
    return HSA_STATUS_SUCCESS;

damaged:
    hsa_heap_free(mem);
    return bad("damaged relocation or symbol tables");
}

static hsa_status_t load_checked(hsa_executable_t executable,
                                 hsa_agent_t agent, u64 co_handle)
{
    executable_t *x = exe_of(executable);
    code_object_t *co = co_of(co_handle);

    if (!hsa_rt_ready())
        return HSA_STATUS_ERROR_NOT_INITIALIZED;
    if (x == NULL)
        return HSA_STATUS_ERROR_INVALID_EXECUTABLE;
    if (x->state == HSA_EXECUTABLE_STATE_FROZEN)
        return HSA_STATUS_ERROR_FROZEN_EXECUTABLE;
    if (co == NULL)
        return HSA_STATUS_ERROR_INVALID_CODE_OBJECT;
    if (!is_gpu(agent))
        return HSA_STATUS_ERROR_INVALID_AGENT;
    g_gpu_agent = agent;
    return load(x, co);
}

hsa_status_t hsa_executable_load_code_object(hsa_executable_t executable,
                                             hsa_agent_t agent,
                                             hsa_code_object_t code_object,
                                             const char *options)
{
    (void)options;
    return load_checked(executable, agent, code_object.handle);
}

hsa_status_t hsa_executable_load_agent_code_object(
    hsa_executable_t executable, hsa_agent_t agent,
    hsa_code_object_reader_t reader, const char *options,
    hsa_loaded_code_object_t *loaded)
{
    hsa_status_t st;

    (void)options;
    st = load_checked(executable, agent, reader.handle);
    if (st == HSA_STATUS_SUCCESS && loaded != NULL)
        loaded->handle = (u64)(ULONG_PTR)exe_of(executable)->images;
    return st;
}

hsa_status_t hsa_executable_load_program_code_object(
    hsa_executable_t executable, hsa_code_object_reader_t reader,
    const char *options, hsa_loaded_code_object_t *loaded)
{
    (void)executable;
    (void)reader;
    (void)options;
    (void)loaded;
    /* program code objects carry no agent code: nothing on this target */
    return HSA_STATUS_ERROR_INVALID_CODE_OBJECT;
}

hsa_status_t hsa_executable_freeze(hsa_executable_t executable,
                                   const char *options)
{
    executable_t *x = exe_of(executable);

    (void)options;
    if (x == NULL)
        return HSA_STATUS_ERROR_INVALID_EXECUTABLE;
    if (x->state == HSA_EXECUTABLE_STATE_FROZEN)
        return HSA_STATUS_ERROR_FROZEN_EXECUTABLE;
    x->state = HSA_EXECUTABLE_STATE_FROZEN;
    return HSA_STATUS_SUCCESS;
}

hsa_status_t hsa_executable_get_info(hsa_executable_t executable,
                                     hsa_executable_info_t attribute,
                                     void *value)
{
    executable_t *x = exe_of(executable);

    if (x == NULL)
        return HSA_STATUS_ERROR_INVALID_EXECUTABLE;
    if (value == NULL)
        return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    switch (attribute) {
    case HSA_EXECUTABLE_INFO_PROFILE:
        *(hsa_profile_t *)value = x->profile;
        break;
    case HSA_EXECUTABLE_INFO_STATE:
        *(hsa_executable_state_t *)value = x->state;
        break;
    case HSA_EXECUTABLE_INFO_DEFAULT_FLOAT_ROUNDING_MODE:
        *(hsa_default_float_rounding_mode_t *)value =
            HSA_DEFAULT_FLOAT_ROUNDING_MODE_NEAR;
        break;
    default:
        return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    }
    return HSA_STATUS_SUCCESS;
}

hsa_status_t hsa_executable_global_variable_define(hsa_executable_t e,
                                                   const char *name,
                                                   void *address)
{
    (void)name;
    (void)address;
    return exe_of(e) == NULL ? HSA_STATUS_ERROR_INVALID_EXECUTABLE
                             : HSA_STATUS_ERROR_VARIABLE_UNDEFINED;
}

hsa_status_t hsa_executable_agent_global_variable_define(hsa_executable_t e,
                                                         hsa_agent_t agent,
                                                         const char *name,
                                                         void *address)
{
    (void)agent;
    return hsa_executable_global_variable_define(e, name, address);
}

hsa_status_t hsa_executable_validate(hsa_executable_t executable,
                                     uint32_t *result)
{
    if (exe_of(executable) == NULL)
        return HSA_STATUS_ERROR_INVALID_EXECUTABLE;
    if (result == NULL)
        return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    *result = 0;
    return HSA_STATUS_SUCCESS;
}

/* ---- symbols ----------------------------------------------------------- */

static symbol_t *sym_of(hsa_executable_symbol_t h)
{
    symbol_t *s = (symbol_t *)(ULONG_PTR)h.handle;

    return s != NULL && s->magic == SYM_MAGIC ? s : NULL;
}

/* "foo" and "foo.kd" both name the kernel whose descriptor is "foo.kd" */
static int name_matches(const symbol_t *s, const char *name)
{
    size_t n = strlen(name);

    if (strcmp(s->name, name) == 0)
        return 1;
    return s->is_kernel && strlen(s->name) == n + 3 &&
           strncmp(s->name, name, n) == 0;
}

hsa_status_t hsa_executable_get_symbol_by_name(hsa_executable_t executable,
                                               const char *name,
                                               const hsa_agent_t *agent,
                                               hsa_executable_symbol_t *out)
{
    executable_t *x = exe_of(executable);
    symbol_t *s;

    (void)agent;
    if (x == NULL)
        return HSA_STATUS_ERROR_INVALID_EXECUTABLE;
    if (name == NULL || out == NULL)
        return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    for (s = x->symbols; s != NULL; s = s->next) {
        if (name_matches(s, name)) {
            out->handle = (u64)(ULONG_PTR)s;
            return HSA_STATUS_SUCCESS;
        }
    }
    return HSA_STATUS_ERROR_INVALID_SYMBOL_NAME;
}

hsa_status_t hsa_executable_get_symbol(hsa_executable_t executable,
                                       const char *module_name,
                                       const char *symbol_name,
                                       hsa_agent_t agent,
                                       int32_t call_convention,
                                       hsa_executable_symbol_t *symbol)
{
    (void)module_name;
    (void)call_convention;
    return hsa_executable_get_symbol_by_name(executable, symbol_name, &agent,
                                             symbol);
}

static const hsa_kernel_descriptor *kd_of(const symbol_t *s)
{
    return (const hsa_kernel_descriptor *)(ULONG_PTR)s->address;
}

hsa_status_t hsa_executable_symbol_get_info(hsa_executable_symbol_t symbol,
                                            hsa_executable_symbol_info_t attr,
                                            void *value)
{
    symbol_t *s = sym_of(symbol);

    if (s == NULL)
        return HSA_STATUS_ERROR_INVALID_EXECUTABLE_SYMBOL;
    if (value == NULL)
        return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    switch (attr) {
    case HSA_EXECUTABLE_SYMBOL_INFO_TYPE:
        *(hsa_symbol_kind_t *)value =
            s->is_kernel ? HSA_SYMBOL_KIND_KERNEL : HSA_SYMBOL_KIND_VARIABLE;
        return HSA_STATUS_SUCCESS;
    case HSA_EXECUTABLE_SYMBOL_INFO_NAME_LENGTH:
        *(uint32_t *)value = (uint32_t)strlen(s->name);
        return HSA_STATUS_SUCCESS;
    case HSA_EXECUTABLE_SYMBOL_INFO_NAME:
        memcpy(value, s->name, strlen(s->name));
        return HSA_STATUS_SUCCESS;
    case HSA_EXECUTABLE_SYMBOL_INFO_MODULE_NAME_LENGTH:
        *(uint32_t *)value = 0;
        return HSA_STATUS_SUCCESS;
    case HSA_EXECUTABLE_SYMBOL_INFO_MODULE_NAME:
        return HSA_STATUS_SUCCESS;
    case HSA_EXECUTABLE_SYMBOL_INFO_AGENT:
        *(hsa_agent_t *)value = g_gpu_agent;
        return HSA_STATUS_SUCCESS;
    case HSA_EXECUTABLE_SYMBOL_INFO_LINKAGE:
        *(hsa_symbol_linkage_t *)value = HSA_SYMBOL_LINKAGE_MODULE;
        return HSA_STATUS_SUCCESS;
    case HSA_EXECUTABLE_SYMBOL_INFO_IS_DEFINITION:
        *(bool *)value = true;
        return HSA_STATUS_SUCCESS;
    default:
        break;
    }
    if (!s->is_kernel) {
        switch (attr) {
        case HSA_EXECUTABLE_SYMBOL_INFO_VARIABLE_ADDRESS:
            *(uint64_t *)value = s->address;
            return HSA_STATUS_SUCCESS;
        case HSA_EXECUTABLE_SYMBOL_INFO_VARIABLE_ALLOCATION:
            *(hsa_variable_allocation_t *)value = HSA_VARIABLE_ALLOCATION_AGENT;
            return HSA_STATUS_SUCCESS;
        case HSA_EXECUTABLE_SYMBOL_INFO_VARIABLE_SEGMENT:
            *(hsa_variable_segment_t *)value = HSA_VARIABLE_SEGMENT_GLOBAL;
            return HSA_STATUS_SUCCESS;
        case HSA_EXECUTABLE_SYMBOL_INFO_VARIABLE_ALIGNMENT:
            *(uint32_t *)value = 8;
            return HSA_STATUS_SUCCESS;
        case HSA_EXECUTABLE_SYMBOL_INFO_VARIABLE_SIZE:
            *(uint32_t *)value = (uint32_t)s->size;
            return HSA_STATUS_SUCCESS;
        case HSA_EXECUTABLE_SYMBOL_INFO_VARIABLE_IS_CONST:
            *(bool *)value = false;
            return HSA_STATUS_SUCCESS;
        default:
            return HSA_STATUS_ERROR_INVALID_ARGUMENT;
        }
    }
    switch (attr) {
    case HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_OBJECT:
        *(uint64_t *)value = s->address;
        break;
    case HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_KERNARG_SEGMENT_SIZE:
        *(uint32_t *)value = kd_of(s)->kernarg_size;
        break;
    case HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_KERNARG_SEGMENT_ALIGNMENT:
        *(uint32_t *)value = 16;
        break;
    case HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_GROUP_SEGMENT_SIZE:
        *(uint32_t *)value = kd_of(s)->group_segment_fixed_size;
        break;
    case HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_PRIVATE_SEGMENT_SIZE:
        *(uint32_t *)value = kd_of(s)->private_segment_fixed_size;
        break;
    case HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_DYNAMIC_CALLSTACK:
        *(bool *)value =
            (kd_of(s)->kernel_code_properties & KD_USES_DYNAMIC_STACK) != 0;
        break;
    case HSA_EXECUTABLE_SYMBOL_INFO_KERNEL_CALL_CONVENTION:
        *(uint32_t *)value = 0;
        break;
    default:
        return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    }
    return HSA_STATUS_SUCCESS;
}

hsa_status_t hsa_executable_iterate_symbols(
    hsa_executable_t executable,
    hsa_status_t (*cb)(hsa_executable_t, hsa_executable_symbol_t, void *),
    void *data)
{
    executable_t *x = exe_of(executable);
    symbol_t *s;

    if (x == NULL)
        return HSA_STATUS_ERROR_INVALID_EXECUTABLE;
    if (cb == NULL)
        return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    for (s = x->symbols; s != NULL; s = s->next) {
        hsa_executable_symbol_t h;
        hsa_status_t st;

        h.handle = (u64)(ULONG_PTR)s;
        st = cb(executable, h, data);
        if (st != HSA_STATUS_SUCCESS)
            return st;
    }
    return HSA_STATUS_SUCCESS;
}

typedef struct {
    hsa_status_t (*cb)(hsa_executable_t, hsa_agent_t, hsa_executable_symbol_t,
                       void *);
    hsa_agent_t agent;
    void *data;
} agent_iter;

static hsa_status_t agent_iter_cb(hsa_executable_t e,
                                  hsa_executable_symbol_t s, void *d)
{
    agent_iter *it = (agent_iter *)d;

    return it->cb(e, it->agent, s, it->data);
}

hsa_status_t hsa_executable_iterate_agent_symbols(
    hsa_executable_t executable, hsa_agent_t agent,
    hsa_status_t (*cb)(hsa_executable_t, hsa_agent_t,
                       hsa_executable_symbol_t, void *),
    void *data)
{
    agent_iter it;

    if (cb == NULL)
        return HSA_STATUS_ERROR_INVALID_ARGUMENT;
    it.cb = cb;
    it.agent = agent;
    it.data = data;
    return hsa_executable_iterate_symbols(executable, agent_iter_cb, &it);
}
