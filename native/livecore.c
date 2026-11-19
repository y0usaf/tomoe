#include "livecore.h"

#include "backend.h"
#include "internal.h"
#include "lisp.h"
#include "main-functions.h"
#include "state.h"

#include <dlfcn.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <link.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define LIVECORE_JUMP 5
#define LIVECORE_SLOT 16
#define LIVECORE_PREFIX "livecore_"
#define LIVECORE_NEAR ((uintptr_t)1 << 31)
#define LIVECORE_IMAGES 8

struct livecore_symbol {
    char *name;
    char *section;
    unsigned ordinal;
    uintptr_t address;
    size_t size;
    bool function;
    bool patchable;
};

struct livecore_image {
    char path[PATH_MAX];
    uintptr_t bias;
    uintptr_t load_low, load_high;
    uintptr_t text_low, text_high;
    struct livecore_symbol *symbols;
    size_t count;
    unsigned char *code;
    size_t code_bytes;
    uintptr_t *slots;
    size_t slot_count;
    size_t slots_used;
    size_t id;
    bool active;
};

struct livecore_patch_slot {
    size_t image;
    uintptr_t *slot;
    uintptr_t entry;
};

struct livecore_patch {
    char *name;
    char *section;
    unsigned ordinal;
    uintptr_t target;
    size_t slot_count;
    struct livecore_patch_slot slots[LIVECORE_IMAGES];
};

struct livecore_action {
    uintptr_t entry;
    uintptr_t trampoline;
};

struct livecore_span {
    uintptr_t low, high;
    size_t index;
};

static struct livecore_image *livecore_current;
static struct livecore_patch *livecore_patches;
static size_t livecore_patch_count, livecore_patch_capacity;
static size_t livecore_next_id = 1;
static struct tomoe *livecore_server;

struct livecore_state_entry {
    char name[32];
    void *pointer;
};

static struct livecore_state_entry livecore_states[32];

static void *livecore_state_get(const char *name, size_t size) {
    for (size_t i = 0; i < 32 && livecore_states[i].name[0]; i++)
        if (!strcmp(livecore_states[i].name, name)) return livecore_states[i].pointer;
    for (size_t i = 0; i < 32; i++) {
        if (livecore_states[i].name[0]) continue;
        void *pointer = calloc(1, size ? size : 1);
        if (!pointer) return NULL;
        snprintf(livecore_states[i].name, sizeof(livecore_states[i].name), "%s", name);
        livecore_states[i].pointer = pointer;
        return pointer;
    }
    return NULL;
}

static const struct tomoe_state_abi livecore_state_abi = { livecore_state_get };

void livecore_attach_state(void) {
    tomoe_core_attach(&livecore_state_abi);
}

static bool livecore_driver_function(const char *name) {
    for (size_t i = 0; i < sizeof(livecore_driver) / sizeof(livecore_driver[0]); i++)
        if (!strcmp(livecore_driver[i], name)) return true;
    return false;
}

static size_t livecore_format(char *out, size_t size, const char *format, ...) {
    va_list args;
    va_start(args, format);
    int written = vsnprintf(out, size, format, args);
    va_end(args);
    if (written < 0) return 0;
    return (size_t)written < size ? (size_t)written : size - 1;
}

static bool livecore_prologue(const unsigned char *entry) {
    return entry[0] == 0x0f && entry[1] == 0x1f && entry[2] == 0x44 && entry[3] == 0x00 &&
        entry[4] == 0x00;
}

static int livecore_by_address(const void *left, const void *right) {
    const struct livecore_symbol *a = left, *b = right;
    int order = strcmp(a->section, b->section);
    if (order) return order;
    order = strcmp(a->name, b->name);
    if (order) return order;
    if (a->address < b->address) return -1;
    return a->address > b->address;
}

static int livecore_by_key(const void *left, const void *right) {
    const struct livecore_symbol *a = left, *b = right;
    int order = strcmp(a->name, b->name);
    if (order) return order;
    order = strcmp(a->section, b->section);
    if (order) return order;
    return a->ordinal < b->ordinal ? -1 : a->ordinal > b->ordinal;
}

static int livecore_by_span(const void *left, const void *right) {
    const struct livecore_span *a = left, *b = right;
    if (a->low < b->low) return -1;
    return a->low > b->low;
}

enum { LIVECORE_INJECT_NONE, LIVECORE_INJECT_ALLOC, LIVECORE_INJECT_PROTECT };

static int livecore_injected;

static void livecore_read_injection(void) {
    const char *path = tomoe_test_flag_value("swap-fail");
    livecore_injected = LIVECORE_INJECT_NONE;
    if (!path) return;
    FILE *file = fopen(path, "r");
    if (!file) return;
    char text[32] = { 0 };
    if (!fgets(text, sizeof(text), file)) text[0] = '\0';
    fclose(file);
    unlink(path);
    if (strstr(text, "alloc")) livecore_injected = LIVECORE_INJECT_ALLOC;
    else if (strstr(text, "protect")) livecore_injected = LIVECORE_INJECT_PROTECT;
}

