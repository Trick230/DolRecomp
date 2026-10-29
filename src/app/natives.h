#ifndef DOLRECOMP_APP_NATIVES_H
#define DOLRECOMP_APP_NATIVES_H

#include "analysis/code_section.h"
#include "backend/emitter.h"

// --natives file: one guest function per line,
//   <address> <size> <fnv1a64 of its code bytes> <host symbol> [jump]
// all numbers hex, '#' starts a comment; `jump` marks a native that sets the
// pc itself (see DolRecompNative). A host implementation hard-codes the
// structure layout of the exact SDK build it was written against, so every
// entry is pinned to the guest bytes it replaces and a mismatch is an error.
//
// What differs between games built on the same SDK (where the linker put a
// function or a global, small-data offsets) comes from the same file as
//   define <NAME> <integer>
// lines (C syntax: 0x hex, decimal, negative), which the recompiler writes to
// ppc_natives.h for the natives to compile against, so one native source
// serves every game that lists it.
typedef struct {
    DolRecompNative* entries;  // sorted by address, names owned by the list
    u32* sizes;
    u64* hashes;
    u32 count;
    char** define_names;  // in file order, owned by the list
    long long* define_values;
    u32 define_count;
} DolRecompNativeList;

int natives_load(DolRecompNativeList* list, const char* path);
int natives_verify(const DolRecompNativeList* list, const LoadedCodeSection* sections,
                   u32 section_count);
// Writes the defines as a C header (an empty one without any).
int natives_write_header(const DolRecompNativeList* list, const char* path);
void natives_free(DolRecompNativeList* list);

#endif
