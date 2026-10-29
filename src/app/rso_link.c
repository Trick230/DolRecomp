#include "app/module_link.h"

#include "platform/fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SECTION_ABSOLUTE 0xFFF1u  // SHN_ABS

enum {
    R_PPC_ADDR32 = 1,
    R_PPC_ADDR16_LO = 4,
    R_PPC_ADDR16_HI = 5,
    R_PPC_ADDR16_HA = 6,
    R_PPC_REL24 = 10,
    R_PPC_REL14 = 11,
};

typedef struct {
    u8* data;
    u32 size;
} Bytes;

// The RSO header fields the linker needs (layout as in Dolphin's RSOView).
typedef struct {
    u32 section_count, section_table;
    u8 prolog_section, epilog_section, unresolved_section;
    u32 unresolved_offset;
    u32 internals, internals_size, externals, externals_size;
    u32 exports, exports_size, export_names;
    u32 imports, imports_size, import_names;
} RsoHeader;

typedef struct {
    Bytes file;
    RsoHeader header;
    u32* section_address;
} Module;

typedef struct {
    const char* name;
    u32 address;
    u32 order;
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
    char path[1200];
    if (!join_path(path, sizeof(path), dir, name))
        return 0;
    FILE* file = fopen(path, "rb");
    if (!file)
        return 0;
    fseek(file, 0, SEEK_END);
    const long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    out->data = size > 0 ? (u8*)malloc((size_t)size) : NULL;
    out->size = (u32)size;
    const int ok = out->data && fread(out->data, 1, (size_t)size, file) == (size_t)size;
    fclose(file);
    return ok;
}

static int parse_header(const Bytes* f, RsoHeader* h) {
    if (!fits(f, 0, 0x58))
        return 0;
    const u8* d = f->data;
    h->section_count = read_be32(d + 0x08);
    h->section_table = read_be32(d + 0x0C);
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

static int add_export(Export** exports, u32* count, u32* capacity, const char* name, u32 address) {
    if (*count == *capacity) {
        const u32 grown = *capacity ? *capacity * 2u : 1024u;
        Export* resized = (Export*)realloc(*exports, grown * sizeof(Export));
        if (!resized)
            return 0;
        *exports = resized;
        *capacity = grown;
    }
    (*exports)[*count] = (Export){name, address, *count};
    ++*count;
    return 1;
}

static void free_module(Module* module) {
    free(module->file.data);
    free(module->section_address);
    memset(module, 0, sizeof(*module));
}

// Reads a module and places its sections where the config says it loads.
static int place_module(Module* module, const RecompModule* placed, const char* game_dir) {
    if (!read_file(game_dir, placed->file, &module->file) ||
        !parse_header(&module->file, &module->header)) {
        fprintf(stderr, "error: can't read RSO module %s\n", placed->file);
        return 0;
    }
    const RsoHeader* h = &module->header;
    module->section_address = (u32*)calloc(h->section_count ? h->section_count : 1u, sizeof(u32));
    if (!module->section_address)
        return 0;
    for (u32 i = 0; i < h->section_count; i++) {
        const u8* table = module->file.data + h->section_table + i * 8u;
        const u32 offset = read_be32(table) & ~1u;
        const u32 size = read_be32(table + 4);
        if (offset != 0) {
            module->section_address[i] = placed->base + offset;
        } else if (size != 0) {
            if (placed->bss == 0) {
                fprintf(stderr, "error: %s has a bss section; give its address in the config\n",
                        placed->file);
                return 0;
            }
            module->section_address[i] = placed->bss;
        }
    }
    return 1;
}

typedef int (*ExportVisitor)(void* user, const char* name, u32 address);

// Calls visit for each of a placed module's exports.
static int visit_exports(const Module* module, ExportVisitor visit, void* user) {
    const RsoHeader* h = &module->header;
    for (u32 i = 0; i + 16u <= h->exports_size; i += 16u) {
        const u8* entry = module->file.data + h->exports + i;
        const u32 section = read_be32(entry + 8);
        if (section < h->section_count &&
            !visit(user, cstring(&module->file, h->export_names + read_be32(entry)),
                   module->section_address[section] + read_be32(entry + 4)))
            return 0;
    }
    return 1;
}

typedef struct {
    Export* exports;
    u32 count;
    u32 capacity;
} ExportList;

static int add_to_exports(void* user, const char* name, u32 address) {
    ExportList* list = (ExportList*)user;
    return add_export(&list->exports, &list->count, &list->capacity, name, address);
}

typedef struct {
    ModuleLink* link;
    u32 group;
} EntryList;

static int add_to_entries(void* user, const char* name, u32 address) {
    (void)name;
    EntryList* list = (EntryList*)user;
    return module_link_add_entry(list->link, list->group, address);
}

// Relocates a placed module against the exports (sorted by name) and adds its
// sections to the link.
static int link_module(ModuleLink* link, u32 group, const RecompModule* placed, const Module* module,
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
        relocated = section < h->section_count &&
                    relocate(image, file->size, offset, (u8)info,
                             module->section_address[section] + read_be32(entry + 8),
                             placed->base + offset);
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
        const Export key = {cstring(file, h->import_names + read_be32(import)), 0, 0};
        const Export* found =
            exports->count ? (const Export*)bsearch(&key, exports->exports, exports->count,
                                                    sizeof(Export), compare_name)
                           : NULL;
        missing += found == NULL;
        relocated = relocate(image, file->size, offset, (u8)info,
                             (found ? found->address : unresolved) + read_be32(entry + 8),
                             placed->base + offset);
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
        const bool code = index == h->prolog_section || index == h->epilog_section ||
                          index == h->unresolved_section;
        ok = module_link_add_section(link, group, code, placed->file, placed->base + offset,
                                     image + offset, size);
    }
    if (ok)
        printf("linked %s at 0x%08X (%u unresolved imports)%s\n", placed->file, placed->base, missing,
               group ? " as an overlay" : "");
    free(image);
    return ok;
}

