#include "analysis/functions.h"

#include <stdlib.h>
#include <string.h>

// --- Code and data lookup -----------------------------------------------------

typedef struct {
    const FunctionAnalysisInput* in;
    u8** starts;  // per section, one flag per instruction
} Analysis;

static s32 section_of(const FunctionAnalysisInput* in, u32 address) {
    if (address & 3u)
        return -1;
    for (u32 i = 0; i < in->section_count; i++) {
        if (address - in->sections[i].address < in->sections[i].size)
            return (s32)i;
    }
    return -1;
}

static const u8* data_bytes(const FunctionAnalysisInput* in, u32 address, u32 size) {
    for (u32 i = 0; i < in->data_count; i++) {
        const GuestRegion* r = &in->data[i];
        if (address - r->address < r->size && size <= r->size - (address - r->address))
            return r->bytes + (address - r->address);
    }
    return NULL;
}

static bool mark_start(Analysis* a, u32 address) {
    const s32 s = section_of(a->in, address);
    if (s < 0)
        return false;
    u8* flag = &a->starts[s][(address - a->in->sections[s].address) / 4u];
    const bool added = !*flag;
    *flag = 1;
    return added;
}

// --- Register definitions -----------------------------------------------------

// The GPRs an instruction may write, as a mask. Conservative: anything not
// known to leave a register alone counts as writing both of its register
// fields.
static u32 gpr_writes(u32 raw) {
    const u32 op = raw >> 26, rd = (raw >> 21) & 31u, ra = (raw >> 16) & 31u;
    const u32 d = 1u << rd, a = 1u << ra;
    switch (op) {
    case 7: case 8: case 12: case 13: case 14: case 15:  // mulli subfic addic(.) addi addis
    case 32: case 34: case 40: case 42:                  // lwz lbz lhz lha
        return d;
    case 33: case 35: case 41: case 43:                  // the same with update
        return d | a;
    case 46:                                             // lmw
        return ~0u << rd;
    case 20: case 21: case 23: case 24: case 25: case 26: case 27: case 28: case 29:
        return a;                                        // rotates and logical immediates
    case 37: case 39: case 45: case 49: case 51: case 53: case 55:
        return a;                                        // stores and FP loads with update
    case 10: case 11: case 16: case 17: case 18: case 19:
    case 36: case 38: case 44: case 47: case 48: case 50: case 52: case 54:
        return 0;                                        // compares, branches, stores, FP loads
    case 31:
        switch ((raw >> 1) & 0x3FFu) {
        case 0: case 4: case 32:                         // cmp tw cmpl
        case 150: case 151: case 215: case 407: case 662: case 918:  // stores
        case 661: case 725: case 663: case 727: case 983:  // string and FP stores
        case 535: case 599:                              // FP loads
        case 54: case 86: case 246: case 278: case 470: case 982: case 1014:  // cache
        case 144: case 146: case 210: case 242: case 467:  // mt*
        case 306: case 566: case 598: case 854:          // tlbie tlbsync sync eieio
            return 0;
        case 183: case 247: case 439: case 695: case 759:  // stores with update
        case 567: case 631:                              // FP loads with update
            return a;
        case 23: case 87: case 279: case 343: case 534: case 790: case 339: case 371:
        case 19: case 83: case 595: case 659:
            return d;                                    // indexed loads, mf*
        case 55: case 119: case 311: case 375:
            return d | a;                                // indexed loads with update
        case 28: case 60: case 124: case 284: case 316: case 412: case 444: case 476:
        case 24: case 536: case 792: case 824: case 26: case 922: case 954:
            return a;                                    // logical, shifts, cntlzw, exts*
        default:
            break;
        }
        switch ((raw >> 1) & 0x1FFu) {                   // XO form, either OE
        case 8: case 10: case 11: case 40: case 75: case 104: case 136: case 138:
        case 200: case 202: case 232: case 234: case 235: case 266: case 459: case 491:
            return d;                                    // add/sub/neg/mul/div
        default:
            return d | a;
        }
    default:
        return d | a;
    }
}

