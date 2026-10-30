#ifndef DOLRECOMP_APP_CONFIG_H
#define DOLRECOMP_APP_CONFIG_H

#include "backend/emitter.h"
#include "common/types.h"

// A game's recompilation, as one TOML file. Paths in [main], [rso] and [rel]
// are relative to the game's extracted disc, natives' files and the sigdb to
// the config file unless absolute (see app/natives.h: the game's runtime
// implements what they name; app/sigdb.h for the sigdb).
//
//   [main]
//   dol = "sys/main.dol"
//   cpu = "broadway"
//   natives = ["mwcc.sigs", "rvl.sigs", "sdk.sigs"]
//   sigdb = "C:/Program Files/Dolphin/Sys/totaldb.dsy"
//   shards = 64
//
//   [rso]
//   sel = "files/rso/NightsWii.sel"
//   static_sections = [0, 0x80004000, ...]
//   modules = [{ file = "files/rso/global.rso", base = 0x8106E4C0, bss = 0x812C7F80 }, ...]
//   overlay_heap_top = 0x81796040
//   overlays = [{ file = "files/rso/stage1000.rso" }, ...]
//
//   [rel]
//   modules = [{ file = "files/RELS.arc/rels/d_a_npc.rel" }, ...]
//   overlays = [{ file = "files/RELS.arc/rels/d_stage1.rel" }, ...]
//
//   [defines]
//   IDLE_PC = 0x8036623C
//
// cpu is the console's: "gekko" (GameCube) or "broadway" (Wii, the default).
// The output's header defines DOLRECOMP_CPU_GEKKO or DOLRECOMP_CPU_BROADWAY.
// [defines] are constants of the game's own that its runtime compiles
// against, written to ppc_natives.h with the ones the natives list reads off
// the code (see app/natives.h).
//
// A module's file may run through RARC or U8 archives, either of them or the
// file Yaz0-compressed ("files/RELS.szs/rels/stage.rel"; app/game_file.h).
//
// Modules are resident (modules, always loaded once loaded) or overlays
// (loaded at times into memory others use; each links against the resident
// modules alone). Every overlay, and every resident module without a base,
// is compiled for one place and runs wherever the game loads it (see
// app/module_link.h), so a config needs no addresses: base and bss are
// optional. A resident module's base, where the game always loads its file,
// with its bss, what the game hands the linker for it, compiles it for where
// it runs (no faster in practice). `dolrecomp rso-find` reads where RSO
// modules loaded from a RAM dump.
// With overlay_heap_top, an RSO overlay with no base is compiled for where the
// SDK's expanded heap hands out the file and then the bss from the tail of
// free space ending there (32-byte aligned, a 0x10-byte block header below
// each): base = top - round32(file size), bss = base - round32(bss size +
// 0x10). That is where NiGHTS loads them; any other place works as well.
// static_sections, the static module's section addresses by index, may be
// left out for a DOL of the usual layout (2 text and 8 data sections); they
// are then read off it.

#define RECOMP_MAX_STATIC_SECTIONS 32u
#define RECOMP_MAX_MODULES 32u
#define RECOMP_MAX_NATIVES_FILES 16u
#define RECOMP_MAX_DEFINES 32u

typedef struct {
    char file[512];
    u32 base;
    u32 bss;  // 0 when not given
} RecompModule;

typedef struct {
    char name[64];
    u32 value;
} RecompDefine;

typedef struct {
    char dol[512];
    DolRecompCPU cpu;
    char natives[RECOMP_MAX_NATIVES_FILES][1024];
    u32 natives_count;
    char sigdb[1024];  // empty without one
    u32 shards;

    char sel[512];  // empty without [rso]
    u32 static_sections[RECOMP_MAX_STATIC_SECTIONS];
    u32 static_section_count;
    RecompModule rso_modules[RECOMP_MAX_MODULES];
    u32 rso_module_count;
    RecompModule rso_overlays[RECOMP_MAX_MODULES];
    u32 rso_overlay_count;
    u32 rso_overlay_heap_top;  // 0 when not given

    RecompModule rel_modules[RECOMP_MAX_MODULES];  // [rel]
    u32 rel_module_count;
    RecompModule rel_overlays[RECOMP_MAX_MODULES];
    u32 rel_overlay_count;

    RecompDefine defines[RECOMP_MAX_DEFINES];  // [defines]
    u32 define_count;
} RecompConfig;

int config_load(RecompConfig* config, const char* path);

#endif
