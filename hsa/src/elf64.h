/*
 * elf64.h - the few ELF64 structures the code object loader reads.
 */
#ifndef ELF64_H
#define ELF64_H

typedef struct {
    u8 e_ident[16];
    u16 e_type;
    u16 e_machine;
    u32 e_version;
    u64 e_entry;
    u64 e_phoff;
    u64 e_shoff;
    u32 e_flags;
    u16 e_ehsize;
    u16 e_phentsize;
    u16 e_phnum;
    u16 e_shentsize;
    u16 e_shnum;
    u16 e_shstrndx;
} elf_ehdr;

typedef struct {
    u32 p_type;
    u32 p_flags;
    u64 p_offset;
    u64 p_vaddr;
    u64 p_paddr;
    u64 p_filesz;
    u64 p_memsz;
    u64 p_align;
} elf_phdr;

typedef struct {
    u32 sh_name;
    u32 sh_type;
    u64 sh_flags;
    u64 sh_addr;
    u64 sh_offset;
    u64 sh_size;
    u32 sh_link;
    u32 sh_info;
    u64 sh_addralign;
    u64 sh_entsize;
} elf_shdr;

typedef struct {
    u32 st_name;
    u8 st_info;
    u8 st_other;
    u16 st_shndx;
    u64 st_value;
    u64 st_size;
} elf_sym;

typedef struct {
    u64 r_offset;
    u64 r_info;
    long long r_addend;
} elf_rela;

#define ELF_EM_AMDGPU       224
#define ELF_OSABI_AMDHSA    64
#define ELF_ET_DYN          3
#define ELF_PT_LOAD         1
#define ELF_SHT_SYMTAB      2
#define ELF_SHT_RELA        4
#define ELF_SHT_DYNSYM      11
#define ELF_STT_OBJECT      1
#define ELF_STB_LOCAL       0
#define ELF_SHN_UNDEF       0

/* AMDGPU relocation types */
#define R_AMDGPU_ABS32_LO   1
#define R_AMDGPU_ABS32_HI   2
#define R_AMDGPU_ABS64      3
#define R_AMDGPU_REL32      4
#define R_AMDGPU_REL64      5
#define R_AMDGPU_ABS32      6
#define R_AMDGPU_REL32_LO   10
#define R_AMDGPU_REL32_HI   11
#define R_AMDGPU_RELATIVE64 13

#endif