// The constant in `reg` just before instruction `index`, if straight-line
// code before it builds one from lis/addi/ori.
static bool resolve_constant(const PPCInst* insts, u32 index, u32 reg, u32* value, u32 depth) {
    if (depth > 3)
        return false;
    for (u32 back = 1; back <= 64 && back <= index; back++) {
        const PPCInst* in = &insts[index - back];
        // Not a dataflow analysis: an address loaded before a loop is still
        // found across the branch into it. Only a return ends the search, and
        // a wrong table costs nothing (see find_jump_table).
        if ((in->op == PPC_OP_BCLR && in->bo == 20) || in->op == PPC_OP_RFI)
            return false;
        if (!(gpr_writes(in->raw) & (1u << reg)))
            continue;
        u32 base = 0;
        switch (in->op) {
        case PPC_OP_ADDI:
            if (in->rA && !resolve_constant(insts, index - back, in->rA, &base, depth + 1))
                return false;
            *value = base + (u32)(s32)in->simm;
            return true;
        case PPC_OP_ADDIS:
            if (in->rA && !resolve_constant(insts, index - back, in->rA, &base, depth + 1))
                return false;
            *value = base + ((u32)(u16)in->simm << 16);
            return true;
        case PPC_OP_ORI:
            if (!resolve_constant(insts, index - back, in->rS, &base, depth + 1))
                return false;
            *value = base | in->uimm;
            return true;
        default:
            return false;
        }
    }
    return false;
}

// --- Jump tables --------------------------------------------------------------

static bool is_bound_compare(const PPCInst* in, u32 index_reg) {
    return in->op == PPC_OP_CMPLI && in->rA == index_reg && in->l == 0;
}

// The case count of compare `cmp` if a bgt (or bgtlr) on its field follows it
// before instruction `end`.
static u32 guarded_count(const PPCInst* insts, u32 cmp, u32 end) {
    const u32 bi = insts[cmp].crfD * 4u + 1u;
    for (u32 i = cmp + 1; i < end; i++) {
        const PPCInst* b = &insts[i];
        if ((b->op == PPC_OP_BC || b->op == PPC_OP_BCLR) && !b->lk && (b->bo == 12 || b->bo == 4) &&
            b->bi == bi)
            return (u32)insts[cmp].uimm + 1u;
    }
    return 0;
}

// The case count of `index_reg`, scaled at `scale` and loaded at `load`,
// bounded by `cmplwi crN, index_reg, N; bgt`. The compare usually comes
// before the scaling, but MWCC may also schedule it between the two.
static u32 jump_table_bound(const PPCInst* insts, u32 scale, u32 load, u32 index_reg) {
    for (u32 i = scale + 1; i < load; i++) {
        if (is_bound_compare(&insts[i], index_reg))
            return guarded_count(insts, i, load);
        if (gpr_writes(insts[i].raw) & (1u << index_reg))
            break;
    }
    for (u32 back = 1; back <= 16 && back <= scale; back++) {
        if (is_bound_compare(&insts[scale - back], index_reg))
            return guarded_count(insts, scale - back, load);
        if (gpr_writes(insts[scale - back].raw) & (1u << index_reg))
            return 0;
    }
    return 0;
}

