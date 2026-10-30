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
//
// Overlays are modules the game loads into memory other modules use at other
// times (one stage's module after another's). Each is its own group: group 0
// is everything resident, overlay k is group k + 1.

typedef struct {
    const char* module;  // the config's file name
    u32 group;
    u32 address;
    u32 size;
    u8* bytes;  // relocated, owned
} LinkedSection;

typedef struct {
    u32 group;
    u32 address;
} LinkedEntry;

// A word of an overlay's code that depends on where the game loads it: the
// bits outside keep_mask. An address-high also says whose shift its target
// moves by (EmitHaReloc in backend/emitter.h).
typedef enum {
    LINKED_RELOC_PLAIN,
    LINKED_RELOC_HA_MODULE,
    LINKED_RELOC_HA_BSS,
} LinkedRelocKind;

typedef struct {
    u32 group;
    u32 address;
    u32 keep_mask;
    LinkedRelocKind kind;
} LinkedReloc;

// Where an overlay was linked: its file at base, its bss (section
// bss_section of the module's table, ~0u for none) at bss.
typedef struct {
    u32 base;
    u32 bss;
    u32 bss_section;
} LinkedOverlay;

typedef struct {
    LinkedSection* text;
    u32 count;
    LinkedSection* data;  // the other loaded sections, where code pointers live
    u32 data_count;
    LinkedEntry* entries;  // where other code enters: exports, REL prologs/epilogs
    u32 entry_count;
    LinkedReloc* relocs;
    u32 reloc_count;
    LinkedOverlay overlays[RECOMP_MAX_MODULES];  // overlay k's at k
} ModuleLink;

// Each adds the config's modules to the link.
int rso_link(ModuleLink* link, const RecompConfig* config, const char* game_dir);
int rel_link(ModuleLink* link, const RecompConfig* config, const char* game_dir);

// Prints where each of the config's RSO modules is loaded in a dump of guest
// RAM (MEM1 at 0x80000000, MEM2 at 0x90000000 or NULL), as config lines
// checked against the config.
int rso_find(const RecompConfig* config, const char* game_dir, const u8* mem1, u32 mem1_size,
             const u8* mem2, u32 mem2_size);
// Where one RSO module is loaded in such a dump: 1 found, 0 not loaded, -1
// the module can't be read.
int rso_locate(const char* game_dir, const char* file, const u8* mem1, u32 mem1_size, const u8* mem2,
               u32 mem2_size, u32* base, u32* bss);

int module_link_add_section(ModuleLink* link, u32 group, bool code, const char* module,
                            u32 address, const u8* bytes, u32 size);
int module_link_add_entry(ModuleLink* link, u32 group, u32 address);
int module_link_add_reloc(ModuleLink* link, const LinkedReloc* reloc);
void module_link_free(ModuleLink* link);

#endif
