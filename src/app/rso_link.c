#include "app/game_file.h"
#include "app/module_link.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SECTION_ABSOLUTE 0xFFF1u  // SHN_ABS

typedef struct {
    u8* data;
    u32 size;
} Bytes;

// The RSO header fields the linker needs (layout as in Dolphin's RSOView).
typedef struct {
    u32 section_count, section_table;
    u32 name, bss_size;
    u8 prolog_section, epilog_section, unresolved_section;
    u32 unresolved_offset;
    u32 internals, internals_size, externals, externals_size;
    u32 exports, exports_size, export_names;
    u32 imports, imports_size, import_names;
} RsoHeader;

typedef struct {
    Bytes file;
    RsoHeader header;
    u32 base, bss;  // where it loads
    u32* section_address;
    u32 group;
} Module;

typedef struct {
    const char* name;
    u32 address;
    u32 order;
    u32 group;
    bool bss;
} Export;

static int fits(const Bytes* b, u64 offset, u64 size) {
    return offset <= b->size && size <= b->size - offset;
}

static const char* cstring(const Bytes* b, u32 offset) {
    // A name running off the end reads as empty rather than past the buffer.
    if (offset >= b->size || !memchr(b->data + offset, 0, b->size - offset))
        return "";
    return (const char*)b->data + offset;
}

static int read_file(const char* dir, const char* name, Bytes* out) {
    return game_file_read(dir, name, &out->data, &out->size);
}

static int parse_header(const Bytes* f, RsoHeader* h) {
    if (!fits(f, 0, 0x58))
        return 0;
    const u8* d = f->data;
    h->section_count = read_be32(d + 0x08);
    h->section_table = read_be32(d + 0x0C);
    h->name = read_be32(d + 0x10);
    h->bss_size = read_be32(d + 0x1C);
    h->prolog_section = d[0x20];
    h->epilog_section = d[0x21];
    h->unresolved_section = d[0x22];
    h->unresolved_offset = read_be32(d + 0x2C);
    h->internals = read_be32(d + 0x30);
    h->internals_size = read_be32(d + 0x34);
    h->externals = read_be32(d + 0x38);
    h->externals_size = read_be32(d + 0x3C);
    h->exports = read_be32(d + 0x40);
    h->exports_size = read_be32(d + 0x44);
    h->export_names = read_be32(d + 0x48);
    h->imports = read_be32(d + 0x4C);
    h->imports_size = read_be32(d + 0x50);
    h->import_names = read_be32(d + 0x54);
    return fits(f, h->section_table, (u64)h->section_count * 8u) &&
           fits(f, h->internals, h->internals_size) && fits(f, h->externals, h->externals_size) &&
           fits(f, h->exports, h->exports_size) && fits(f, h->imports, h->imports_size);
}

// Patches one relocation the way the SDK's RSO linker does. S is the target
// address, P the address of the patched field.
static int relocate(u8* image, u32 image_size, u32 offset, u8 type, u32 S, u32 P) {
    if (offset > image_size || image_size - offset < 4)
        return 0;
    u8* field = image + offset;
    u32 word = read_be32(field);
    switch (type) {
    case R_PPC_ADDR32:
        word = S;
        break;
    case R_PPC_ADDR16_LO:
        field[0] = (u8)(S >> 8);
        field[1] = (u8)S;
        return 1;
    case R_PPC_ADDR16_HI:
        field[0] = (u8)(S >> 24);
        field[1] = (u8)(S >> 16);
        return 1;
    case R_PPC_ADDR16_HA:
        S += 0x8000u;
        field[0] = (u8)(S >> 24);
        field[1] = (u8)(S >> 16);
        return 1;
    case R_PPC_REL24:
        word = (word & 0xFC000003u) | ((S - P) & 0x03FFFFFCu);
        break;
    case R_PPC_REL14:
        word = (word & 0xFFFF0003u) | ((S - P) & 0x0000FFFCu);
        break;
    default:
        return 0;
    }
    field[0] = (u8)(word >> 24);
    field[1] = (u8)(word >> 16);
    field[2] = (u8)(word >> 8);
    field[3] = (u8)word;
    return 1;
}