// MWCC's switch: cmplwi; bgt default (or bgtlr); table address into rT;
// rlwinm rI, rIdx, 2, 0, 29; lwzx rX, rT, rI; mtctr rX; bctr. Its cases
// become local labels; a target the table misses still goes through the
// dynamic branch, so a misread table is slow, never wrong.
static bool find_jump_table(const FunctionAnalysisInput* in, u32 section, u32 bctr, JumpTable* out) {
    const PPCInst* insts = in->insts[section];
    u32 mtctr = 0, lwzx = 0;
    for (u32 back = 1; back <= 3 && back <= bctr && !mtctr; back++) {
        const PPCInst* i = &insts[bctr - back];
        if (i->op == PPC_OP_MTSPR && i->spr == 9)
            mtctr = bctr - back;
    }
    if (!mtctr)
        return false;
    const u32 target_reg = insts[mtctr].rS;
    for (u32 back = 1; back <= 4 && back <= mtctr && !lwzx; back++) {
        const PPCInst* i = &insts[mtctr - back];
        if (i->op == PPC_OP_LWZX && i->rD == target_reg)
            lwzx = mtctr - back;
        else if (gpr_writes(i->raw) & (1u << target_reg))
            return false;
    }
    if (!lwzx)
        return false;

    // Either lwzx operand can hold the table; the other holds the scaled index.
    const PPCInst* load = &insts[lwzx];
    u32 table = 0, scaled = 0;
    if (resolve_constant(insts, lwzx, load->rA, &table, 0))
        scaled = load->rB;
    else if (resolve_constant(insts, lwzx, load->rB, &table, 0))
        scaled = load->rA;
    else
        return false;

    u32 count = 0;
    for (u32 back = 1; back <= 24 && back <= lwzx; back++) {
        const PPCInst* i = &insts[lwzx - back];
        if (!(gpr_writes(i->raw) & (1u << scaled)))
            continue;
        if (i->op == PPC_OP_RLWINM && i->sh == 2 && i->mb == 0 && i->me == 29)
            count = jump_table_bound(insts, lwzx - back, lwzx, i->rS);
        break;
    }
    if (count == 0 || count > 4096)
        return false;
    const u8* words = data_bytes(in, table, count * 4u);
    if (!words)
        return false;
    u32* targets = (u32*)malloc(count * sizeof(u32));
    if (!targets)
        return false;
    for (u32 i = 0; i < count; i++) {
        targets[i] = read_be32(words + i * 4u);
        if (section_of(in, targets[i]) != (s32)section) {
            free(targets);
            return false;
        }
    }
    *out = (JumpTable){in->sections[section].address + bctr * 4u, table, count, targets};
    return true;
}

static int compare_table(const void* left, const void* right) {
    const u32 a = ((const JumpTable*)left)->bctr, b = ((const JumpTable*)right)->bctr;
    return a < b ? -1 : a > b;
}

static int compare_range(const void* left, const void* right) {
    const u32 a = ((const GuestFunction*)left)->start, b = ((const GuestFunction*)right)->start;
    return a < b ? -1 : a > b;
}

// Whether `address` holds a word of a jump table; `ranges` are the tables'
// extents, sorted.
static bool in_table(const GuestFunction* ranges, u32 count, u32 address) {
    u32 lo = 0, hi = count;
    while (lo < hi) {
        const u32 mid = lo + (hi - lo) / 2u;
        if (ranges[mid].start <= address)
            lo = mid + 1u;
        else
            hi = mid;
    }
    return lo && address < ranges[lo - 1u].end;
}

// --- Starts -------------------------------------------------------------------