static bool livecore_alloc_fails;

static char *livecore_copy(const char *text) {
    size_t length = strlen(text);
    if (livecore_alloc_fails) return NULL;
    char *copy = malloc(length + 1);
    if (copy) memcpy(copy, text, length + 1);
    return copy;
}

static void livecore_release(struct livecore_image *image) {
    for (size_t i = 0; i < image->count; i++) {
        free(image->symbols[i].name);
        free(image->symbols[i].section);
    }
    free(image->symbols);
    image->symbols = NULL;
    image->count = 0;
}

static bool livecore_read_symbols(struct livecore_image *image, char *error, size_t error_size) {
    int fd = open(image->path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        livecore_format(error, error_size, "cannot open %s: %s", image->path, strerror(errno));
        return false;
    }
    struct stat info;
    if (fstat(fd, &info) != 0 || info.st_size < (off_t)sizeof(Elf64_Ehdr)) {
        livecore_format(error, error_size, "%s is not an ELF file", image->path);
        close(fd);
        return false;
    }
    size_t size = (size_t)info.st_size;
    void *data = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (data == MAP_FAILED) {
        livecore_format(error, error_size, "cannot map %s", image->path);
        return false;
    }
    const Elf64_Ehdr *header = data;
    const Elf64_Shdr *symbols = NULL;
    const Elf64_Shdr *sections = NULL;
    const Elf64_Sym *table = NULL;
    const char *names = NULL;
    const char *section_names = NULL;
    size_t count = 0;
    if (memcmp(header->e_ident, ELFMAG, SELFMAG) != 0 || header->e_machine != EM_X86_64 ||
            header->e_shentsize != sizeof(Elf64_Shdr) || !header->e_shoff || !header->e_shnum ||
            header->e_shoff + (size_t)header->e_shnum * sizeof(Elf64_Shdr) > size) {
        livecore_format(error, error_size, "%s is not a whole x86-64 ELF image", image->path);
        goto failed;
    }
    sections = (const Elf64_Shdr *)((const char *)data + header->e_shoff);
    for (unsigned i = 0; i < header->e_shnum; i++)
        if (sections[i].sh_type == SHT_SYMTAB) {
            symbols = &sections[i];
            break;
        }
    if (!symbols || symbols->sh_link >= header->e_shnum) {
        livecore_format(error, error_size, "%s has no symbol table", image->path);
        goto failed;
    }
    if (sections[symbols->sh_link].sh_offset + sections[symbols->sh_link].sh_size > size ||
            symbols->sh_offset + symbols->sh_size > size ||
            (header->e_shstrndx < header->e_shnum &&
             sections[header->e_shstrndx].sh_offset + sections[header->e_shstrndx].sh_size > size)) {
        livecore_format(error, error_size, "%s has a truncated symbol table", image->path);
        goto failed;
    }
    if (header->e_shstrndx < header->e_shnum)
        section_names = (const char *)data + sections[header->e_shstrndx].sh_offset;
    names = (const char *)data + sections[symbols->sh_link].sh_offset;
    table = (const Elf64_Sym *)((const char *)data + symbols->sh_offset);
    count = symbols->sh_size / sizeof(Elf64_Sym);
    struct livecore_symbol *found = calloc(count + 1, sizeof(*found));
    if (!found) {
        livecore_format(error, error_size, "cannot hold %zu symbols", count);
        goto failed;
    }
    size_t used = 0;
    for (size_t i = 0; i < count; i++) {
        const Elf64_Sym *symbol = &table[i];
        unsigned type = ELF64_ST_TYPE(symbol->st_info);
        if (type != STT_FUNC && type != STT_OBJECT) continue;
        if (symbol->st_shndx == SHN_UNDEF || symbol->st_shndx >= header->e_shnum) continue;
        if (symbol->st_name >= sections[symbols->sh_link].sh_size) continue;
        const Elf64_Shdr *section = &sections[symbol->st_shndx];
        if (type == STT_FUNC && !(section->sh_flags & SHF_EXECINSTR)) continue;
        if (type == STT_OBJECT && !(section->sh_flags & SHF_WRITE)) continue;
        const char *name = names + symbol->st_name;
        const char *section_name = section_names ? section_names + section->sh_name : "";
        if (!name[0] || strlen(name) > 512 || strlen(section_name) > 512) continue;
        found[used].name = livecore_copy(name);
        found[used].section = livecore_copy(section_name);
        if (!found[used].name || !found[used].section) {
            for (size_t j = 0; j <= used; j++) {
                free(found[j].name);
                free(found[j].section);
            }
            free(found);
            livecore_format(error, error_size, "cannot copy the names of %s", image->path);
            goto failed;
        }
        found[used].address = image->bias + symbol->st_value;
        found[used].size = symbol->st_size;
        found[used].function = type == STT_FUNC;
        found[used].patchable = found[used].function && found[used].size >= LIVECORE_JUMP &&
            livecore_prologue((const unsigned char *)found[used].address);
        used++;
    }
    munmap(data, size);
    qsort(found, used, sizeof(*found), livecore_by_address);
    for (size_t i = 1; i < used; i++)
        if (!strcmp(found[i].name, found[i - 1].name) &&
                !strcmp(found[i].section, found[i - 1].section))
            found[i].ordinal = found[i - 1].ordinal + 1;
    qsort(found, used, sizeof(*found), livecore_by_key);
    livecore_release(image);
    image->symbols = found;
    image->count = used;
    return true;
failed:
    munmap(data, size);
    return false;
}