static int compare_name(const void* a, const void* b) {
    return strcmp(((const Export*)a)->name, ((const Export*)b)->name);
}

// By name, the latest definition first.
static int compare_export(const void* a, const void* b) {
    const int names = compare_name(a, b);
    if (names)
        return names;
    return ((const Export*)a)->order < ((const Export*)b)->order ? 1 : -1;
}

static int add_export(Export** exports, u32* count, u32* capacity, const char* name, u32 address, u32 group,
                      bool bss) {
    if (*count == *capacity) {
        const u32 grown = *capacity ? *capacity * 2u : 1024u;
        Export* resized = (Export*)realloc(*exports, grown * sizeof(Export));
        if (!resized)
            return 0;
        *exports = resized;
        *capacity = grown;
    }
    (*exports)[*count] = (Export){name, address, *count, group, bss};
    ++*count;
    return 1;
}

static void free_module(Module* module) {
    free(module->file.data);
    free(module->section_address);
    memset(module, 0, sizeof(*module));
}

static u32 round32(u32 size) {
    return (size + 31u) & ~31u;
}

static int load_module(Module* module, const char* file, const char* game_dir) {
    if (read_file(game_dir, file, &module->file) && parse_header(&module->file, &module->header))
        return 1;
    fprintf(stderr, "error: can't read RSO module %s\n", file);
    return 0;
}

// Where an overlay the config gives no base loads (see app/config.h): the
// file, then the bss, from the tail of the expanded heap's free space ending
// at top.
static void place_on_heap(Module* module, u32 top) {
    module->base = top - round32(module->file.size);
    module->bss = module->header.bss_size ? module->base - round32(module->header.bss_size + 0x10u) : 0;
}

// Reads a module and places its sections where the config says it loads. With
// no base there, it is a group of its own, compiled for the heap rule if an
// overlay, or the link's arena if resident; so is an overlay with one.
static int place_module(ModuleLink* link, Module* module, const RecompModule* placed, bool overlay, u32 heap_top,
                        const char* game_dir) {
    if (!load_module(module, placed->file, game_dir))
        return 0;
    const RsoHeader* h = &module->header;
    module->base = placed->base;
    module->bss = placed->bss;
    if (placed->base == 0 && overlay) {
        place_on_heap(module, heap_top ? heap_top : LINK_OVERLAY_TOP);
    } else if (placed->base == 0) {
        module->base = module_link_place(link, module->file.size);
        module->bss = h->bss_size ? module_link_place(link, h->bss_size) : 0;
    } else if (!overlay && (!module_link_reserve(link, module->base, module->file.size) ||
                            (module->bss && !module_link_reserve(link, module->bss, h->bss_size)))) {
        return 0;
    }
    module->section_address = (u32*)calloc(h->section_count ? h->section_count : 1u, sizeof(u32));
    if (!module->section_address)
        return 0;
    u32 bss_section = ~0u;
    for (u32 i = 0; i < h->section_count; i++) {
        const u8* table = module->file.data + h->section_table + i * 8u;
        const u32 offset = read_be32(table) & ~1u;
        const u32 size = read_be32(table + 4);
        if (offset != 0) {
            module->section_address[i] = module->base + offset;
        } else if (size != 0) {
            if (module->bss == 0) {
                fprintf(stderr, "error: %s has a bss section; give its address in the config\n",
                        placed->file);
                return 0;
            }
            module->section_address[i] = module->bss;
            bss_section = i;
        }
    }
    module->group = 0;
    if (overlay || placed->base == 0) {
        const LinkedGroup group = {placed->file, 0, overlay, module->base, module->bss, bss_section};
        module->group = module_link_add_group(link, &group);
        if (!module->group)
            return 0;
    }
    return 1;
}

typedef int (*ExportVisitor)(void* user, const Module* module, const char* name, u32 address, bool bss);

// Calls visit for each of a placed module's exports.
static int visit_exports(const Module* module, ExportVisitor visit, void* user) {
    const RsoHeader* h = &module->header;
    for (u32 i = 0; i + 16u <= h->exports_size; i += 16u) {
        const u8* entry = module->file.data + h->exports + i;
        const u32 section = read_be32(entry + 8);
        if (section >= h->section_count)
            continue;
        const u32 start = module->section_address[section];
        if (!visit(user, module, cstring(&module->file, h->export_names + read_be32(entry)),
                   start + read_be32(entry + 4), module->bss && start == module->bss))
            return 0;
    }
    return 1;
}

