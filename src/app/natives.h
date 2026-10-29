#ifndef DOLRECOMP_APP_NATIVES_H
#define DOLRECOMP_APP_NATIVES_H

#include "analysis/code_section.h"
#include "backend/emitter.h"

// Natives are found by signature: a .sigs file lists the guest functions that
// have host implementations by their code, not their address, so one file
// serves every game built on the same SDK. One function per line:
//
//   <symbol> <size> <first> <hash> [at <expression>] [jump]
//
// size in hex bytes; first and hash, the masked first word and the FNV-1a 64
// of the masked code (`dolrecomp sign` prints the three). The mask hides what
// the linker decides: targets of branches that leave the function, lis
// immediates, small-data (r2/r13) offsets and the low halves paired with a lis.
// Everything else stays, struct offsets included, so a host implementation
// only replaces the SDK build it was written against. Without `at` the code
// must occur once in the game; code too generic for that (a getter, an
// instruction hooked mid-function) is found `at` an expression of other
// entries and checked there. `jump` marks a native that sets the pc itself
// (see DolRecompNative). A symbol starting with '@' is an anchor: an entry
// that only locates others. '#' starts a comment.
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
// 16-bit immediate there. Offsets are hex.
//
// An entry the game doesn't have is skipped, with any define that needs it.
typedef struct {
    char* symbol;
    char* at;  // NULL to search
    u32 size;
    u32 first;
    u64 hash;
    int jump;
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
    DolRecompNative* entries;  // sorted by address, names owned by the list
    u32* sizes;
    u64* hashes;  // FNV-1a 64 of the code bytes, unmasked
    u32 count;
    char** define_names;  // in file order, owned by the list
    long long* define_values;
    u32 define_count;
} DolRecompNativeList;

// Appends the signatures in text (origin names it in errors).
int natives_parse(NativeSignatures* signatures, const char* text, const char* origin);
// Appends a .sigs file, or "@name": a set built into DolRecomp, beside the
// natives it implements (runtime/sdk/<name>.sigs).
int natives_load(NativeSignatures* signatures, const char* path);
void natives_signatures_free(NativeSignatures* signatures);

// Finds the signatures in the code sections.
int natives_resolve(DolRecompNativeList* list, const NativeSignatures* signatures,
                    const LoadedCodeSection* sections, u32 section_count);
// The size, first word and hash of a signature for the code at address.
int natives_sign(const LoadedCodeSection* sections, u32 section_count, u32 address, u32 size,
                 u32* first, u64* hash);

// The natives found, one "<address> <size> <hash> <symbol> [jump]" line each.
int natives_write_list(const DolRecompNativeList* list, const char* path);
// Writes the defines as a C header (an empty one without any).
int natives_write_header(const DolRecompNativeList* list, const char* path);
void natives_free(DolRecompNativeList* list);

// Built into the recompiler (builtin_signatures.c, generated).
const char* natives_builtin_signatures(const char* name);

#endif
