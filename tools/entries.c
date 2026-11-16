#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static const unsigned char narrow[5] = { 0x90, 0x90, 0x90, 0x90, 0x90 };
static const unsigned char wide[5] = { 0x0f, 0x1f, 0x44, 0x00, 0x00 };

static unsigned char *file_data;
static size_t file_size;

static void fail(const char *message) {
    fprintf(stderr, "entries: %s\n", message);
    exit(1);
}

int main(int argc, char **argv) {
    if (argc < 2) fail("usage: entries FILE...");
    int rewritten = 0, wide_entries = 0, bare = 0, status = 0;
    for (int file = 1; file < argc; file++) {
        int fd = open(argv[file], O_RDWR);
        if (fd < 0) {
            fprintf(stderr, "entries: %s: %s\n", argv[file], strerror(errno));
            status = 1;
            continue;
        }
        struct stat info;
        if (fstat(fd, &info) != 0) fail("cannot stat the file");
        file_size = (size_t)info.st_size;
        file_data = mmap(NULL, file_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (file_data == MAP_FAILED) fail("cannot map the file");
        if (file_size < sizeof(Elf64_Ehdr) || memcmp(file_data, ELFMAG, SELFMAG) != 0)
            fail("not an ELF file");
        Elf64_Ehdr *header = (Elf64_Ehdr *)file_data;
        if (header->e_machine != EM_X86_64)
            fail("not an x86-64 file");
        if (header->e_shentsize != sizeof(Elf64_Shdr) || header->e_shoff == 0)
            fail("no section headers");
        Elf64_Shdr *sections = (Elf64_Shdr *)(file_data + header->e_shoff);
        Elf64_Shdr *symbols = NULL;
        for (unsigned i = 0; i < header->e_shnum; i++) {
            if (header->e_shnum > 1 && i == header->e_shstrndx) continue;
            if (sections[i].sh_type != SHT_SYMTAB) continue;
            symbols = &sections[i];
            break;
        }
        if (!symbols) fail("no symbol table; keep the binary unstripped");
        if (symbols->sh_link >= header->e_shnum) fail("bad string table link");
        Elf64_Shdr *strings = &sections[symbols->sh_link];
        const char *names = (const char *)(file_data + strings->sh_offset);
        size_t count = symbols->sh_size / sizeof(Elf64_Sym);
        Elf64_Sym *table = (Elf64_Sym *)(file_data + symbols->sh_offset);
        for (size_t i = 0; i < count; i++) {
            Elf64_Sym *symbol = &table[i];
            if (ELF64_ST_TYPE(symbol->st_info) != STT_FUNC) continue;
            if (symbol->st_shndx == SHN_UNDEF || symbol->st_shndx >= header->e_shnum) continue;
            if (symbol->st_name >= strings->sh_size) continue;
            Elf64_Shdr *section = &sections[symbol->st_shndx];
            if (!(section->sh_flags & SHF_EXECINSTR)) continue;
            if (symbol->st_value < section->sh_addr) continue;
            size_t offset = (size_t)(section->sh_offset + (symbol->st_value - section->sh_addr));
            if (offset + sizeof(wide) > file_size) fail("a function entry is past the end");
            unsigned char *entry = file_data + offset;
            if (memcmp(entry, wide, sizeof(wide)) == 0) {
                wide_entries++;
            } else if (memcmp(entry, narrow, sizeof(narrow)) == 0) {
                memcpy(entry, wide, sizeof(wide));
                rewritten++;
            } else {
                fprintf(stderr, "entries: %s: %s has no patchable prologue\n", argv[file],
                    names + symbol->st_name);
                bare++;
            }
        }
        if (msync(file_data, file_size, MS_SYNC) != 0) fail("cannot write the file");
        if (munmap(file_data, file_size) != 0) fail("cannot unmap the file");
        close(fd);
    }
    printf("entries: %d widened, %d already wide, %d without a prologue\n",
        rewritten, wide_entries, bare);
    return status;
}
