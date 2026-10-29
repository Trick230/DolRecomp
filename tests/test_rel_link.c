#include "app/module_link.h"
#include "common/types.h"

#include <stdio.h>
#include <string.h>

#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        fprintf(stderr, __VA_ARGS__); \
        fprintf(stderr, "\n"); \
        return 0; \
    } \
} while (0)

enum {
    R_PPC_ADDR32 = 1,
    R_PPC_REL24 = 10,
    R_DOLPHIN_SECTION = 202,
    R_DOLPHIN_END = 203,
};

#define A_BASE 0x80500000u
#define A_BSS 0x80600000u
#define B_BASE 0x80510000u

static u8* relocation(u8* p, u16 offset, u8 type, u8 section, u32 symbol) {
    write_be16(p, offset);
    p[2] = type;
    p[3] = section;
    write_be32(p + 4, symbol);
    return p + 8;
}

static int write_file(const char* path, const u8* data, u32 size) {
    FILE* out = fopen(path, "wb");
    if (!out)
        return 0;
    int ok = fwrite(data, 1, size, out) == size;
    return fclose(out) == 0 && ok;
}

static void header(u8* f, u32 id, u32 sections, u32 relocations, u32 imports, u32 import_size,
                   u32 bss_size, u32 prolog_offset, u32 fix_size) {
    write_be32(f + 0x00, id);
    write_be32(f + 0x0C, sections);
    write_be32(f + 0x10, 0x50);
    write_be32(f + 0x1C, 3);
    write_be32(f + 0x20, bss_size);
    write_be32(f + 0x24, relocations);
    write_be32(f + 0x28, imports);
    write_be32(f + 0x2C, import_size);
    f[0x30] = 1;  // prolog in section 1
    write_be32(f + 0x34, prolog_offset);
    write_be32(f + 0x40, 0x20);
    write_be32(f + 0x44, 4);
    write_be32(f + 0x48, fix_size);
}

// Module 1: text calls module 2 and the DOL, data points at its own text and
// bss, and at module 9, which isn't loaded.
static int write_a(const char* path, u32 id) {
    u8 f[0x1B0];
    memset(f, 0, sizeof(f));
    header(f, id, 4, 0x120, 0x188, 0x20, 0x20, 8, sizeof(f));
    write_be32(f + 0x58, 0x100 | 1u);
    write_be32(f + 0x5C, 0x10);
    write_be32(f + 0x60, 0x110);
    write_be32(f + 0x64, 0x10);
    write_be32(f + 0x6C, 0x20);  // bss

    write_be32(f + 0x100, 0x48000001u);  // bl
    write_be32(f + 0x104, 0x48000001u);  // bl
    write_be32(f + 0x108, 0x4E800020u);
    write_be32(f + 0x10C, 0x60000000u);
    write_be32(f + 0x118, 0xDEADBEEFu);

    u8* r = f + 0x120;
    r = relocation(r, 0, R_DOLPHIN_SECTION, 1, 0);  // 0x120: module 2
    r = relocation(r, 0, R_PPC_REL24, 1, 0);
    r = relocation(r, 0, R_DOLPHIN_END, 0, 0);
    r = relocation(r, 0, R_DOLPHIN_SECTION, 1, 0);  // 0x138: the DOL
    r = relocation(r, 4, R_PPC_REL24, 0, 0x80003100u);
    r = relocation(r, 0, R_DOLPHIN_END, 0, 0);
    r = relocation(r, 0, R_DOLPHIN_SECTION, 2, 0);  // 0x150: itself
    r = relocation(r, 0, R_PPC_ADDR32, 1, 4);
    r = relocation(r, 4, R_PPC_ADDR32, 3, 0);
    r = relocation(r, 0, R_DOLPHIN_END, 0, 0);
    r = relocation(r, 0, R_DOLPHIN_SECTION, 2, 0);  // 0x170: module 9
    r = relocation(r, 8, R_PPC_ADDR32, 1, 0);
    relocation(r, 0, R_DOLPHIN_END, 0, 0);

    const u32 imports[][2] = {{2, 0x120}, {0, 0x138}, {id, 0x150}, {9, 0x170}};
    for (u32 i = 0; i < 4; i++) {
        write_be32(f + 0x188 + i * 8u, imports[i][0]);
        write_be32(f + 0x18C + i * 8u, imports[i][1]);
    }
    return write_file(path, f, sizeof(f));
}