static const struct livecore_symbol *livecore_find(const struct livecore_image *image,
        const struct livecore_symbol *key) {
    size_t low = 0, high = image->count;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        const struct livecore_symbol *candidate = &image->symbols[middle];
        int order = strcmp(key->name, candidate->name);
        if (!order) order = strcmp(key->section, candidate->section);
        if (!order) {
            if (key->ordinal < candidate->ordinal) order = -1;
            else if (key->ordinal > candidate->ordinal) order = 1;
        }
        if (!order) return candidate;
        if (order < 0) high = middle;
        else low = middle + 1;
    }
    return NULL;
}

struct livecore_lookup {
    const char *path;
    struct livecore_image *image;
    bool found;
};

static int livecore_phdr(struct dl_phdr_info *info, size_t size, void *data) {
    (void)size;
    struct livecore_lookup *lookup = data;
    const char *name = info->dlpi_name ? info->dlpi_name : "";
    if (lookup->found) return 0;
    if (lookup->path) {
        char real[PATH_MAX];
        if (!name[0] || !realpath(name, real) || strcmp(real, lookup->path) != 0) return 0;
    } else if (name[0]) {
        return 0;
    }
    struct livecore_image *image = lookup->image;
    image->bias = (uintptr_t)info->dlpi_addr;
    image->load_low = UINTPTR_MAX;
    image->text_low = UINTPTR_MAX;
    for (unsigned i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr) *program = &info->dlpi_phdr[i];
        if (program->p_type != PT_LOAD) continue;
        uintptr_t low = image->bias + program->p_vaddr;
        uintptr_t high = low + program->p_memsz;
        if (low < image->load_low) image->load_low = low;
        if (high > image->load_high) image->load_high = high;
        if (!(program->p_flags & PF_X)) continue;
        if (low < image->text_low) image->text_low = low;
        if (high > image->text_high) image->text_high = high;
    }
    lookup->found = image->load_low < image->load_high;
    return lookup->found ? 1 : 0;
}

static bool livecore_locate(struct livecore_image *image, const char *path, char *error,
        size_t error_size) {
    struct livecore_lookup lookup = { path, image, false };
    dl_iterate_phdr(livecore_phdr, &lookup);
    if (!lookup.found) {
        livecore_format(error, error_size, "cannot find %s in memory",
            path ? path : "the executable");
        return false;
    }
    return true;
}

static void *livecore_map_near(uintptr_t low, uintptr_t high, size_t bytes) {
    size_t length = (bytes + 0x1fffff) & ~(size_t)0x1fffff;
    for (uintptr_t step = 0; step < LIVECORE_NEAR; step += 0x200000) {
        uintptr_t candidates[2];
        candidates[0] = (low - 0x200000 - step) & ~(uintptr_t)0x1fffff;
        candidates[1] = step ? (high + 0x10000000 + step + 0x1fffff) & ~(uintptr_t)0x1fffff : 0;
        for (int i = 0; i < 2; i++) {
            uintptr_t candidate = candidates[i];
            if (!candidate || candidate < 0x1000000) continue;
            if (candidate >= low && candidate < high) continue;
            if (candidate < low && low - candidate > LIVECORE_NEAR) continue;
            if (candidate >= high && candidate - high > LIVECORE_NEAR) continue;
            void *mapped = mmap((void *)candidate, length, PROT_READ | PROT_WRITE | PROT_EXEC,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE | MAP_NORESERVE, -1, 0);
            if (mapped == (void *)candidate) return mapped;
            if (mapped != MAP_FAILED) munmap(mapped, length);
        }
    }
    return NULL;
}