typedef struct {
    Export* exports;
    u32 count;
    u32 capacity;
} ExportList;

static int add_to_exports(void* user, const Module* module, const char* name, u32 address, bool bss) {
    ExportList* list = (ExportList*)user;
    return add_export(&list->exports, &list->count, &list->capacity, name, address, module->group, bss);
}

static int add_to_entries(void* user, const Module* module, const char* name, u32 address, bool bss) {
    (void)name;
    (void)bss;
    return module_link_add_entry((ModuleLink*)user, module->group, address);
}

static bool code_section(const RsoHeader* h, u32 index) {
    return index == h->prolog_section || index == h->epilog_section || index == h->unresolved_section;
}

// Whether a module file offset is in one of its code sections.
static bool in_code(const Module* module, u32 offset) {
    const RsoHeader* h = &module->header;
    for (u32 i = 0; i < h->section_count; i++) {
        const u8* table = module->file.data + h->section_table + i * 8u;
        const u32 start = read_be32(table) & ~1u;
        if (start && code_section(h, i) && offset - start < read_be32(table + 4))
            return true;
    }
    return false;
}

// Relocates a placed module against the exports (sorted by name) and adds its
// sections to the link.
static int link_module(ModuleLink* link, const RecompModule* placed, const Module* module,
                       const ExportList* exports) {
    const Bytes* file = &module->file;
    const RsoHeader* h = &module->header;
    u8* image = (u8*)malloc(file->size);
    if (!image)
        return 0;
    memcpy(image, file->data, file->size);

    int relocated = 1;
    for (u32 i = 0; relocated && i + 12u <= h->internals_size; i += 12u) {
        const u8* entry = file->data + h->internals + i;
        const u32 offset = read_be32(entry), info = read_be32(entry + 4);
        const u32 section = info >> 8;
        if (section >= h->section_count) {
            relocated = 0;
            break;
        }
        const u32 target = module->section_address[section] + read_be32(entry + 8);
        relocated = relocate(image, file->size, offset, (u8)info, target, module->base + offset) &&
                    (!in_code(module, offset) ||
                     module_link_note(link, module->group, module->base + offset, (u8)info, module->group,
                                      module->bss && module->section_address[section] == module->bss, target));
    }
    // Imports nothing exports stay bound to the module's unresolved stub,
    // which is also what the game's linker leaves them pointing at.
    const u32 unresolved = h->unresolved_section < h->section_count
                               ? module->section_address[h->unresolved_section] + h->unresolved_offset
                               : 0;
    u32 missing = 0;
    for (u32 i = 0; relocated && i + 12u <= h->externals_size; i += 12u) {
        const u8* entry = file->data + h->externals + i;
        const u32 offset = read_be32(entry), info = read_be32(entry + 4);
        if ((u64)(info >> 8) * 12u + 12u > h->imports_size) {
            relocated = 0;
            break;
        }
        const u8* import = file->data + h->imports + (info >> 8) * 12u;
        const Export key = {cstring(file, h->import_names + read_be32(import)), 0, 0, 0, false};
        const Export* found =
            exports->count ? (const Export*)bsearch(&key, exports->exports, exports->count,
                                                    sizeof(Export), compare_name)
                           : NULL;
        missing += found == NULL;
        const u32 target = (found ? found->address : unresolved) + read_be32(entry + 8);
        relocated = relocate(image, file->size, offset, (u8)info, target, module->base + offset) &&
                    (!in_code(module, offset) ||
                     module_link_note(link, module->group, module->base + offset, (u8)info,
                                      found ? found->group : module->group, found && found->bss, target));
    }
    if (!relocated) {
        fprintf(stderr, "error: %s: malformed or unsupported relocation\n", placed->file);
        free(image);
        return 0;
    }

    // Executable sections are the ones holding the prolog/epilog/unresolved
    // entry points; RSO section tables carry no executable flag of their own.
    int ok = 1;
    for (u32 index = 0; ok && index < h->section_count; index++) {
        const u8* table = file->data + h->section_table + index * 8u;
        const u32 offset = read_be32(table) & ~1u, size = read_be32(table + 4);
        if (offset == 0 || size == 0 || !fits(file, offset, size))
            continue;
        ok = module_link_add_section(link, module->group, code_section(h, index), placed->file,
                                     module->base + offset, image + offset, size);
    }
    if (ok)
        printf("linked %s at 0x%08X (%u unresolved imports)%s\n", placed->file, module->base, missing,
               !module->group                          ? ""
               : link->groups[module->group - 1].overlay ? " as an overlay"
                                                         : " as a module of its own");
    free(image);
    return ok;
}

