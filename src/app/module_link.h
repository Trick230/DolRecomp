#ifndef DOLRECOMP_APP_MODULE_LINK_H
#define DOLRECOMP_APP_MODULE_LINK_H

#include "app/config.h"
#include "common/types.h"

// Modules linked at their known load addresses, the way the game's own linker
// does at run time, so their code can be recompiled ahead of time: RSO modules
// (Wii) against the static module, REL modules (GameCube and Wii) against the
// DOL and each other. The game still loads them itself; if one ever lands
// elsewhere, the runtime sees different bytes in the function hashes and
// leaves that code alone.

typedef struct {
    const char* module;  // the config's file name
    u32 address;
    u32 size;
    u8* bytes;  // relocated, owned
} LinkedSection;

typedef struct {
    LinkedSection* text;
    u32 count;
    LinkedSection* data;  // the other loaded sections, where code pointers live
    u32 data_count;
    u32* entries;  // where other code enters: RSO exports, REL prologs/epilogs
    u32 entry_count;
} ModuleLink;

// Each adds the config's modules to the link.
int rso_link(ModuleLink* link, const RecompConfig* config, const char* game_dir);
int rel_link(ModuleLink* link, const RecompConfig* config, const char* game_dir);

int module_link_add_section(ModuleLink* link, bool code, const char* module, u32 address,
                            const u8* bytes, u32 size);
int module_link_add_entry(ModuleLink* link, u32 address);
void module_link_free(ModuleLink* link);

#endif
