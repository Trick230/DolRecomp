// Glue between the host runtime and the code DolRecomp generated for a game;
// it compiles with the game's generated sources (see guest_module.h).

#include "ppc_recomp.h"

#include "core/guest_module.h"

#include "ppc_tables.inc"

_Static_assert(sizeof(s_functions) / sizeof(s_functions[0]) ==
                   sizeof(s_function_ranges) / sizeof(s_function_ranges[0]),
               "one range per function");

const GuestModule g_guest_module = {
    s_code_ranges,
    sizeof(s_code_ranges) / sizeof(s_code_ranges[0]),
    s_function_ranges,
    s_functions,
    s_function_hashes,
    sizeof(s_functions) / sizeof(s_functions[0]),
#if DOLRECOMP_OVERLAY_COUNT
    s_overlays,
#else
    NULL,
#endif
    DOLRECOMP_OVERLAY_COUNT,
    s_resident_relocs,
    DOLRECOMP_RESIDENT_RELOC_COUNT,
};

u32 dolrecomp_overlay_shift[DOLRECOMP_OVERLAY_COUNT ? DOLRECOMP_OVERLAY_COUNT : 1u];
u32 dolrecomp_overlay_bss_shift[DOLRECOMP_OVERLAY_COUNT ? DOLRECOMP_OVERLAY_COUNT : 1u];

void guest_set_direct_calls(const unsigned char* function_ok, u32 idle_pc)
{
    dolrecomp_idle_pc = idle_pc;
    dolrecomp_direct_ok = function_ok;
}
