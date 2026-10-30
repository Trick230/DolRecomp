#include "app/game_file.h"
#include "app/module_link.h"

#include "frontend/container/rel.h"

#include <stdio.h>
#include <stdlib.h>

static u32 round32(u32 size) {
    return (size + 31u) & ~31u;
}

// Reads a module and loads its image where the config says the game loads it.
// With no base there, it is a group of its own, compiled for the link's arena
// if resident, or if an overlay for the end of MEM1 (its file, then its bss);
// so is an overlay with one.
static int load_rel(ModuleLink* link, RELFile* rel, u32* group, const RecompModule* placed, bool overlay,
                    const char* game_dir) {
    u8* bytes = NULL;
    u32 size = 0;
    if (!game_file_read(game_dir, placed->file, &bytes, &size)) {
        fprintf(stderr, "error: can't read REL module %s\n", placed->file);
        return 0;
    }
    const u32 bss_size = size >= 0x24u ? read_be32(bytes + 0x20) : 0;
    u32 base = placed->base, bss = placed->bss;
    if (base == 0 && overlay) {
        base = LINK_OVERLAY_TOP - round32(size) - round32(bss_size);
        bss = bss_size ? base + round32(size) : 0;
    } else if (base == 0) {
        base = module_link_place(link, size);
        bss = bss_size ? module_link_place(link, bss_size) : 0;
    } else if (!overlay &&
               (!module_link_reserve(link, base, size) || (bss && !module_link_reserve(link, bss, bss_size)))) {
        free(bytes);
        return 0;
    }
    if (!rel_load_image_bytes(rel, bytes, size, base)) {
        fprintf(stderr, "error: can't read REL module %s\n", placed->file);
        return 0;
    }
    // The game hands OSLink the bss; its sections follow each other there.
    u32 bss_section = ~0u;
    for (u32 i = 0, at = bss; i < rel->section_count; i++) {
        RELSection* section = &rel->sections[i];
        if (!section->bss)
            continue;
        if (bss == 0) {
            fprintf(stderr, "error: %s has a bss section; give its address in the config\n", placed->file);
            return 0;
        }
        if (bss_section == ~0u)
            bss_section = i;
        section->address = at;
        at += section->size;
    }
    *group = 0;
    if (overlay || placed->base == 0) {
        const LinkedGroup linked = {placed->file, rel->module_id, overlay, base, bss, bss_section};
        *group = module_link_add_group(link, &linked);
        if (!*group)
            return 0;
    }
    return 1;
}

// The modules a REL links against, with their groups.
typedef struct {
    ModuleLink* link;
    const RELModuleMapEntry* entries;
    const u32* groups;
    u32 count;
    u32 group;  // the linking module's
} LinkNotes;

static int note(void* user, const RELFile* rel, const RELSection* patched, u32 patch_address, u8 type,
                u32 target_module, u32 target_section, u32 target) {
    const LinkNotes* notes = (const LinkNotes*)user;
    if (!patched->executable || type == 0)
        return 1;
    u32 group = 0;
    const RELFile* owner = NULL;
    for (u32 i = 0; target_module && i < notes->count; i++) {
        if (notes->entries[i].module_id == target_module) {
            group = notes->groups[i];
            owner = notes->entries[i].rel;
        }
    }
    const bool to_bss = owner && target_section < owner->section_count && owner->sections[target_section].bss;
    return module_link_note(notes->link, notes->group, patch_address, type, group, to_bss, target);
}

