#include "app/module_link.h"
#include "common/types.h"

#include <stdio.h>
#include <stdlib.h>
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
};

#define STATIC_TEXT 0x80004000u
#define RESIDENT_BASE 0x80010000u
#define OVERLAY_BASE 0x80020000u
#define OVERLAY_BSS 0x8001FF00u
#define HEAP_TOP 0x80030000u
#define BSS_SIZE 0x24u

// Every module has the same layout: the header, a table of sections 1 text
// (0x100, 16 bytes), 2 data (0x110, 8 bytes) and 3 bss, then its relocations,
// exports, imports and name.
#define FILE_SIZE 0x1E0u
#define TABLE 0x58u
#define INTERNALS 0x120u
#define EXTERNALS 0x140u
#define EXPORTS 0x160u
#define EXPORT_NAMES 0x180u
#define IMPORTS 0x190u
#define IMPORT_NAMES 0x1B0u
#define NAME 0x1C0u

static int write_file(const char* path, const u8* data, u32 size) {
    FILE* out = fopen(path, "wb");
    if (!out)
        return 0;
    int ok = fwrite(data, 1, size, out) == size;
    return fclose(out) == 0 && ok;
}

static void entry12(u8* p, u32 a, u32 b, u32 c) {
    write_be32(p, a);
    write_be32(p + 4, b);
    write_be32(p + 8, c);
}

static void module_header(u8* f, u32 bss_size, u32 internals, u32 externals, u32 exports, u32 imports) {
    memset(f, 0, FILE_SIZE);
    write_be32(f + 0x08, 4);
    write_be32(f + 0x0C, TABLE);
    write_be32(f + 0x10, NAME);
    write_be32(f + 0x14, 2);
    write_be32(f + 0x1C, bss_size);
    f[0x20] = f[0x21] = f[0x22] = 1;  // prolog, epilog and unresolved in the text
    write_be32(f + 0x2C, 0xC);        // the unresolved stub: the last word
    write_be32(f + 0x30, INTERNALS);
    write_be32(f + 0x34, internals * 12u);
    write_be32(f + 0x38, EXTERNALS);
    write_be32(f + 0x3C, externals * 12u);
    write_be32(f + 0x40, EXPORTS);
    write_be32(f + 0x44, exports * 16u);
    write_be32(f + 0x48, EXPORT_NAMES);
    write_be32(f + 0x4C, IMPORTS);
    write_be32(f + 0x50, imports * 12u);
    write_be32(f + 0x54, IMPORT_NAMES);
    write_be32(f + TABLE + 8, 0x100);
    write_be32(f + TABLE + 12, 0x10);
    write_be32(f + TABLE + 16, 0x110);
    write_be32(f + TABLE + 20, 8);
    write_be32(f + TABLE + 28, bss_size);
    for (u32 i = 0; i < 4; i++)
        write_be32(f + 0x100 + i * 4u, i == 2 ? 0x4E800020u : 0x60000000u);
    f[NAME] = 'm';
}

static void add_export(u8* f, u32 index, const char* name, u32 offset) {
    strcpy((char*)f + EXPORT_NAMES + index * 4u, name);
    write_be32(f + EXPORTS + index * 16u, index * 4u);
    write_be32(f + EXPORTS + index * 16u + 4, offset);
    write_be32(f + EXPORTS + index * 16u + 8, 1);
}

// The static module: f at 0x40 in its section 1.
static int write_sel(const char* path) {
    u8 f[FILE_SIZE];
    module_header(f, 0, 0, 0, 1, 0);
    write_be32(f + 0x08, 0);
    add_export(f, 0, "f", 0x40);
    return write_file(path, f, FILE_SIZE);
}

// A resident module exporting g, its second word.
static int write_resident(const char* path) {
    u8 f[FILE_SIZE];
    module_header(f, 0, 0, 0, 1, 0);
    write_be32(f + 0x08, 3);
    add_export(f, 0, "g", 4);
    return write_file(path, f, FILE_SIZE);
}

// An overlay: its text calls f and g, its data points at its own text and
// bss, and it exports h, its third word.
static int write_overlay(const char* path) {
    u8 f[FILE_SIZE];
    module_header(f, BSS_SIZE, 2, 2, 1, 2);
    write_be32(f + 0x100, 0x48000001u);  // bl
    write_be32(f + 0x104, 0x48000001u);  // bl
    entry12(f + INTERNALS, 0x110, 1u << 8 | R_PPC_ADDR32, 8);
    entry12(f + INTERNALS + 12, 0x114, 3u << 8 | R_PPC_ADDR32, 0);
    entry12(f + EXTERNALS, 0x100, 0u << 8 | R_PPC_REL24, 0);
    entry12(f + EXTERNALS + 12, 0x104, 1u << 8 | R_PPC_REL24, 0);
    strcpy((char*)f + IMPORT_NAMES, "f");
    strcpy((char*)f + IMPORT_NAMES + 4, "g");
    entry12(f + IMPORTS, 0, 0, 0);
    entry12(f + IMPORTS + 12, 4, 0, 0);
    add_export(f, 0, "h", 8);
    return write_file(path, f, FILE_SIZE);
}

static u32 branch_target(const u8* p, u32 address) {
    return address + (u32)sign_extend(read_be32(p) & 0x03FFFFFCu, 26);
}