// Module 2: one leaf function.
static int write_b(const char* path) {
    u8 f[0x108];
    memset(f, 0, sizeof(f));
    header(f, 2, 2, 0, 0, 0, 0, 0, sizeof(f));
    write_be32(f + 0x58, 0x100 | 1u);
    write_be32(f + 0x5C, 8);
    write_be32(f + 0x100, 0x4E800020u);
    write_be32(f + 0x104, 0x60000000u);
    return write_file(path, f, sizeof(f));
}

static void place(RecompConfig* config, u32 index, const char* file, u32 base, u32 bss) {
    strcpy(config->rel_modules[index].file, file);
    config->rel_modules[index].base = base;
    config->rel_modules[index].bss = bss;
}

static u32 branch_target(const u8* p, u32 address) {
    const u32 word = read_be32(p);
    return address + (u32)sign_extend(word & 0x03FFFFFCu, 26);
}

static int test_link(void) {
    CHECK(write_a("test_link_a.rel", 1) && write_b("test_link_b.rel"), "can't write the RELs");
    RecompConfig config;
    memset(&config, 0, sizeof(config));
    place(&config, 0, "test_link_a.rel", A_BASE, A_BSS);
    place(&config, 1, "test_link_b.rel", B_BASE, 0);
    config.rel_module_count = 2;

    ModuleLink link = {0};
    CHECK(rel_link(&link, &config, "."), "rel_link failed");
    CHECK(link.count == 2 && link.data_count == 1, "sections: %u text, %u data", link.count,
          link.data_count);
    const LinkedSection* text = &link.text[0];
    const LinkedSection* data = &link.data[0];
    CHECK(text->address == A_BASE + 0x100 && text->size == 0x10, "A text at 0x%08X", text->address);
    CHECK(link.text[1].address == B_BASE + 0x100, "B text at 0x%08X", link.text[1].address);
    CHECK(branch_target(text->bytes, text->address) == B_BASE + 0x100, "call into module 2");
    CHECK((read_be32(text->bytes) & 1u) == 1u, "bl lost its link bit");
    CHECK(branch_target(text->bytes + 4, text->address + 4) == 0x80003100u, "call into the DOL");
    CHECK(read_be32(data->bytes) == A_BASE + 0x104, "pointer into its text: 0x%08X",
          read_be32(data->bytes));
    CHECK(read_be32(data->bytes + 4) == A_BSS, "pointer into its bss: 0x%08X",
          read_be32(data->bytes + 4));
    CHECK(read_be32(data->bytes + 8) == 0xDEADBEEFu, "import of an absent module was linked");
    CHECK(link.entry_count == 2 && link.entries[0] == A_BASE + 0x108 &&
              link.entries[1] == B_BASE + 0x100,
          "entries: %u", link.entry_count);
    module_link_free(&link);

    // A bss nobody placed, or two modules with one id, can't link.
    place(&config, 0, "test_link_a.rel", A_BASE, 0);
    CHECK(!rel_link(&link, &config, "."), "a bss without an address linked");
    module_link_free(&link);
    CHECK(write_a("test_link_a.rel", 2), "can't write the REL");
    place(&config, 0, "test_link_a.rel", A_BASE, A_BSS);
    CHECK(!rel_link(&link, &config, "."), "two modules 2 linked");
    module_link_free(&link);

    remove("test_link_a.rel");
    remove("test_link_b.rel");
    return 1;
}

int main(void) {
    if (!test_link())
        return 1;
    printf("REL link tests passed\n");
    return 0;
}
