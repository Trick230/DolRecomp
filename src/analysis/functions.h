#ifndef DOLRECOMP_ANALYSIS_FUNCTIONS_H
#define DOLRECOMP_ANALYSIS_FUNCTIONS_H

#include "analysis/code_section.h"
#include "common/types.h"
#include "frontend/decoder.h"

// Splits the code into functions, the unit the recompiler emits as one host
// function, and finds the jump tables their computed branches go through.
//
// A function starts at every address code enters from elsewhere: a call
// target, a code pointer in data or built in a register, an export or a
// given seed. It runs to the next start, so every byte of code belongs to
// exactly one function. A direct branch that leaves its function then has to
// land on a start, so its target becomes one, and so on until nothing
// changes. A wrong start costs nothing but a tail call: control falls or
// branches into it as into any other function.

typedef struct {
    u32 address;
    u32 size;
    const u8* bytes;  // guest (big-endian) bytes
} GuestRegion;

typedef struct {
    u32 bctr;     // the bctr that indexes it
    u32 address;  // the table
    u32 count;
    u32* targets;
} JumpTable;

typedef struct {
    u32 start;
    u32 end;
} GuestFunction;

typedef struct {
    const LoadedCodeSection* sections;
    const PPCInst* const* insts;  // decoded, one array per section
    u32 section_count;
    const GuestRegion* data;      // read for code pointers and jump tables
    u32 data_count;
    const u32* seeds;             // entry point, exports, natives
    u32 seed_count;
} FunctionAnalysisInput;

typedef struct {
    GuestFunction* functions;  // sorted by address
    u32 function_count;
    JumpTable* tables;         // sorted by bctr
    u32 table_count;
} FunctionAnalysis;

int analyze_functions(FunctionAnalysis* out, const FunctionAnalysisInput* in);
void function_analysis_free(FunctionAnalysis* analysis);

// The jump table the bctr at `address` goes through, or NULL.
const JumpTable* function_analysis_table(const FunctionAnalysis* analysis, u32 address);
// The function covering `address`, or NULL.
const GuestFunction* function_analysis_find(const FunctionAnalysis* analysis, u32 address);

#endif
