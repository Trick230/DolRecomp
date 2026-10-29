#ifndef DOLRECOMP_APP_CONFIG_H
#define DOLRECOMP_APP_CONFIG_H

#include "common/types.h"

// A game's recompilation, as one TOML file. Paths in [main] and [rso] are
// relative to the game's extracted disc, natives' files to the config file
// ("@name" is a signature set built into DolRecomp, see app/natives.h).
//
//   [main]
//   dol = "sys/main.dol"
//   natives = ["@mwcc", "@rvl", "sdk.sigs"]
//   shards = 64
//
//   [rso]
//   sel = "files/rso/NightsWii.sel"
//   static_sections = [0, 0x80004000, ...]
//   modules = [{ file = "files/rso/global.rso", base = 0x8106E4C0, bss = 0x812C7F80 }, ...]

#define RECOMP_MAX_STATIC_SECTIONS 32u
#define RECOMP_MAX_RSO_MODULES 32u
#define RECOMP_MAX_NATIVES_FILES 16u

typedef struct {
    char file[512];
    u32 base;
    u32 bss;  // 0 when the module has no bss
} RecompRsoModule;

typedef struct {
    char dol[512];
    char natives[RECOMP_MAX_NATIVES_FILES][1024];
    u32 natives_count;
    u32 shards;

    char sel[512];  // empty without [rso]
    u32 static_sections[RECOMP_MAX_STATIC_SECTIONS];
    u32 static_section_count;
    RecompRsoModule modules[RECOMP_MAX_RSO_MODULES];
    u32 module_count;
} RecompConfig;

int config_load(RecompConfig* config, const char* path);

#endif