// Relocates a loaded module against the map's and adds it to the link.
static int link_rel(ModuleLink* link, RELFile* rel, u32 group, const char* file, const RELModuleMapEntry* entries,
                    const u32* groups, u32 count) {
    LinkNotes notes = {link, entries, groups, count, group};
    // Imports of any module not in the map stay as in the file, as OSLink
    // leaves them until that module loads.
    const RELModuleMap map = {entries, count, true, note, &notes};
    if (!rel_apply_relocations(rel, &map)) {
        fprintf(stderr, "error: %s: can't apply its relocations\n", file);
        return 0;
    }
    for (u32 i = 0; i < rel->section_count; i++) {
        const RELSection* section = &rel->sections[i];
        if (section->data &&
            !module_link_add_section(link, group, section->executable, file, section->address, section->data,
                                     section->size))
            return 0;
    }
    const u8 entry_sections[] = {(u8)rel->prolog_section, (u8)rel->epilog_section, (u8)rel->unresolved_section};
    const u32 entry_offsets[] = {rel->prolog_offset, rel->epilog_offset, rel->unresolved_offset};
    for (u32 i = 0; i < 3; i++) {
        if (entry_sections[i] &&
            !module_link_add_entry(link, group, rel->sections[entry_sections[i]].address + entry_offsets[i]))
            return 0;
    }
    printf("linked %s at 0x%08X (module %u, %u relocations)%s\n", file, rel->base_address, rel->module_id,
           rel->relocation_count,
           !group                          ? ""
           : link->groups[group - 1].overlay ? " as an overlay"
                                             : " as a module of its own");
    return 1;
}

static int unique_id(const RELModuleMapEntry* entries, const RecompModule* const* placed, u32 count) {
    for (u32 i = 0; i < count; i++) {
        if (entries[i].module_id == entries[count].module_id) {
            fprintf(stderr, "error: %s and %s are both module %u\n", placed[i]->file, placed[count]->file,
                    entries[count].module_id);
            return 0;
        }
    }
    return 1;
}

int rel_link(ModuleLink* link, const RecompConfig* config, const char* game_dir) {
    const u32 module_count = config->rel_module_count;
    if (module_count == 0 && config->rel_overlay_count == 0)
        return 1;
    // The resident modules, then a slot for the overlay being linked.
    const u32 slots = module_count + 1u;
    RELFile* rels = (RELFile*)calloc(slots, sizeof(RELFile));
    RELModuleMapEntry* entries = (RELModuleMapEntry*)calloc(slots, sizeof(RELModuleMapEntry));
    u32* groups = (u32*)calloc(slots, sizeof(u32));
    const RecompModule** placed = (const RecompModule**)calloc(slots, sizeof(RecompModule*));
    u32 loaded = 0;
    int ok = 0;
    if (!rels || !entries || !groups || !placed)
        goto done;

    // The modules the config places first, so the others' arena leaves them be.
    for (u32 pass = 0; pass < 2; pass++) {
        for (u32 m = 0; m < module_count; m++) {
            if ((config->rel_modules[m].base == 0) != (pass == 1))
                continue;
            placed[loaded] = &config->rel_modules[m];
            if (!load_rel(link, &rels[loaded], &groups[loaded], placed[loaded], false, game_dir)) {
                rel_free(&rels[loaded]);
                goto done;
            }
            entries[loaded] = (RELModuleMapEntry){rels[loaded].module_id, &rels[loaded]};
            if (!unique_id(entries, placed, loaded++))
                goto done;
        }
    }
    // Every resident module is loaded at once.
    for (u32 m = 0; m < module_count; m++) {
        if (!link_rel(link, &rels[m], groups[m], placed[m]->file, entries, groups, module_count))
            goto done;
    }
    // Each overlay links against the resident modules and itself.
    for (u32 k = 0; k < config->rel_overlay_count; k++) {
        placed[module_count] = &config->rel_overlays[k];
        RELFile* overlay = &rels[module_count];
        int linked = load_rel(link, overlay, &groups[module_count], placed[module_count], true, game_dir);
        if (linked) {
            entries[module_count] = (RELModuleMapEntry){overlay->module_id, overlay};
            linked = unique_id(entries, placed, module_count) &&
                     link_rel(link, overlay, groups[module_count], placed[module_count]->file, entries, groups,
                              slots);
        }
        rel_free(overlay);
        if (!linked)
            goto done;
    }
    ok = 1;

done:
    for (u32 i = 0; rels && i < loaded; i++)
        rel_free(&rels[i]);
    free(rels);
    free(entries);
    free(groups);
    free(placed);
    return ok;
}
