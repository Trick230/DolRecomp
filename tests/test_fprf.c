/* The inline fast paths leave FPSCR[FPRF] to be classified when it is read.
 * Each case runs one instruction through its fast path and through the
 * out-of-line path, which writes FPRF at once, and compares the whole
 * register file after ppc_fpscr_sync, including a compare that follows. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "core/cpu.h"

static const f64 values[] = {
    0.0, -0.0, 1.0, -1.5, 3.0e-39, -1.0e-40, 1.1754943508222875e-38, 3.4e38,
    1.0e300, -2.0e-310, INFINITY, -INFINITY, 0.1, -7.25,
};
#define VALUE_COUNT (sizeof(values) / sizeof(values[0]))

enum { D = 1, A = 2, B = 3, C = 4 };

typedef void (*Op)(CPUState* cpu);

static void fadds_fast(CPUState* c) { ppc_fadds_inline(c, D, A, B); }
static void fadds_slow(CPUState* c) { ppc_fadds(c, D, A, B); }
static void fsubs_fast(CPUState* c) { ppc_fsubs_inline(c, D, A, B); }
static void fsubs_slow(CPUState* c) { ppc_fsubs(c, D, A, B); }
static void fmuls_fast(CPUState* c) { ppc_fmuls_inline(c, D, A, C); }
static void fmuls_slow(CPUState* c) { ppc_fmuls(c, D, A, C); }
static void fdivs_fast(CPUState* c) { ppc_fdivs_inline(c, D, A, B); }
static void fdivs_slow(CPUState* c) { ppc_fdivs(c, D, A, B); }
static void fadd_fast(CPUState* c) { ppc_fadd_inline(c, D, A, B); }
static void fadd_slow(CPUState* c) { ppc_fadd(c, D, A, B); }
static void fmul_fast(CPUState* c) { ppc_fmul_inline(c, D, A, C); }
static void fmul_slow(CPUState* c) { ppc_fmul(c, D, A, C); }
static void frsp_fast(CPUState* c) { ppc_frsp_inline(c, D, B); }
static void frsp_slow(CPUState* c) { ppc_frsp(c, D, B); }
static void ps_sub_fast(CPUState* c) { ppc_ps_sub_inline(c, D, A, B); }
static void ps_sub_slow(CPUState* c) { ppc_ps_sub_op(c, D, A, B); }
static void ps_madd_fast(CPUState* c) { ppc_ps_madd_inline(c, D, A, C, B, false, false); }
static void ps_madd_slow(CPUState* c) { ppc_ps_madd_op(c, D, A, C, B, false, false); }
static void fmadds_fast(CPUState* c) {
    f64 out;
    if (ppc_fma_inline(c, c->fpr[A], c->fpr[C], c->fpr[B], true, false, false, &out))
        c->fpr[D] = c->ps1[D] = out;
}
/* What generated fmadds falls back to when the fast path declines. */
static void fmadds_slow(CPUState* c) {
    f64 out;
    if (ppc_fma(c, c->fpr[A], c->fpr[C], c->fpr[B], true, false, false, &out))
        c->fpr[D] = c->ps1[D] = out;
}

static const struct {
    const char* name;
    Op fast, slow;
} ops[] = {
    {"fadds", fadds_fast, fadds_slow}, {"fsubs", fsubs_fast, fsubs_slow},
    {"fmuls", fmuls_fast, fmuls_slow}, {"fdivs", fdivs_fast, fdivs_slow},
    {"fadd", fadd_fast, fadd_slow},    {"fmul", fmul_fast, fmul_slow},
    {"frsp", frsp_fast, frsp_slow},    {"ps_sub", ps_sub_fast, ps_sub_slow},
    {"ps_madd", ps_madd_fast, ps_madd_slow}, {"fmadds", fmadds_fast, fmadds_slow},
};

static void load(CPUState* c, f64 a, f64 b) {
    memset(c, 0, sizeof(*c));
    c->fpr[A] = c->ps1[A] = a;
    c->fpr[B] = c->ps1[B] = b;
    c->fpr[C] = c->ps1[C] = b;
    c->fpscr = 0x0001F000u; /* stale FPRF the result must replace */
}

static bool same(const CPUState* x, const CPUState* y) {
    return x->fpscr == y->fpscr && x->cr == y->cr &&
           memcmp(&x->fpr[D], &y->fpr[D], sizeof(f64)) == 0 &&
           memcmp(&x->ps1[D], &y->ps1[D], sizeof(f64)) == 0;
}

int main(void) {
    static CPUState fast, slow;
    int failures = 0, cases = 0;
    for (size_t o = 0; o < sizeof(ops) / sizeof(ops[0]); ++o) {
        for (size_t i = 0; i < VALUE_COUNT; ++i) {
            for (size_t j = 0; j < VALUE_COUNT; ++j) {
                for (int compare = 0; compare < 2; ++compare) {
                    load(&fast, values[i], values[j]);
                    load(&slow, values[i], values[j]);
                    ops[o].fast(&fast);
                    ops[o].slow(&slow);
                    if (compare) {
                        ppc_fcmp_inline(&fast, 6, fast.fpr[D], values[j], false);
                        ppc_fcmp(&slow, 6, slow.fpr[D], values[j], false);
                    }
                    ppc_fpscr_sync(&fast);
                    ++cases;
                    if (!same(&fast, &slow)) {
                        if (++failures <= 10)
                            printf("%s(%g, %g)%s: fpscr %08X vs %08X\n", ops[o].name,
                                   values[i], values[j], compare ? " + fcmpu" : "",
                                   fast.fpscr, slow.fpscr);
                    }
                }
            }
        }
    }
    printf("%d/%d cases match\n", cases - failures, cases);
    return failures != 0;
}
