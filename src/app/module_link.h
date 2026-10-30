#ifndef DOLRECOMP_APP_MODULE_LINK_H
#define DOLRECOMP_APP_MODULE_LINK_H

#include "app/config.h"
#include "common/types.h"

// Modules linked the way the game's own linker does at run time, so their
// code can be recompiled ahead of time: RSO modules (Wii) against the static
// module, REL modules (GameCube and Wii) against the DOL and each other. The
// game still loads them itself.
//
// Group 0 is the code compiled where it runs: the DOL and the modules the
// config gives a base. Every other module is a group of its own, from 1,
// compiled for one place and run wherever the game loads it (LinkedGroup):
// the resident modules the config gives no base, and the overlays, which the
// game loads at times into memory other modules use at other times (one
// stage's module after another's). If a module ever lands somewhere its code
// wasn't built for, the runtime sees different bytes in the function hashes
// and leaves that code alone.

#define LINK_MAX_GROUPS (4u * RECOMP_MAX_MODULES + 1u)

// Where modules of their own group are compiled for when the config doesn't
// say: the resident ones one after another from the end of the DOL's image,
// the overlays from the end of MEM1 down, out of each other's way.
#define LINK_OVERLAY_TOP 0x81800000u

// The ELF relocation types modules use (RSO and REL alike).
enum {
    R_PPC_ADDR32 = 1,
    R_PPC_ADDR16_LO = 4,
    R_PPC_ADDR16_HI = 5,
    R_PPC_ADDR16_HA = 6,
    R_PPC_REL24 = 10,
    R_PPC_REL14 = 11,
};

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

// A word of code that depends on where some module loads: the bits outside
// keep_mask. An address-high also says whose shift its target moves by: the
// target group's, of its file or its bss (EmitHaReloc in backend/emitter.h).
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
    u32 target;  // an address-high's target group
} LinkedReloc;

// A module of its own group: compiled with its file at base and its bss
// (section bss_section of its table, ~0u for none) at bss. The runtime finds
// where the game linked it by its file name (RSO) or module id (REL).
typedef struct {
    const char* file;
    u32 rel_id;  // 0 for an RSO module
    bool overlay;
    u32 base;
    u32 bss;
    u32 bss_section;
} LinkedGroup;

typedef struct {
    u32 start;
    u32 end;
} LinkedRange;

typedef struct {
    LinkedSection* text;
    u32 count;
    LinkedSection* data;  // the other loaded sections, where code pointers live
    u32 data_count;
    LinkedEntry* entries;  // where other code enters: exports, REL prologs/epilogs
    u32 entry_count;
    LinkedReloc* relocs;
    u32 reloc_count;
    LinkedGroup groups[LINK_MAX_GROUPS];  // group g's at g - 1
    u32 group_count;                      // with group 0
    // Placement of resident modules of their own group: the next free address
    // (set by the caller past the DOL's image), and what modules the config
    // places hold.
    u32 arena;
    LinkedRange reserved[2u * LINK_MAX_GROUPS];
    u32 reserved_count;
} ModuleLink;

// Each adds the config's modules to the link. rso_link takes the static
// module's section addresses (config.static_sections, or ones read off the
// DOL).
int rso_link(ModuleLink* link, const RecompConfig* config, const char* game_dir, const u32* static_sections,
             u32 static_section_count);
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
// A new group for a module of its own: its index, or 0 when there are too many.
u32 module_link_add_group(ModuleLink* link, const LinkedGroup* group);
// Memory a module the config places holds, kept out of the arena.
int module_link_reserve(ModuleLink* link, u32 address, u32 size);
// Where in the arena a resident module's next size bytes go (32-byte aligned).
u32 module_link_place(ModuleLink* link, u32 size);
// Notes a relocation (ELF R_PPC_* type) of the word holding address in group's
// code to target, in target_group (in its bss or not), if where either loads
// changes the word. A branch into another group also marks where it enters.
int module_link_note(ModuleLink* link, u32 group, u32 address, u8 type, u32 target_group, bool to_bss,
                     u32 target);
void module_link_free(ModuleLink* link);

#endif
