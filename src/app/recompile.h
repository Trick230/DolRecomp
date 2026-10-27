#ifndef DOLRECOMP_APP_RECOMPILE_H
#define DOLRECOMP_APP_RECOMPILE_H

#include "app/config.h"

// Recompiles the game a config describes into out_dir:
//   ppc_recomp.h          declarations and the runtime interface
//   chunks/*.c            the code, a run of guest functions per file
//   ppc_recomp.<n>.c      config.shards files that include the chunks, so the
//                         build knows its sources before the recompiler runs
//   ppc_tables.inc        code ranges, and each function's range, code and
//                         text hash
//   functions.txt         the functions found, for comparing with symbol maps
int recompile(const RecompConfig* config, const char* game_dir, const char* out_dir);

#endif