static bool livecore_window(struct livecore_image *image, size_t wanted, char *error,
        size_t error_size) {
    if (image->code && image->slot_count - image->slots_used >= wanted) return true;
    size_t total = image->slots_used + wanted + 4096;
    size_t code_bytes = (total * LIVECORE_SLOT + 0xfff) & ~(size_t)0xfff;
    size_t slot_bytes = (total * 8 + 0xfff) & ~(size_t)0xfff;
    unsigned char *code = livecore_map_near(image->load_low, image->load_high,
        code_bytes + slot_bytes);
    if (!code) {
        livecore_format(error, error_size, "cannot place trampolines near %s", image->path);
        return false;
    }
    uintptr_t *slots = (uintptr_t *)(code + code_bytes);
    if (image->code) {
        memcpy(code, image->code, image->slots_used * LIVECORE_SLOT);
        memcpy(slots, image->slots, image->slots_used * 8);
        for (size_t i = 0; i < livecore_patch_count; i++) {
            struct livecore_patch *patch = &livecore_patches[i];
            for (size_t j = 0; j < patch->slot_count; j++) {
                uintptr_t *slot = patch->slots[j].slot;
                if (slot < image->slots || slot >= image->slots + image->slot_count) continue;
                size_t index = (size_t)(slot - image->slots);
                uintptr_t *moved = slots + index;
                int32_t relative = (int32_t)((uintptr_t)moved -
                    (uintptr_t)(code + index * LIVECORE_SLOT + 6));
                memcpy(code + index * LIVECORE_SLOT + 2, &relative, sizeof(relative));
                patch->slots[j].slot = moved;
            }
        }
    }
    image->code = code;
    image->code_bytes = code_bytes + slot_bytes;
    image->slots = slots;
    image->slot_count = total;
    return true;
}

static uintptr_t livecore_slot(struct livecore_image *image, uintptr_t target) {
    size_t index = image->slots_used++;
    unsigned char *trampoline = image->code + index * LIVECORE_SLOT;
    uintptr_t *address = image->slots + index;
    int32_t relative = (int32_t)((uintptr_t)address - (uintptr_t)(trampoline + 6));
    trampoline[0] = 0xff;
    trampoline[1] = 0x25;
    memcpy(trampoline + 2, &relative, sizeof(relative));
    memset(trampoline + 6, 0, LIVECORE_SLOT - 6);
    __atomic_store_n(address, target, __ATOMIC_SEQ_CST);
    return (uintptr_t)trampoline;
}

static bool livecore_patch_entry(uintptr_t entry, uintptr_t trampoline, char *error,
        size_t error_size) {
    uintptr_t aligned = entry & ~(uintptr_t)7;
    size_t shift = entry - aligned;
    if (shift + LIVECORE_JUMP > 8) {
        livecore_format(error, error_size, "the entry at %p cannot be patched in one store",
            (void *)entry);
        return false;
    }
    intptr_t delta = (intptr_t)trampoline - (intptr_t)(entry + LIVECORE_JUMP);
    if (delta > INT32_MAX || delta < INT32_MIN) {
        livecore_format(error, error_size, "the trampoline is too far from %p", (void *)entry);
        return false;
    }
    uint64_t word;
    memcpy(&word, (const void *)aligned, sizeof(word));
    unsigned char bytes[8];
    memcpy(bytes, &word, sizeof(bytes));
    int32_t jump = (int32_t)delta;
    bytes[shift] = 0xe9;
    memcpy(bytes + shift + 1, &jump, sizeof(jump));
    memcpy(&word, bytes, sizeof(word));
    __atomic_store_n((uint64_t *)(void *)aligned, word, __ATOMIC_SEQ_CST);
    return true;
}

static struct livecore_patch *livecore_patch_for(const struct livecore_symbol *symbol) {
    for (size_t i = 0; i < livecore_patch_count; i++) {
        struct livecore_patch *patch = &livecore_patches[i];
        if (patch->ordinal == symbol->ordinal && !strcmp(patch->name, symbol->name) &&
                !strcmp(patch->section, symbol->section))
            return patch;
    }
    return NULL;
}

static bool livecore_reserve(size_t wanted) {
    while (livecore_patch_capacity < wanted) {
        size_t capacity = livecore_patch_capacity ? livecore_patch_capacity * 2 : 1024;
        struct livecore_patch *grown = realloc(livecore_patches, capacity * sizeof(*grown));
        if (!grown) return false;
        livecore_patches = grown;
        livecore_patch_capacity = capacity;
    }
    return true;
}

static struct livecore_patch *livecore_record(const struct livecore_symbol *symbol) {
    struct livecore_patch *patch = livecore_patch_for(symbol);
    if (patch) return patch;
    if (!livecore_reserve(livecore_patch_count + 1)) return NULL;
    patch = &livecore_patches[livecore_patch_count];
    patch->name = livecore_copy(symbol->name);
    patch->section = livecore_copy(symbol->section);
    if (!patch->name || !patch->section) {
        free(patch->name);
        free(patch->section);
        return NULL;
    }
    patch->ordinal = symbol->ordinal;
    patch->slot_count = 0;
    livecore_patch_count++;
    return patch;
}

static uintptr_t *livecore_slot_for(const struct livecore_patch *patch, size_t image) {
    if (!patch) return NULL;
    for (size_t i = 0; i < patch->slot_count; i++)
        if (patch->slots[i].image == image) return patch->slots[i].slot;
    return NULL;
}