static const LinkedSection* data_of(const ModuleLink* link, u32 group) {
    for (u32 i = 0; i < link->data_count; i++) {
        if (link->data[i].group == group)
            return &link->data[i];
    }
    return NULL;
}

static int has_entry(const ModuleLink* link, u32 group, u32 address) {
    for (u32 i = 0; i < link->entry_count; i++) {
        if (link->entries[i].group == group && link->entries[i].address == address)
            return 1;
    }
    return 0;
}

static void configure(RecompConfig* config) {
    memset(config, 0, sizeof(*config));
    strcpy(config->sel, "test_rso.sel");
    config->static_sections[1] = STATIC_TEXT;
    config->static_section_count = 2;
    strcpy(config->rso_modules[0].file, "test_rso_resident.rso");
    config->rso_modules[0].base = RESIDENT_BASE;
    config->rso_module_count = 1;
    strcpy(config->rso_overlays[0].file, "test_rso_overlay.rso");
    config->rso_overlays[0].base = OVERLAY_BASE;
    config->rso_overlays[0].bss = OVERLAY_BSS;
    strcpy(config->rso_overlays[1].file, "test_rso_overlay.rso");  // placed by the heap rule
    config->rso_overlay_count = 2;
    config->rso_overlay_heap_top = HEAP_TOP;
}

static int test_link(void) {
    RecompConfig config;
    configure(&config);
    ModuleLink link = {0};
    CHECK(rso_link(&link, &config, "."), "rso_link failed");
    CHECK(link.count == 3, "%u text sections", link.count);
    const u32 heap_base = HEAP_TOP - FILE_SIZE;
    const u32 heap_bss = heap_base - 0x40u;  // round32(0x24 + 0x10)
    const u32 bases[] = {RESIDENT_BASE, OVERLAY_BASE, heap_base};
    for (u32 g = 0; g < 3; g++) {
        CHECK(link.text[g].group == g && link.text[g].address == bases[g] + 0x100,
              "text %u: group %u at 0x%08X", g, link.text[g].group, link.text[g].address);
    }
    for (u32 g = 1; g < 3; g++) {
        const LinkedSection* text = &link.text[g];
        CHECK(branch_target(text->bytes, text->address) == STATIC_TEXT + 0x40, "overlay %u: call to f", g);
        CHECK(branch_target(text->bytes + 4, text->address + 4) == RESIDENT_BASE + 0x104,
              "overlay %u: call to g", g);
        CHECK((read_be32(text->bytes) & 1u) == 1u, "bl lost its link bit");
        const LinkedSection* data = data_of(&link, g);
        CHECK(data && read_be32(data->bytes) == bases[g] + 0x108, "overlay %u: pointer into its text", g);
        CHECK(read_be32(data->bytes + 4) == (g == 1 ? OVERLAY_BSS : heap_bss), "overlay %u: bss 0x%08X", g,
              read_be32(data->bytes + 4));
        CHECK(has_entry(&link, g, bases[g] + 0x108), "overlay %u: its export", g);
        CHECK(!has_entry(&link, 0, bases[g] + 0x108), "an overlay's export is resident");
    }
    CHECK(has_entry(&link, 0, STATIC_TEXT + 0x40) && has_entry(&link, 0, RESIDENT_BASE + 0x104),
          "resident exports");
    module_link_free(&link);

    // An overlay with neither a base nor the heap rule can't be placed.
    config.rso_overlay_heap_top = 0;
    CHECK(!rso_link(&link, &config, "."), "an overlay with no base linked");
    module_link_free(&link);
    return 1;
}

// A loaded overlay's header points at its own section table and name, and
// the table at its bss.
static int test_locate(void) {
    const u32 size = 0x40000u;
    u8* mem1 = (u8*)calloc(size, 1);
    CHECK(mem1, "out of memory");
    u32 base = 0, bss = 0;
    int ok = rso_locate(".", "test_rso_overlay.rso", mem1, size, NULL, 0, &base, &bss) == 0;
    u8* header = mem1 + (OVERLAY_BASE - 0x80000000u);
    write_be32(header + 0x08, 4);
    write_be32(header + 0x0C, OVERLAY_BASE + TABLE);
    write_be32(header + 0x10, OVERLAY_BASE + NAME);
    write_be32(header + TABLE + 24, OVERLAY_BSS);
    ok = ok && rso_locate(".", "test_rso_overlay.rso", mem1, size, NULL, 0, &base, &bss) == 1 &&
         base == OVERLAY_BASE && bss == OVERLAY_BSS;
    ok = ok && rso_locate(".", "test_rso_none.rso", mem1, size, NULL, 0, &base, &bss) == -1;
    free(mem1);
    CHECK(ok, "rso_locate: base 0x%08X, bss 0x%08X", base, bss);
    return 1;
}

int main(void) {
    const int written = write_sel("test_rso.sel") && write_resident("test_rso_resident.rso") &&
                        write_overlay("test_rso_overlay.rso");
    const int ok = written && test_link() && test_locate();
    remove("test_rso.sel");
    remove("test_rso_resident.rso");
    remove("test_rso_overlay.rso");
    if (!written)
        fprintf(stderr, "can't write the modules\n");
    if (!ok)
        return 1;
    printf("RSO link tests passed\n");
    return 0;
}