int rso_link(ModuleLink* link, const RecompConfig* config, const char* game_dir, const u32* static_sections,
             u32 static_section_count) {
    const u32 module_count = config->rso_module_count;
    Bytes sel = {0};
    RsoHeader sel_header;
    Module* modules = (Module*)calloc(module_count ? module_count : 1u, sizeof(Module));
    Module overlay = {0};
    ExportList exports = {0};
    int ok = 0;

    if (!modules)
        goto done;
    if (!read_file(game_dir, config->sel, &sel) || !parse_header(&sel, &sel_header)) {
        fprintf(stderr, "error: can't read the static module %s\n", config->sel);
        goto done;
    }

    // Exports: the static module's are section-relative against the static
    // section addresses; each RSO's against its own placed sections.
    for (u32 i = 0; i + 16u <= sel_header.exports_size; i += 16u) {
        const u8* entry = sel.data + sel_header.exports + i;
        const u32 offset = read_be32(entry + 4), section = read_be32(entry + 8);
        const char* name = cstring(&sel, sel_header.export_names + read_be32(entry));
        if (section == SECTION_ABSOLUTE) {
            if (!add_export(&exports.exports, &exports.count, &exports.capacity, name, offset, 0, false))
                goto done;
        } else if (section < static_section_count && static_sections[section]) {
            if (!add_export(&exports.exports, &exports.count, &exports.capacity, name,
                            static_sections[section] + offset, 0, false))
                goto done;
        }
    }
    // The modules the config places first, so the others' arena leaves them be.
    for (u32 pass = 0; pass < 2; pass++) {
        for (u32 m = 0; m < module_count; m++) {
            if ((config->rso_modules[m].base == 0) != (pass == 1))
                continue;
            if (!place_module(link, &modules[m], &config->rso_modules[m], false, 0, game_dir) ||
                !visit_exports(&modules[m], add_to_exports, &exports))
                goto done;
        }
    }
    // A name defined twice keeps its latest definition.
    if (exports.count)
        qsort(exports.exports, exports.count, sizeof(Export), compare_export);
    u32 unique = 0;
    for (u32 i = 0; i < exports.count; i++) {
        if (!unique || strcmp(exports.exports[unique - 1u].name, exports.exports[i].name) != 0)
            exports.exports[unique++] = exports.exports[i];
    }
    exports.count = unique;
    for (u32 i = 0; i < exports.count; i++) {
        if (!module_link_add_entry(link, exports.exports[i].group, exports.exports[i].address))
            goto done;
    }
    for (u32 m = 0; m < module_count; m++) {
        if (!link_module(link, &config->rso_modules[m], &modules[m], &exports))
            goto done;
    }

    // Each overlay links against the resident modules alone, the way it finds
    // them when it loads; its own exports are where other code enters it.
    for (u32 k = 0; k < config->rso_overlay_count; k++) {
        const RecompModule* placed = &config->rso_overlays[k];
        const int linked = place_module(link, &overlay, placed, true, config->rso_overlay_heap_top, game_dir) &&
                           visit_exports(&overlay, add_to_entries, link) &&
                           link_module(link, placed, &overlay, &exports);
        free_module(&overlay);
        if (!linked)
            goto done;
    }
    ok = 1;

done:
    free(exports.exports);
    for (u32 m = 0; modules && m < module_count; m++)
        free_module(&modules[m]);
    free(modules);
    free_module(&overlay);
    free(sel.data);
    return ok;
}

