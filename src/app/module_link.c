#include "app/module_link.h"

#include <stdlib.h>
#include <string.h>

int module_link_add_section(ModuleLink* link, u32 group, bool code, const char* module,
                            u32 address, const u8* bytes, u32 size) {
    LinkedSection** sections = code ? &link->text : &link->data;
    u32* count = code ? &link->count : &link->data_count;
    for (u32 i = 0; i < *count; i++) {
        if ((*sections)[i].group == group && (*sections)[i].address == address)
            return 1;
    }
    LinkedSection* resized = (LinkedSection*)realloc(*sections, (*count + 1u) * sizeof(LinkedSection));
    if (!resized)
        return 0;
    *sections = resized;
    LinkedSection* section = &resized[*count];
    section->bytes = (u8*)malloc(size ? size : 1u);
    if (!section->bytes)
        return 0;
    memcpy(section->bytes, bytes, size);
    section->module = module;
    section->group = group;
    section->address = address;
    section->size = size;
    ++*count;
    return 1;
}

int module_link_add_entry(ModuleLink* link, u32 group, u32 address) {
    // Grows by powers of two.
    if ((link->entry_count & (link->entry_count - 1u)) == 0) {
        const u32 capacity = link->entry_count ? link->entry_count * 2u : 1u;
        LinkedEntry* resized = (LinkedEntry*)realloc(link->entries, capacity * sizeof(LinkedEntry));
        if (!resized)
            return 0;
        link->entries = resized;
    }
    link->entries[link->entry_count++] = (LinkedEntry){group, address};
    return 1;
}

int module_link_add_reloc(ModuleLink* link, const LinkedReloc* reloc) {
    if ((link->reloc_count & (link->reloc_count - 1u)) == 0) {
        const u32 capacity = link->reloc_count ? link->reloc_count * 2u : 1u;
        LinkedReloc* resized = (LinkedReloc*)realloc(link->relocs, capacity * sizeof(LinkedReloc));
        if (!resized)
            return 0;
        link->relocs = resized;
    }
    link->relocs[link->reloc_count++] = *reloc;
    return 1;
}

void module_link_free(ModuleLink* link) {
    for (u32 i = 0; i < link->count; i++)
        free(link->text[i].bytes);
    for (u32 i = 0; i < link->data_count; i++)
        free(link->data[i].bytes);
    free(link->text);
    free(link->data);
    free(link->entries);
    free(link->relocs);
    memset(link, 0, sizeof(*link));
}