int rso_link(ModuleLink* link, const RecompConfig* config, const char* game_dir) {
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

    // Exports: the static module's are section-relative against the
    // configured static section addresses; each RSO's against its own placed
    // sections.
    for (u32 i = 0; i + 16u <= sel_header.exports_size; i += 16u) {
        const u8* entry = sel.data + sel_header.exports + i;
        const u32 offset = read_be32(entry + 4), section = read_be32(entry + 8);
        const char* name = cstring(&sel, sel_header.export_names + read_be32(entry));
        if (section == SECTION_ABSOLUTE) {
            if (!add_to_exports(&exports, name, offset))
                goto done;
        } else if (section < config->static_section_count && config->static_sections[section]) {
            if (!add_to_exports(&exports, name, config->static_sections[section] + offset))
                goto done;
        }
    }
    for (u32 m = 0; m < module_count; m++) {
        if (!place_module(&modules[m], &config->rso_modules[m], game_dir) ||
            !visit_exports(&modules[m], add_to_exports, &exports))
            goto done;
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
        if (!module_link_add_entry(link, 0, exports.exports[i].address))
            goto done;
    }
    for (u32 m = 0; m < module_count; m++) {
        if (!link_module(link, 0, &config->rso_modules[m], &modules[m], &exports))
            goto done;
    }

    // Each overlay links against the resident modules alone, the way it finds
    // them when it loads; its own exports are where other code enters it.
    for (u32 k = 0; k < config->rso_overlay_count; k++) {
        EntryList entries = {link, k + 1u};
        const int linked = place_module(&overlay, &config->rso_overlays[k], game_dir) &&
                           visit_exports(&overlay, add_to_entries, &entries) &&
                           link_module(link, k + 1u, &config->rso_overlays[k], &overlay, &exports);
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
