// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef DOLRECOMP_REG_LOCALS_H
#define DOLRECOMP_REG_LOCALS_H

#include "../common/types.h"
#include <stdio.h>

// Rewrites generated functions so the guest's integer registers, CR, XER, LR
// and CTR live in C locals, which the compiler keeps in host registers across
// guest memory accesses, instead of in the context.
//
// The emitter still writes them as ctx->gpr[n] and so on, with a line
// `/*@flush*/` before anything that needs the context current, after which
// the context holds them, and a line `/*@reload*/` where locals take over
// again. Every return stores what locals hold newer. A data flow over the
// function's lines stores only registers that may have changed and loads only
// registers that are read before being written. A register gets a local only
// where it is touched well more often than it has to move; `all_registers`
// gives every one a local, for tests.
//
// `text` holds whole functions and is modified. Returns false, with a message
// on stderr, if the markers leave a register's holder undecided.
bool reg_locals_rewrite(FILE* out, char* text, bool all_registers);

#endif /* DOLRECOMP_REG_LOCALS_H */
