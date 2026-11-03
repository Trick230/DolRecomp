#ifndef DOLRECOMP_APP_NATIVES_H
#define DOLRECOMP_APP_NATIVES_H

#include "analysis/code_section.h"
#include "backend/emitter.h"

// Natives are found by signature: a .sigs file lists the guest functions that
// have host implementations by their code, not their address, so one file
// serves every game built on the same SDK. One function per line:
//
//   <symbol> <size> <first> <hash> [at <expression>] [jump] [anywhere]
//
// size in hex bytes; first and hash, the masked first word and the FNV-1a 64
// of the masked code (`dolrecomp sign` prints the three). The mask hides what
// the linker decides: targets of branches that leave the function, lis
// immediates, small-data (r2/r13) offsets and the low halves paired with a lis.
// Everything else stays, struct offsets included, so a host implementation
// only replaces the SDK build it was written against. A symbol may have a line
// for each SDK build its host code is right for. Without `at` the code must
// occur once in the game; code too generic for that (a getter, an
// instruction hooked mid-function) is found `at` an expression of other
// entries and checked there. `jump` marks a native that sets the pc itself
// (see DolRecompNative). A symbol starting with '@' is an anchor: an entry
// that only locates others. '#' starts a comment.
//
// `anywhere` says the host code keeps no state in the game's globals (memcpy, a
// matrix routine): it reads and writes its arguments, at most a constant. It
// then also replaces copies of the code inside modules compiled to run
// anywhere, each found once per module (a module with two keeps its guest
// code). Such a module's globals aren't the ones the defines read off the
// static code, so other natives stay out of them.
//
// What differs between games on the same SDK (where a function or a global
// went, small-data offsets) is read off the code they matched:
//
//   define <NAME> <expression>
//
// which the recompiler writes to ppc_natives.h for the natives to compile
// against. Expressions: `S` or `S+off`, an address in entry S;
// `call(S+off)`, the target of the branch there; `addr(S+hi,lo)`, the address
// the lis at S+hi and the instruction at S+lo build; `imm(S+off)`, the signed
// 16-bit immediate there. Offsets are hex. A name may have a line for each SDK
// build, where the value sits elsewhere: the first the game can read wins.
//
// An entry the game doesn't have is skipped, with any define that needs it.
typedef struct {
    char* symbol;
    char* at;  // NULL to search
    u32 size;
    u32 first;
    u64 hash;
    int jump;
    int anywhere;
} NativeSignature;

typedef struct {
    char* name;
    char* expression;
} NativeDefine;

typedef struct {
    NativeSignature* entries;
    u32 count;
    NativeDefine* defines;
    u32 define_count;
} NativeSignatures;

typedef struct {
    DolRecompNative* entries;  // sorted by address then group, names owned by the list
    u32* sizes;
    u64* hashes;  // FNV-1a 64 of the code bytes, unmasked
    u32 count;
    char** define_names;  // in file order, owned by the list
    long long* define_values;
    u32 define_count;
} DolRecompNativeList;

// Appends the signatures in text (origin names it in errors).
int natives_parse(NativeSignatures* signatures, const char* text, const char* origin);
// Appends a .sigs file.
int natives_load(NativeSignatures* signatures, const char* path);
void natives_signatures_free(NativeSignatures* signatures);

// Finds the signatures in the code sections.
int natives_resolve(DolRecompNativeList* list, const NativeSignatures* signatures,
                    const LoadedCodeSection* sections, u32 section_count);
// Adds the `anywhere` signatures found in the code sections of group, a module
// compiled to run anywhere.
int natives_resolve_module(DolRecompNativeList* list, const NativeSignatures* signatures,
                           const LoadedCodeSection* sections, u32 section_count, u32 group);
// The size, first word and hash of a signature for the code at address.
int natives_sign(const LoadedCodeSection* sections, u32 section_count, u32 address, u32 size,
                 u32* first, u64* hash);

// The natives found, one "<address> <size> <hash> <symbol> [jump] [group <g>]"
// line each.
int natives_write_list(const DolRecompNativeList* list, const char* path);
// Writes the defines as a C header (an empty one without any).
int natives_write_header(const DolRecompNativeList* list, const char* path);
// Adds a define of the game's own (the config's [defines]) to the header.
int natives_add_define(DolRecompNativeList* list, const char* name, long long value);
void natives_free(DolRecompNativeList* list);

#endif