static bool seed_starts(Analysis* a, const FunctionAnalysis* out) {
    const FunctionAnalysisInput* in = a->in;
    GuestFunction* tables = (GuestFunction*)malloc((out->table_count + 1u) * sizeof(GuestFunction));
    if (!tables)
        return false;
    for (u32 i = 0; i < out->table_count; i++)
        tables[i] = (GuestFunction){out->tables[i].address,
                                    out->tables[i].address + out->tables[i].count * 4u};
    qsort(tables, out->table_count, sizeof(GuestFunction), compare_range);
    for (u32 s = 0; s < in->section_count; s++)
        mark_start(a, in->sections[s].address);
    for (u32 i = 0; i < in->seed_count; i++)
        mark_start(a, in->seeds[i]);

    for (u32 s = 0; s < in->section_count; s++) {
        const PPCInst* insts = in->insts[s];
        const u32 count = in->sections[s].size / 4u;
        for (u32 i = 0; i < count; i++) {
            const PPCInst* inst = &insts[i];
            if ((inst->op == PPC_OP_B || inst->op == PPC_OP_BC) && inst->lk)
                mark_start(a, inst->branch_target);
            // Code addresses built in registers: callbacks handed to the OS or
            // stored in a structure, often several from one lis.
            if (inst->op == PPC_OP_ADDIS && inst->rA == 0) {
                const u32 hi = (u32)(u16)inst->simm << 16;
                for (u32 j = i + 1; j < count && j <= i + 32u; j++) {
                    const PPCInst* lo = &insts[j];
                    if (lo->op == PPC_OP_ADDI && lo->rA == inst->rD)
                        mark_start(a, hi + (u32)(s32)lo->simm);
                    else if (lo->op == PPC_OP_ORI && lo->rS == inst->rD)
                        mark_start(a, hi | lo->uimm);
                    if ((gpr_writes(lo->raw) & (1u << inst->rD)) || (lo->op == PPC_OP_BCLR && lo->bo == 20))
                        break;
                }
            }
        }
    }

    // Code pointers in data: vtables, callback tables. Jump tables hold
    // branch targets inside one function, not starts.
    for (u32 r = 0; r < in->data_count; r++) {
        const GuestRegion* region = &in->data[r];
        for (u32 offset = 0; offset + 4u <= region->size; offset += 4u) {
            const u32 word = read_be32(region->bytes + offset);
            if (section_of(in, word) >= 0 &&
                !in_table(tables, out->table_count, region->address + offset))
                mark_start(a, word);
        }
    }
    free(tables);
    return true;
}

// The next start after instruction `i` of section `s`, as an index.
static u32 next_start(const Analysis* a, u32 s, u32 i, u32 count) {
    const u8* starts = a->starts[s];
    for (++i; i < count && !starts[i]; ++i) {
    }
    return i;
}

// Whether `address` holds `stwu r1, -N(r1)`. No branch inside a function
// goes back to its prologue, so a branch to one is a tail call.
static bool is_prologue(const FunctionAnalysisInput* in, u32 address) {
    const s32 s = section_of(in, address);
    if (s < 0)
        return false;
    const PPCInst* inst = &in->insts[s][(address - in->sections[s].address) / 4u];
    return inst->op == PPC_OP_STWU && inst->rS == 1 && inst->rA == 1 && inst->simm < 0;
}

// Makes every branch that leaves its function, or enters a prologue, land on
// a start, until nothing changes.
static void close_starts(Analysis* a, const FunctionAnalysis* out) {
    const FunctionAnalysisInput* in = a->in;
    for (bool changed = true; changed;) {
        changed = false;
        for (u32 s = 0; s < in->section_count; s++) {
            const PPCInst* insts = in->insts[s];
            const u32 base = in->sections[s].address, count = in->sections[s].size / 4u;
            for (u32 first = 0; first < count;) {
                const u32 end = next_start(a, s, first, count);
                const u32 lo = base + first * 4u, hi = base + end * 4u;
                for (u32 i = first; i < end; i++) {
                    const PPCInst* inst = &insts[i];
                    if ((inst->op == PPC_OP_B || inst->op == PPC_OP_BC) && !inst->lk &&
                        (inst->branch_target < lo || inst->branch_target >= hi ||
                         (inst->branch_target != lo && is_prologue(in, inst->branch_target))))
                        changed |= mark_start(a, inst->branch_target);
                    if (inst->op == PPC_OP_BCCTR && !inst->lk) {
                        const JumpTable* t = function_analysis_table(out, base + i * 4u);
                        for (u32 k = 0; t && k < t->count; k++) {
                            if (t->targets[k] < lo || t->targets[k] >= hi)
                                changed |= mark_start(a, t->targets[k]);
                        }
                    }
                }
                first = end;
            }
        }
    }
}

// --- Driver -------------------------------------------------------------------