static bool livecore_add_slot(struct livecore_patch *patch, size_t image, uintptr_t *slot,
        uintptr_t entry) {
    if (patch->slot_count >= LIVECORE_IMAGES) return false;
    patch->slots[patch->slot_count].image = image;
    patch->slots[patch->slot_count].slot = slot;
    patch->slots[patch->slot_count].entry = entry;
    patch->slot_count++;
    return true;
}

static int livecore_compare_page(const void *left, const void *right) {
    uintptr_t a = *(const uintptr_t *)left, b = *(const uintptr_t *)right;
    return a < b ? -1 : a > b;
}

static size_t livecore_pages(const struct livecore_image *image, uintptr_t **pages) {
    long page = sysconf(_SC_PAGESIZE);
    size_t count = 0;
    for (size_t i = 0; i < image->count; i++) {
        const struct livecore_symbol *symbol = &image->symbols[i];
        if (!symbol->function || !symbol->patchable) continue;
        if (strncmp(symbol->name, LIVECORE_PREFIX, strlen(LIVECORE_PREFIX)) == 0) continue;
        count++;
    }
    uintptr_t *found = calloc(count + 1, sizeof(*found));
    if (!found) return 0;
    size_t used = 0;
    for (size_t i = 0; i < image->count; i++) {
        const struct livecore_symbol *symbol = &image->symbols[i];
        if (!symbol->function || !symbol->patchable) continue;
        if (strncmp(symbol->name, LIVECORE_PREFIX, strlen(LIVECORE_PREFIX)) == 0) continue;
        found[used++] = symbol->address & ~(uintptr_t)(page - 1);
    }
    qsort(found, used, sizeof(*found), livecore_compare_page);
    size_t unique = 0;
    for (size_t i = 0; i < used; i++)
        if (!unique || found[i] != found[unique - 1]) found[unique++] = found[i];
    *pages = found;
    return unique;
}

static bool livecore_make_writable(uintptr_t low, uintptr_t high, bool executable, char *error,
        size_t error_size) {
    long page = sysconf(_SC_PAGESIZE);
    uintptr_t start = low & ~(uintptr_t)(page - 1);
    uintptr_t end = (high + (uintptr_t)page - 1) & ~(uintptr_t)(page - 1);
    int protection = PROT_READ | PROT_WRITE | (executable ? PROT_EXEC : 0);
    if (start >= end) return true;
    if (mprotect((void *)start, end - start, protection) != 0) {
        livecore_format(error, error_size, "cannot write memory at %p: %s", (void *)low,
            strerror(errno));
        return false;
    }
    return true;
}

static size_t livecore_objects(const struct livecore_image *image, struct livecore_span **spans) {
    size_t count = 0;
    for (size_t i = 0; i < image->count; i++)
        if (!image->symbols[i].function && image->symbols[i].size) count++;
    struct livecore_span *found = calloc(count + 1, sizeof(*found));
    if (!found) return 0;
    size_t used = 0;
    for (size_t i = 0; i < image->count; i++) {
        const struct livecore_symbol *symbol = &image->symbols[i];
        if (symbol->function || !symbol->size) continue;
        found[used].low = symbol->address;
        found[used].high = symbol->address + symbol->size;
        found[used].index = i;
        used++;
    }
    qsort(found, used, sizeof(*found), livecore_by_span);
    *spans = found;
    return used;
}

static size_t livecore_kept(const struct livecore_image *image) {
    size_t count = 0;
    for (size_t i = 0; i < image->count; i++)
        if (image->symbols[i].function && !image->symbols[i].patchable) count++;
    return count;
}

static bool livecore_start(char *error, size_t error_size) {
    if (livecore_current) return true;
    struct livecore_image *image = calloc(1, sizeof(*image));
    if (!image) {
        livecore_format(error, error_size, "cannot hold the running core's record");
        return false;
    }
    char resolved[PATH_MAX];
    if (!realpath("/proc/self/exe", resolved)) {
        livecore_format(error, error_size, "cannot resolve /proc/self/exe: %s", strerror(errno));
        return false;
    }
    snprintf(image->path, sizeof(image->path), "%s", resolved);
    if (!livecore_locate(image, NULL, error, error_size)) return false;
    if (!livecore_read_symbols(image, error, error_size)) return false;
    image->id = livecore_next_id++;
    image->active = true;
    livecore_current = image;
    return true;
}

struct livecore_repoint {
    const struct livecore_image *from;
    const struct livecore_image *to;
    const struct livecore_span *spans;
    size_t count;
};

static const void *livecore_resolve(const void *implementation, void *data) {
    struct livecore_repoint *repoint = data;
    uintptr_t address = (uintptr_t)implementation;
    if (!address || !repoint->count) return NULL;
    size_t low = 0, high = repoint->count;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        if (repoint->spans[middle].low <= address) low = middle + 1;
        else high = middle;
    }
    if (!low) return NULL;
    const struct livecore_span *span = &repoint->spans[low - 1];
    if (address >= span->high) return NULL;
    const struct livecore_symbol *symbol = &repoint->from->symbols[span->index];
    const struct livecore_symbol *pair = livecore_find(repoint->to, symbol);
    if (!pair || pair->size != span->high - span->low) return NULL;
    return (const void *)(pair->address + (address - span->low));
}

