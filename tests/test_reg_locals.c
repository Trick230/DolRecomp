// SPDX-License-Identifier: GPL-3.0-or-later
// The rewrite that keeps registers in locals: what it loads, stores and refuses.
#include "backend/reg_locals.h"

#include <stdlib.h>
#include <string.h>

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            fprintf(stderr, "check failed: %s:%d: %s\n", __FILE__,        \
                    __LINE__, #cond);                                     \
            return 1;                                                     \
        }                                                                 \
    } while (0)

static const char* const g_path = "test_reg_locals.out";

// The rewrite of `input`, or NULL if it refused it. The caller frees it.
static char* rewrite_with(const char* input, bool all_registers) {
    FILE* out = fopen(g_path, "wb");
    char* text = (char*)malloc(strlen(input) + 1u);
    if (!out || !text)
        exit(1);
    strcpy(text, input);
    const bool ok = reg_locals_rewrite(out, text, all_registers);
    free(text);
    fclose(out);
    if (!ok)
        return NULL;
    FILE* in = fopen(g_path, "rb");
    if (!in)
        exit(1);
    char* result = (char*)calloc(1u, 1u << 16);
    if (!result)
        exit(1);
    fread(result, 1u, (1u << 16) - 1u, in);
    fclose(in);
    return result;
}

static char* rewrite(const char* input) {
    return rewrite_with(input, true);
}

#define HEAD "void f(CPUState* restrict ctx) {\n/*@reload*/\n"

int main(void) {
    // Read first: loaded. Written first: not. Only what changed is stored.
    char* text = rewrite(HEAD
                         "label_1:\n"
                         "    ctx->gpr[3] = ctx->gpr[4] + 1;\n"
                         "    return;\n"
                         "}\n");
    CHECK(text);
    CHECK(strstr(text, "reg_r4 = ctx->gpr[4];"));
    CHECK(!strstr(text, "reg_r3 = ctx->gpr[3];"));
    CHECK(strstr(text, "reg_r3 = reg_r4 + 1;"));
    CHECK(strstr(text, "ctx->gpr[3] = reg_r3;"));
    CHECK(!strstr(text, "ctx->gpr[4] = reg_r4;"));
    free(text);

    // A write only some paths make: the others must store what they found.
    text = rewrite(HEAD
                   "    if (ctx->pc) {\n"
                   "        ctx->gpr[5] = 1;\n"
                   "    }\n"
                   "    return;\n"
                   "}\n");
    CHECK(text);
    CHECK(strstr(text, "reg_r5 = ctx->gpr[5];"));
    CHECK(strstr(text, "ctx->gpr[5] = reg_r5;"));
    free(text);

    // A call: stores what changed, loads what the code after it reads and
    // nothing it writes first, and names the context in between.
    text = rewrite(HEAD
                   "    ctx->lr = 4;\n"
                   "    ctx->gpr[6] = 1;\n"
                   "/*@flush*/\n"
                   "    g(ctx);\n"
                   "    ctx->gpr[7] = ctx->lr;\n"
                   "/*@reload*/\n"
                   "    ctx->gpr[6] = ctx->gpr[8];\n"
                   "    ctx->cr = 0;\n"
                   "    return;\n"
                   "}\n");
    CHECK(text);
    CHECK(strstr(text, "ctx->lr = reg_lr;"));
    CHECK(strstr(text, "ctx->gpr[6] = reg_r6;"));
    CHECK(strstr(text, "ctx->gpr[7] = ctx->lr;"));
    CHECK(strstr(text, "reg_r8 = ctx->gpr[8];"));
    CHECK(!strstr(text, "reg_cr = ctx->cr;"));
    CHECK(strstr(text, "ctx->cr = reg_cr;"));
    free(text);

    // Exits past a flush store nothing, and a jump keeps the registers valid
    // around a loop.
    text = rewrite(HEAD
                   "label_2:\n"
                   "    ctx->ctr--;\n"
                   "    if (ctx->ctr != 0) {\n"
                   "        goto label_2;\n"
                   "    }\n"
                   "    ctx->pc = 1;\n"
                   "/*@flush*/\n"
                   "    h(ctx);\n"
                   "    return;\n"
                   "}\n");
    CHECK(text);
    CHECK(strstr(text, "reg_ctr = ctx->ctr;"));
    CHECK(strstr(text, "reg_ctr--;"));
    CHECK(strstr(text, "ctx->ctr = reg_ctr;\n    h(ctx);\n    return;"));
    CHECK(!strstr(text, "reg_exit"));
    free(text);

    // Shared exits, for exits storing the same registers.
    text = rewrite(HEAD
                   "    ctx->gpr[9] = 1;\n"
                   "    if (ctx->pc) return;\n"
                   "    if (ctx->exception) return;\n"
                   "    return;\n"
                   "}\n");
    CHECK(text);
    CHECK(strstr(text, "if (ctx->pc) goto reg_exit_0;"));
    CHECK(strstr(text, "if (ctx->exception) goto reg_exit_0;"));
    CHECK(strstr(text, "reg_exit_0:\n    ctx->gpr[9] = reg_r9;\n    return;"));
    CHECK(!strstr(text, "reg_exit_1"));
    free(text);

    // Text outside a function passes through; ctr and cr stay apart.
    text = rewrite("static int x;\n" HEAD "    ctx->cr = ctx->ctr;\n    return;\n}\n");
    CHECK(text);
    CHECK(strstr(text, "static int x;"));
    CHECK(strstr(text, "reg_cr = reg_ctr;"));
    free(text);

    // Chosen by what they cost: read once, a register stays in the context; in
    // a loop, it gets a local.
    text = rewrite_with(HEAD
                        "    ctx->gpr[3] = ctx->gpr[4];\n"
                        "label_3:\n"
                        "    ctx->ctr--;\n"
                        "    ctx->gpr[5] = ctx->gpr[5] + ctx->ctr;\n"
                        "    if (ctx->ctr != 0) goto label_3;\n"
                        "    return;\n"
                        "}\n",
                        false);
    CHECK(text);
    CHECK(strstr(text, "ctx->gpr[3] = ctx->gpr[4];"));
    CHECK(strstr(text, "reg_ctr--;"));
    CHECK(strstr(text, "reg_r5 = reg_r5 + reg_ctr;"));
    CHECK(strstr(text, "ctx->gpr[5] = reg_r5;"));
    free(text);

    // What it refuses: a reload losing a write, a register or an exit where
    // the holder depends on the path, an index known only at run time.
    fputs("expected errors:\n", stderr);
    CHECK(!rewrite(HEAD "    ctx->gpr[3] = 1;\n/*@reload*/\n    return;\n}\n"));
    CHECK(!rewrite(HEAD "    if (ctx->pc) {\n/*@flush*/\n    }\n    ctx->gpr[3] = 1;\n    return;\n}\n"));
    CHECK(!rewrite(HEAD
                   "    ctx->gpr[3] = 1;\n"
                   "    if (ctx->pc) {\n"
                   "/*@flush*/\n"
                   "        g(ctx);\n"
                   "    }\n"
                   "    return;\n"
                   "}\n"));
    CHECK(!rewrite(HEAD "    ctx->gpr[reg] = 1;\n    return;\n}\n"));
    CHECK(!rewrite(HEAD "    goto label_9;\n    return;\n}\n"));
    CHECK(!rewrite(HEAD "    (void)dolrecomp_enter_indirect(ctx, 4);\n    return;\n}\n"));

    remove(g_path);
    return 0;
}