int analyze_functions(FunctionAnalysis* out, const FunctionAnalysisInput* in) {
    memset(out, 0, sizeof(*out));
    Analysis a = {in, (u8**)calloc(in->section_count ? in->section_count : 1u, sizeof(u8*))};
    int ok = a.starts != NULL;
    for (u32 s = 0; ok && s < in->section_count; s++)
        ok = (a.starts[s] = (u8*)calloc(in->sections[s].size / 4u + 1u, 1u)) != NULL;

    // Jump tables first: their words are not code pointers.
    u32 capacity = 0;
    for (u32 s = 0; ok && s < in->section_count; s++) {
        const u32 count = in->sections[s].size / 4u;
        for (u32 i = 0; ok && i < count; i++) {
            const PPCInst* inst = &in->insts[s][i];
            JumpTable table;
            if (inst->op != PPC_OP_BCCTR || inst->lk || inst->bo != 20 ||
                !find_jump_table(in, s, i, &table))
                continue;
            if (out->table_count == capacity) {
                capacity = capacity ? capacity * 2u : 64u;
                JumpTable* grown = (JumpTable*)realloc(out->tables, capacity * sizeof(JumpTable));
                if (!grown) {
                    free(table.targets);
                    ok = 0;
                    break;
                }
                out->tables = grown;
            }
            out->tables[out->table_count++] = table;
        }
    }
    if (ok && out->table_count)
        qsort(out->tables, out->table_count, sizeof(JumpTable), compare_table);

    ok = ok && seed_starts(&a, out);
    if (ok)
        close_starts(&a, out);

    u32 total = 0;
    for (u32 s = 0; ok && s < in->section_count; s++) {
        for (u32 i = 0; i < in->sections[s].size / 4u; i++)
            total += a.starts[s][i];
    }
    if (ok)
        ok = (out->functions = (GuestFunction*)malloc((total ? total : 1u) * sizeof(GuestFunction))) != NULL;
    for (u32 s = 0; ok && s < in->section_count; s++) {
        const u32 base = in->sections[s].address, count = in->sections[s].size / 4u;
        for (u32 first = 0; first < count;) {
            const u32 end = next_start(&a, s, first, count);
            out->functions[out->function_count++] = (GuestFunction){base + first * 4u, base + end * 4u};
            first = end;
        }
    }
    // Sections are not in address order (RSO text follows the DOL's).
    if (ok && out->function_count) {
        for (u32 i = 1; i < out->function_count; i++) {
            GuestFunction f = out->functions[i];
            u32 j = i;
            for (; j && out->functions[j - 1u].start > f.start; j--)
                out->functions[j] = out->functions[j - 1u];
            out->functions[j] = f;
        }
    }

    for (u32 s = 0; a.starts && s < in->section_count; s++)
        free(a.starts[s]);
    free(a.starts);
    if (!ok)
        function_analysis_free(out);
    return ok;
}

void function_analysis_free(FunctionAnalysis* analysis) {
    for (u32 i = 0; i < analysis->table_count; i++)
        free(analysis->tables[i].targets);
    free(analysis->tables);
    free(analysis->functions);
    memset(analysis, 0, sizeof(*analysis));
}

const JumpTable* function_analysis_table(const FunctionAnalysis* analysis, u32 address) {
    u32 lo = 0, hi = analysis->table_count;
    while (lo < hi) {
        const u32 mid = lo + (hi - lo) / 2u;
        if (analysis->tables[mid].bctr < address)
            lo = mid + 1u;
        else
            hi = mid;
    }
    return lo < analysis->table_count && analysis->tables[lo].bctr == address ? &analysis->tables[lo]
                                                                              : NULL;
}

const GuestFunction* function_analysis_find(const FunctionAnalysis* analysis, u32 address) {
    u32 lo = 0, hi = analysis->function_count;
    while (lo < hi) {
        const u32 mid = lo + (hi - lo) / 2u;
        if (analysis->functions[mid].end <= address)
            lo = mid + 1u;
        else
            hi = mid;
    }
    return lo < analysis->function_count && analysis->functions[lo].start <= address
               ? &analysis->functions[lo]
               : NULL;
}