struct livecore_plan {
    struct livecore_image *next;
    char real[PATH_MAX];
    size_t functions, objects, bytes, fresh;
    struct livecore_span *objects_table;
    size_t objects_count;
    uintptr_t *pages;
    size_t page_count;
    struct livecore_action *actions;
    size_t action_count;
};


static bool livecore_protect(struct livecore_plan *plan, char *error, size_t error_size);
static bool livecore_arm(struct livecore_plan *plan, char *error, size_t error_size);

static struct livecore_plan *livecore_pending;

static void livecore_drop_plan(struct livecore_plan *plan) {
    if (!plan) return;
    free(plan->objects_table);
    free(plan->pages);
    free(plan->actions);
    free(plan);
}

static void livecore_release_plan(struct livecore_plan *plan) {
    if (!plan) return;
    if (plan->next) {
        livecore_release(plan->next);
        free(plan->next);
    }
    free(plan->objects_table);
    free(plan->pages);
    free(plan->actions);
    free(plan);
}

static struct livecore_plan *livecore_prepare(const char *path) {
    char error[512] = "";
    if (livecore_pending) lisp_error("A core swap is already waiting for the next frame.");
    if (!path || !*path) lisp_error("A core swap needs a path.");
    if (!livecore_start(error, sizeof(error))) lisp_error("%s.", error);
    struct livecore_image *current = livecore_current;
    struct livecore_plan *plan = calloc(1, sizeof(*plan));
    if (!plan) lisp_error("Cannot hold the swap plan.");
    livecore_read_injection();
    if (!realpath(path, plan->real)) {
        struct livecore_plan *failed = plan;
        livecore_release_plan(failed);
        lisp_error("Cannot find the core %s: %s.", path, strerror(errno));
    }
    void *handle = dlopen(plan->real, RTLD_NOW | RTLD_LOCAL);
    if (handle) {
        void (*attach)(const struct tomoe_state_abi *) =
            (void (*)(const struct tomoe_state_abi *))dlsym(handle, "tomoe_core_attach");
        if (attach) attach(&livecore_state_abi);
    }
    const char *(*abi)(void) = NULL;
    if (handle) abi = (const char *(*)(void))dlsym(handle, "tomoe_core_abi");
    if (!handle || !abi) {
        if (handle) dlclose(handle);
        livecore_release_plan(plan);
        lisp_error("That library is not a Tomoe core.");
    }
    if (strcmp(abi(), tomoe_core_abi()) != 0) {
        char mismatch[256];
        livecore_format(mismatch, sizeof(mismatch),
            "That core was built for another layout (%s, running %s); restart to load it.",
            abi(), tomoe_core_abi());
        dlclose(handle);
        livecore_release_plan(plan);
        lisp_error("%s", mismatch);
    }
    if (livecore_next_id > LIVECORE_IMAGES) {
        dlclose(handle);
        livecore_release_plan(plan);
        lisp_error("This session has swapped its core too many times to keep the old ones.");
    }
    struct livecore_image *next = calloc(1, sizeof(*next));
    if (!next) {
        dlclose(handle);
        livecore_release_plan(plan);
        lisp_error("Cannot hold the new core's record.");
    }
    plan->next = next;
    snprintf(next->path, sizeof(next->path), "%s", plan->real);
    if (!livecore_locate(next, plan->real, error, sizeof(error)) ||
            !livecore_read_symbols(next, error, sizeof(error))) {
        dlclose(handle);
        livecore_release_plan(plan);
        lisp_error("%s.", error);
    }
    size_t fresh = 0;
    bool ready = false;
    for (size_t i = 0; i < current->count; i++) {
        const struct livecore_symbol *symbol = &current->symbols[i];
        const struct livecore_symbol *pair = livecore_find(next, symbol);
        if (symbol->function) {
            if (!symbol->patchable) continue;
            if (livecore_driver_function(symbol->name)) continue;
            plan->functions++;
            if (!pair) {
                livecore_format(error, sizeof(error), "%s is gone from this build; restart to load it",
                    symbol->name);
                goto failed;
            }
            if (!pair->patchable) {
                livecore_format(error, sizeof(error), "%s has no patchable entry in the new core",
                    pair->name);
                goto failed;
            }
            if (strncmp(symbol->name, LIVECORE_PREFIX, strlen(LIVECORE_PREFIX)) == 0) continue;
            if (!livecore_slot_for(livecore_patch_for(symbol), current->id)) fresh++;
            continue;
        }
        if (!symbol->size || strstr(symbol->section, ".rel.ro")) continue;
        if (!pair || pair->size != symbol->size) {
            livecore_format(error, sizeof(error), "the data at %s does not match this build; "
                "restart to load it", symbol->name);
            goto failed;
        }
    }
    plan->fresh = fresh;
    plan->objects_count = livecore_objects(current, &plan->objects_table);
    if (!livecore_window(current, fresh, error, sizeof(error))) goto failed;
    plan->page_count = livecore_pages(current, &plan->pages);
    long page = sysconf(_SC_PAGESIZE);
    if (fresh && (!plan->pages || !plan->page_count)) {
        livecore_format(error, sizeof(error), "cannot list the code to patch");
        goto failed;
    }
    size_t upcoming = current->slots_used;
    for (size_t i = 0; i < current->count; i++) {
        const struct livecore_symbol *symbol = &current->symbols[i];
        if (!symbol->function || !symbol->patchable) continue;
        if (livecore_driver_function(symbol->name)) continue;
        if (strncmp(symbol->name, LIVECORE_PREFIX, strlen(LIVECORE_PREFIX)) == 0) continue;
        if (livecore_slot_for(livecore_patch_for(symbol), current->id)) continue;
        uintptr_t entry = symbol->address;
        uintptr_t trampoline = (uintptr_t)current->code + upcoming * LIVECORE_SLOT;
        if ((entry & 7) + LIVECORE_JUMP > 8) {
            livecore_format(error, sizeof(error), "the entry at %p cannot be patched in one store",
                (void *)entry);
            goto failed;
        }
        intptr_t delta = (intptr_t)trampoline - (intptr_t)(entry + LIVECORE_JUMP);
        if (delta > INT32_MAX || delta < INT32_MIN) {
            livecore_format(error, sizeof(error), "the trampoline is too far from %p", (void *)entry);
            goto failed;
        }
        upcoming++;
    }
    if (!livecore_reserve(livecore_patch_count + fresh)) {
        livecore_format(error, sizeof(error), "cannot hold %zu patches", fresh);
        goto failed;
    }
    if (!livecore_protect(plan, error, sizeof(error))) goto failed;
    livecore_alloc_fails = livecore_injected == LIVECORE_INJECT_ALLOC;
    if (!livecore_arm(plan, error, sizeof(error))) {
        livecore_alloc_fails = false;
        goto failed;
    }
    livecore_alloc_fails = false;
    (void)page;
    ready = true;
failed:
    if (!ready) {
        long size = sysconf(_SC_PAGESIZE);
        for (size_t i = 0; i < plan->page_count; i++)
            mprotect((void *)plan->pages[i], (size_t)size, PROT_READ | PROT_EXEC);
        dlclose(handle);
        livecore_release_plan(plan);
        lisp_error("%s.", error[0] ? error : "The core was not swapped");
    }
    return plan;
}

