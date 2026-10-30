#pragma once

// A game's recompiled code as the host runtime sees it: its functions, the
// guest code they were built from, and the two calls generated code makes
// into the runtime. guest_module.c builds it from the generated tables.

#include "core/cpu.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GuestRange
{
    u32 start;  // inclusive
    u32 end;    // exclusive
} GuestRange;

// One guest function. It runs from ctx->pc, its start or a point it can resume
// at, until it returns to its caller or stops early (the slice ended, an
// exception, a call it could not make directly); ctx->pc then holds the next
// pc. It leaves the pc alone with no cycles charged if it has no entry there.
typedef void (*GuestCode)(CPUState* ctx);

// A word of an overlay's code whose bits outside keep_mask depend on where
// the module loads.
typedef struct GuestReloc
{
    u32 address;
    u32 keep_mask;
} GuestReloc;

// A module the game loads at times into memory other code uses at other
// times: its functions overlap the resident ones and the other overlays'.
// Its code was compiled for one place (the file at base, its bss at bss) and
// runs wherever the game loads it, given the shifts below; every address
// here is a compiled one.
typedef struct GuestOverlay
{
    const char* name;
    const GuestRange* code_ranges;  // sorted, non-overlapping
    u32 num_code_ranges;
    // Its functions' slice of the module's, tiling its code ranges.
    u32 first_function;
    u32 num_functions;
    u32 base;
    u32 bss;          // 0 for none
    u32 bss_section;  // the bss's index in the module's section table
    // Sorted by address. Its functions' hashes are of their text with these
    // bits cleared.
    const GuestReloc* relocs;
    u32 num_relocs;
} GuestOverlay;

typedef struct GuestModule
{
    // The resident code. Sorted, non-overlapping.
    const GuestRange* code_ranges;
    u32 num_code_ranges;
    // The resident functions tile the code ranges exactly, sorted; each
    // overlay's follow. Each has the FNV-1a 64 hash of the guest text it was
    // built from: the runtime checks guest RAM against it before running the
    // function and again after any icache invalidation that touches it, so
    // of the functions over one address only the one matching RAM runs.
    const GuestRange* function_ranges;
    const GuestCode* functions;
    const u64* function_hashes;
    u32 num_functions;
    const GuestOverlay* overlays;
    u32 num_overlays;
} GuestModule;

extern const GuestModule g_guest_module;

// Overlay k's code runs as loaded at its base + dolrecomp_overlay_shift[k],
// its bss at bss + dolrecomp_overlay_bss_shift[k]. The host runtime's to set,
// while none of the overlay's functions counts as verified.
extern u32 dolrecomp_overlay_shift[];
extern u32 dolrecomp_overlay_bss_shift[];

// Which functions generated code may call directly (one flag per function,
// NULL for none), and the idle-loop pc that must keep returning to the runtime.
void guest_set_direct_calls(const unsigned char* function_ok, u32 idle_pc);

// The host runtime's: the verified function holding target, or NULL.
GuestCode dolrecomp_lookup(CPUState* ctx, u32 target);
// The host runtime's: the slice ran out at a loop about to go to pc. Runs the
// due events in place; nonzero if generated code must leave for the dispatcher.
int dolrecomp_safepoint(CPUState* ctx, u32 pc);

#ifdef __cplusplus
}
#endif