// A dumped region of guest RAM.
typedef struct {
    const u8* bytes;
    u32 size;
    u32 address;
} RamRegion;

static const u8* ram_at(const RamRegion* ram, u32 count, u32 address, u32 size) {
    for (u32 i = 0; i < count; i++) {
        if (address - ram[i].address < ram[i].size && size <= ram[i].size - (address - ram[i].address))
            return ram[i].bytes + (address - ram[i].address);
    }
    return NULL;
}

// A linked module's header in RAM holds its own section table and name as
// addresses, so the header is where both point back from. Its bss is the one
// address-only entry of that table.
static int find_loaded(const Module* module, const RamRegion* ram, u32 count, u32* base, u32* bss) {
    const RsoHeader* h = &module->header;
    for (u32 r = 0; r < count; r++) {
        for (u32 offset = 0; offset + 0x58u <= ram[r].size; offset += 4u) {
            const u8* header = ram[r].bytes + offset;
            const u32 address = ram[r].address + offset;
            if (read_be32(header + 0x08) != h->section_count ||
                read_be32(header + 0x0C) != address + h->section_table ||
                (h->name && read_be32(header + 0x10) != address + h->name))
                continue;
            const u8* table = ram_at(ram, count, address + h->section_table, h->section_count * 8u);
            if (!table)
                continue;
            *base = address;
            *bss = 0;
            for (u32 i = 0; i < h->section_count; i++) {
                const u8* entry = module->file.data + h->section_table + i * 8u;
                if (read_be32(entry) == 0 && read_be32(entry + 4) != 0)
                    *bss = read_be32(table + i * 8u);
            }
            return 1;
        }
    }
    return 0;
}

static void report(const RecompModule* placed, const Module* module, const RamRegion* ram, u32 count,
                   u32 heap_top, int overlay) {
    u32 base, bss;
    if (!find_loaded(module, ram, count, &base, &bss)) {
        printf("# %s: not loaded\n", placed->file);
        return;
    }
    printf("{ file = \"%s\", base = 0x%08X", placed->file, base);
    if (bss)
        printf(", bss = 0x%08X", bss);
    printf(" },");
    Module expected = *module;
    expected.base = placed->base;
    expected.bss = placed->bss;
    if (placed->base == 0 && heap_top)
        place_on_heap(&expected, heap_top);
    if (expected.base == base && expected.bss == bss)
        printf("  # as configured");
    else
        printf("  # config: 0x%08X, bss 0x%08X", expected.base, expected.bss);
    if (overlay)
        printf(", heap top 0x%08X", base + round32(module->file.size));
    printf("\n");
}

int rso_find(const RecompConfig* config, const char* game_dir, const u8* mem1, u32 mem1_size,
             const u8* mem2, u32 mem2_size) {
    const RamRegion ram[] = {{mem1, mem1_size, 0x80000000u}, {mem2, mem2_size, 0x90000000u}};
    const u32 count = mem2 ? 2u : 1u;
    const u32 total = config->rso_module_count + config->rso_overlay_count;
    for (u32 i = 0; i < total; i++) {
        const int overlay = i >= config->rso_module_count;
        const RecompModule* placed =
            overlay ? &config->rso_overlays[i - config->rso_module_count] : &config->rso_modules[i];
        Module module = {0};
        if (!load_module(&module, placed->file, game_dir)) {
            free_module(&module);
            return 0;
        }
        report(placed, &module, ram, count, overlay ? config->rso_overlay_heap_top : 0, overlay);
        free_module(&module);
    }
    return 1;
}

int rso_locate(const char* game_dir, const char* file, const u8* mem1, u32 mem1_size, const u8* mem2,
               u32 mem2_size, u32* base, u32* bss) {
    const RamRegion ram[] = {{mem1, mem1_size, 0x80000000u}, {mem2, mem2_size, 0x90000000u}};
    Module module = {0};
    const int found = load_module(&module, file, game_dir)
                          ? find_loaded(&module, ram, mem2 ? 2u : 1u, base, bss)
                          : -1;
    free_module(&module);
    return found;
}