static const void *livecore_resolve_table(const void *table, struct livecore_repoint *repoint) {
    if (!table) return NULL;
    const void *moved = livecore_resolve(table, repoint);
    return moved ? moved : table;
}

static bool livecore_protect(struct livecore_plan *plan, char *error, size_t error_size) {
    struct livecore_image *current = livecore_current;
    struct livecore_image *next = plan->next;
    long page = sysconf(_SC_PAGESIZE);
    if (livecore_injected == LIVECORE_INJECT_PROTECT) {
        int status = mprotect((void *)page, (size_t)page, PROT_READ | PROT_WRITE);
        livecore_format(error, error_size, "cannot make the code writable: %s",
            status == 0 ? "injected failure" : strerror(errno));
        return false;
    }
    for (size_t i = 0; i < current->count; i++) {
        const struct livecore_symbol *symbol = &current->symbols[i];
        if (symbol->function || !symbol->size || strstr(symbol->section, ".rel.ro")) continue;
        const struct livecore_symbol *pair = livecore_find(next, symbol);
        if (!livecore_make_writable(pair->address, pair->size, false, error, error_size))
            return false;
    }
    for (size_t i = 0; i < plan->page_count; i++) {
        if (mprotect((void *)plan->pages[i], (size_t)page, PROT_READ | PROT_WRITE | PROT_EXEC) == 0)
            continue;
        livecore_format(error, error_size, "cannot write code at %p: %s",
            (void *)plan->pages[i], strerror(errno));
        return false;
    }
    return true;
}

