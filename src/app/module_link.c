#include "app/module_link.h"

#include <stdio.h>
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

u32 module_link_add_group(ModuleLink* link, const LinkedGroup* group) {
    if (link->group_count == 0)
        link->group_count = 1;
    if (link->group_count == LINK_MAX_GROUPS) {
        fprintf(stderr, "error: more than %u modules\n", LINK_MAX_GROUPS - 1u);
        return 0;
    }
    link->groups[link->group_count - 1u] = *group;
    return link->group_count++;
}

int module_link_reserve(ModuleLink* link, u32 address, u32 size) {
    if (size == 0)
        return 1;
    if (link->reserved_count == sizeof(link->reserved) / sizeof(link->reserved[0]))
        return 0;
    link->reserved[link->reserved_count++] = (LinkedRange){address, address + size};
    return 1;
}

u32 module_link_place(ModuleLink* link, u32 size) {
    u32 address = (link->arena + 31u) & ~31u;
    for (u32 i = 0; i < link->reserved_count; i++) {
        const LinkedRange* range = &link->reserved[i];
        if (address < range->end && range->start < address + size) {
            address = (range->end + 31u) & ~31u;
            i = ~0u;  // look again from the first
        }
    }
    link->arena = address + size;
    return address;
}

int module_link_note(ModuleLink* link, u32 group, u32 address, u8 type, u32 target_group, bool to_bss,
                     u32 target) {
    LinkedReloc reloc = {group, address & ~3u, 0, LINKED_RELOC_PLAIN, target_group};
    switch (type) {
    case R_PPC_ADDR16_LO:
    case R_PPC_ADDR16_HI:
    case R_PPC_ADDR16_HA:
        // An address follows its target.
        if (target_group == 0)
            return 1;
        reloc.keep_mask = address & 2u ? 0xFFFF0000u : 0x0000FFFFu;
        if (type != R_PPC_ADDR16_LO)
            reloc.kind = to_bss ? LINKED_RELOC_HA_BSS : LINKED_RELOC_HA_MODULE;
        break;
    case R_PPC_REL24:
    case R_PPC_REL14:
        // A displacement follows both ends.
        if (target_group == group)
            return 1;
        if (!module_link_add_entry(link, target_group, target))
            return 0;
        if (group == 0 && target_group == 0)
            return 1;
        reloc.keep_mask = type == R_PPC_REL24 ? 0xFC000003u : 0xFFFF0003u;
        break;
    default:
        if (target_group == 0 && group == 0)
            return 1;
        // Code takes no absolute words; one would need its own shift.
        fprintf(stderr, "error: relocation type %u in code at 0x%08X of a module of its own\n", type, address);
        return 0;
    }
    return module_link_add_reloc(link, &reloc);
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
