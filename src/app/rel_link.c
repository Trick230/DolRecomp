#include "app/module_link.h"

#include "frontend/container/rel.h"
#include "platform/fs.h"

#include <stdio.h>
#include <stdlib.h>

int rel_link(ModuleLink* link, const RecompConfig* config, const char* game_dir) {
    const u32 module_count = config->rel_module_count;
    if (module_count == 0)
        return 1;
    RELFile* rels = (RELFile*)calloc(module_count, sizeof(RELFile));
    RELModuleMapEntry* entries = (RELModuleMapEntry*)calloc(module_count, sizeof(RELModuleMapEntry));
    u32 loaded = 0;
    int ok = 0;
    if (!rels || !entries)
        goto done;

    while (loaded < module_count) {
        const RecompModule* placed = &config->rel_modules[loaded];
        RELFile* rel = &rels[loaded];
        char path[1200];
        if (!join_path(path, sizeof(path), game_dir, placed->file) ||
            !rel_load_image(rel, path, placed->base)) {
            fprintf(stderr, "error: can't read REL module %s\n", placed->file);
            goto done;
        }
        const u32 index = loaded++;
        // The game hands OSLink the bss; its sections follow each other there.
        u32 bss = placed->bss;
        for (u32 i = 0; i < rel->section_count; i++) {
            RELSection* section = &rel->sections[i];
            if (!section->bss)
                continue;
            if (placed->bss == 0) {
                fprintf(stderr, "error: %s has a bss section; give its address in the config\n",
                        placed->file);
                goto done;
            }
            section->address = bss;
            bss += section->size;
        }
        for (u32 i = 0; i < index; i++) {
            if (entries[i].module_id == rel->module_id) {
                fprintf(stderr, "error: %s and %s are both module %u\n", config->rel_modules[i].file,
                        placed->file, rel->module_id);
                goto done;
            }
        }
        entries[index] = (RELModuleMapEntry){rel->module_id, rel};
    }

    // Every configured module is loaded at once; imports of any other module
    // stay as in the file, as OSLink leaves them until that module loads.
    const RELModuleMap map = {entries, module_count, true};
    for (u32 m = 0; m < module_count; m++) {
        const RecompModule* placed = &config->rel_modules[m];
        RELFile* rel = &rels[m];
        if (!rel_apply_relocations(rel, &map)) {
            fprintf(stderr, "error: %s: can't apply its relocations\n", placed->file);
            goto done;
        }
        for (u32 i = 0; i < rel->section_count; i++) {
            const RELSection* section = &rel->sections[i];
            if (section->data &&
                !module_link_add_section(link, section->executable, placed->file, section->address,
                                         section->data, section->size))
                goto done;
        }
        const u8 entry_sections[] = {(u8)rel->prolog_section, (u8)rel->epilog_section,
                                     (u8)rel->unresolved_section};
        const u32 entry_offsets[] = {rel->prolog_offset, rel->epilog_offset, rel->unresolved_offset};
        for (u32 i = 0; i < 3; i++) {
            if (entry_sections[i] &&
                !module_link_add_entry(link, rel->sections[entry_sections[i]].address + entry_offsets[i]))
                goto done;
        }
        printf("linked %s at 0x%08X (module %u, %u relocations)\n", placed->file, placed->base,
               rel->module_id, rel->relocation_count);
    }
    ok = 1;

done:
    for (u32 i = 0; rels && i < loaded; i++)
        rel_free(&rels[i]);
    free(rels);
    free(entries);
    return ok;
}