static bool livecore_arm(struct livecore_plan *plan, char *error, size_t error_size) {
    struct livecore_image *current = livecore_current;
    struct livecore_image *next = plan->next;
    for (size_t i = 0; i < current->count; i++) {
        const struct livecore_symbol *symbol = &current->symbols[i];
        if (!symbol->function || !symbol->patchable) continue;
        if (livecore_driver_function(symbol->name)) continue;
        if (strncmp(symbol->name, LIVECORE_PREFIX, strlen(LIVECORE_PREFIX)) == 0) continue;
        const struct livecore_symbol *pair = livecore_find(next, symbol);
        struct livecore_patch *record = livecore_record(symbol);
        if (!record) {
            livecore_format(error, error_size, "cannot remember the patch for %s", symbol->name);
            return false;
        }
        record->target = pair->address;
        if (livecore_slot_for(record, current->id)) continue;
        uintptr_t trampoline = livecore_slot(current, pair->address);
        if (!livecore_add_slot(record, current->id, current->slots + current->slots_used - 1,
                symbol->address)) {
            livecore_format(error, error_size, "cannot add a trampoline for %s", symbol->name);
            return false;
        }
        if (plan->action_count % 128 == 0) {
            struct livecore_action *grown = realloc(plan->actions,
                (plan->action_count + 128) * sizeof(*grown));
            if (!grown) {
                livecore_format(error, error_size, "cannot hold %zu entries to patch",
                    plan->action_count + 1);
                return false;
            }
            plan->actions = grown;
        }
        plan->actions[plan->action_count].entry = symbol->address;
        plan->actions[plan->action_count].trampoline = trampoline;
        plan->action_count++;
    }
    return true;
}

static void livecore_commit(struct livecore_plan *plan) {
    struct livecore_image *current = livecore_current;
    struct livecore_image *next = plan->next;
    for (size_t i = 0; i < current->count; i++) {
        const struct livecore_symbol *symbol = &current->symbols[i];
        if (symbol->function || !symbol->size || strstr(symbol->section, ".rel.ro")) continue;
        const struct livecore_symbol *pair = livecore_find(next, symbol);
        memcpy((void *)pair->address, (const void *)symbol->address, symbol->size);
        plan->objects++;
        plan->bytes += symbol->size;
    }
    if (plan->objects_count && livecore_server && livecore_server->display) {
        struct livecore_repoint repoint = { current, next, plan->objects_table,
            plan->objects_count };
        wl_display_repoint(livecore_server->display, livecore_resolve, &repoint);
        struct surface *surface;
        wl_list_for_each(surface, &livecore_server->surfaces, link)
            surface->role = livecore_resolve_table(surface->role, &repoint);
        struct output *output;
        wl_list_for_each(output, &livecore_server->outputs, link)
            if (output->screen)
                output->screen->impl = livecore_resolve_table(output->screen->impl, &repoint);
    }
    for (size_t i = 0; i < livecore_patch_count; i++) {
        struct livecore_patch *record = &livecore_patches[i];
        for (size_t j = 0; j < record->slot_count; j++)
            __atomic_store_n(record->slots[j].slot, record->target, __ATOMIC_SEQ_CST);
    }
    char error[128];
    for (size_t i = 0; i < plan->action_count; i++)
        if (!livecore_patch_entry(plan->actions[i].entry, plan->actions[i].trampoline,
                error, sizeof(error)))
            tomoe_log(LOG_ERROR, "tomoe: %s", error);

    long page = sysconf(_SC_PAGESIZE);
    for (size_t i = 0; i < plan->page_count; i++)
        if (mprotect((void *)plan->pages[i], (size_t)page, PROT_READ | PROT_EXEC) != 0)
            tomoe_log(LOG_WARNING, "tomoe: the code at %p stays writable", (void *)plan->pages[i]);
    next->id = livecore_next_id++;
    next->active = true;
    livecore_current = next;
    tomoe_log(LOG_DEBUG, "tomoe: core swapped to %s (%s): %zu functions, %zu entries patched, "
        "%zu data symbols (%zu bytes), %zu kept, %zu left behind",
        plan->real, tomoe_core_stamp(), plan->functions, plan->action_count, plan->objects,
        plan->bytes, livecore_kept(next), livecore_kept(current));
    livecore_drop_plan(plan);
}

void livecore_service(void) {
    struct livecore_plan *plan = livecore_pending;
    if (!plan) return;
    livecore_pending = NULL;
    livecore_commit(plan);
}

static lisp_value livecore_prim_core_swap(int argc, lisp_value *argv) {
    (void)argc;
    size_t length = 0;
    const char *path = lisp_get_string(argv[0], &length);
    (void)length;
    if (!path) lisp_error("The core path must be a string.");
    if (!livecore_start(NULL, 0)) lisp_error("No core image is running.");
    livecore_pending = livecore_prepare(path);
    return lisp_t();
}

static lisp_value livecore_prim_core_stamp(int argc, lisp_value *argv) {
    (void)argc;
    (void)argv;
    return lisp_cstring(tomoe_core_stamp());
}

static lisp_value livecore_prim_core_abi(int argc, lisp_value *argv) {
    (void)argc;
    (void)argv;
    return lisp_cstring(tomoe_core_abi());
}

bool livecore_init(struct tomoe *server, char *error, size_t error_size) {
    (void)error;
    (void)error_size;
    livecore_server = server;
    lisp_define_primitive("%core-swap", livecore_prim_core_swap, 1, 1);
    lisp_define_primitive("%core-stamp", livecore_prim_core_stamp, 0, 0);
    lisp_define_primitive("%core-abi", livecore_prim_core_abi, 0, 0);
    return true;
}
