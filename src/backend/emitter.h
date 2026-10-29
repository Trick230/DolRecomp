#ifndef DOLRECOMP_EMITTER_H
#define DOLRECOMP_EMITTER_H

#include "../common/types.h"
#include "../frontend/decoder.h"
#include <stdio.h>

typedef enum {
    DOLRECOMP_CPU_GEKKO,
    DOLRECOMP_CPU_BROADWAY,
    DOLRECOMP_CPU_ESPRESSO,
} DolRecompCPU;

// Split C emitter used by the command-line recompiler.

// emit the boilerplate header (includes, typedefs, etc)
void emit_header(FILE* out);
void emit_header_for_cpu(FILE* out, DolRecompCPU cpu);

// The functions code being written can call, sorted: a call to one is a
// direct C call guarded by its dolrecomp_direct_ok flag. ids gives each one's
// index in the runtime's function table (NULL: its index here), tags the tag
// its C names carry (NULL or a NULL entry: none). Call before worker emission;
// passing count == 0 sends every call back to the runtime.
void emit_set_function_table(const u32* starts, const u32* ends, const u32* ids,
                             const char* const* tags, u32 count);

// The tag of the functions being written: an overlay's functions share
// addresses with other modules', so their C names (func_<tag><address>) carry
// one. "" or NULL for none.
void emit_set_symbol_tag(const char* tag);

// A guest function with a host implementation, `bool name(CPUState*)`, that
// the module links in. It returns true after doing the whole call (registers,
// memory, cycles) and false, with no side effects, when the guest code has to
// run instead. Same lifetime rules as the function table; sorted by address.
// A `jump` native ends somewhere other than its return address (a context
// load ending in rfi): it sets ctx->pc itself and its callers go back to the
// dispatcher instead of resuming after the call.
typedef struct {
    u32 address;
    const char* name;
    bool jump;
} DolRecompNative;
void emit_set_natives(const DolRecompNative* natives, u32 count);

// Every address code outside a function may enter it at, sorted (same lifetime
// rules as the function table). With it set, a function's entry switch keeps only
// those, its block leaders and the resume points after its own exits, so the
// compiler can optimize across the instructions in between; a runtime entering
// anywhere else finds no case and must step that instruction itself.
// Requires `out` to be readable back (opened "w+").
void emit_set_entry_targets(const u32* targets, u32 count);

// emit a single recompiled function as C code
bool emit_function(FILE* out, const PPCInst* insts, u32 count, u32 func_addr);

// emit a single instruction as C code
void emit_instruction(FILE* out, const PPCInst* inst);

// emit the boilerplate footer
void emit_footer(FILE* out);

#endif /* DOLRECOMP_EMITTER_H */

// i would just like to mention, that those comments have been here since the beginning of time.
// do not judge old me