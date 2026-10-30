#ifndef DOLRECOMP_APP_SIGDB_H
#define DOLRECOMP_APP_SIGDB_H

#include "analysis/functions.h"
#include "app/natives.h"

// Dolphin's signature database (Data/Sys/totaldb.dsy in a Dolphin install,
// GPL-2.0-or-later; read from wherever it is, never copied here): SDK
// function names keyed by their size and a checksum of the opcodes alone,
// registers and immediates ignored. That is enough to name SDK code in any
// game, not to replace it: functions of the same shape (two getters, a
// vector and a quaternion dot product) share a checksum. Natives keep their
// own signatures; the database names what the recompiler found and proposes
// signatures for a new game.
//
// File: u32 count, then count records { u32 checksum, u32 size, char[128]
// name }, little-endian; a name may carry "\t<library> <object>".

typedef struct {
    u32 checksum;
    u32 size;
    char name[128];  // the object after a tab
    int ambiguous;   // another record has this checksum and a different name
} SigDBEntry;

typedef struct {
    SigDBEntry* entries;  // sorted by checksum
    u32 count;
} SigDB;

int sigdb_load(SigDB* db, const char* path);
void sigdb_free(SigDB* db);

// Dolphin's HashSignatureDB::ComputeCodeChecksum over big-endian code.
u32 sigdb_checksum(const u8* code, u32 size);
// The entry naming code of this checksum and size, NULL when none or ambiguous.
const SigDBEntry* sigdb_find(const SigDB* db, u32 checksum, u32 size);

// Writes sdk_names.txt (<address> <size> <name> [<library> <object>]) for
// the functions the database names, and sigs_candidates.txt: for every
// signature the game did not match whose SDK name (the symbol past its first
// '_', leading underscores aside) the database found, the .sigs line the code
// there takes, to review before it goes into a .sigs file.
int sigdb_report(const SigDB* db, const FunctionAnalysis* analysis, const LoadedCodeSection* sections,
                 u32 section_count, const NativeSignatures* signatures, const DolRecompNativeList* natives,
                 const char* out_dir);

#endif
