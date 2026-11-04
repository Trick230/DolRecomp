// SPDX-License-Identifier: GPL-3.0-or-later
#include "reg_locals.h"

#include <stdlib.h>
#include <string.h>

// One line of a generated function is one node: the emitter writes at most a
// statement per line, and whatever else a line does (an `if`, a whole `goto`)
// shows in its text.

enum { REG_COUNT = 36, REG_LR = 32, REG_CTR = 33, REG_CR = 34, REG_XER = 35 };

typedef u64 RegSet;

// Whether locals or the context hold the registers at a point.
enum { HOLD_LOCALS = 1, HOLD_CONTEXT = 2 };

typedef struct {
    char* text;
    RegSet uses, defs, kills;
    u32 goto_count;
    u32* gotos;   // target lines
    s32 skip_to;  // the end of the conditional scope this line opens, or -1
    bool flush, reload;
    bool has_return, ends;  // ends: control never reaches the next line
    bool dynamic;           // a register the text picks at run time
    RegSet dirty_in;
    RegSet live_in, live_out;
    u8 hold_in;
    bool reached;
} Line;

typedef struct {
    Line* lines;
    u32 count;
    const char* name;
    RegSet promoted;  // the registers worth a local
} Func;

static const char* trim_start(const char* s) {
    while (*s == ' ' || *s == '\t')
        s++;
    return s;
}

static bool is_ident(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

static bool starts_word(const char* s, const char* word) {
    const size_t n = strlen(word);
    return strncmp(s, word, n) == 0 && !is_ident(s[n]);
}

// A register named at s ("ctx->..."): its index, the length of the name, and
// -1 if it isn't one of the registers. *dynamic: a gpr with a run-time index.
static int register_at(const char* s, size_t* length, bool* dynamic) {
    *dynamic = false;
    if (strncmp(s, "ctx->", 5) != 0)
        return -1;
    const char* name = s + 5;
    if (strncmp(name, "gpr[", 4) == 0) {
        const char* p = name + 4;
        int value = 0;
        while (*p >= '0' && *p <= '9')
            value = value * 10 + (*p++ - '0');
        if (p == name + 4 || *p != ']') {
            *dynamic = true;
            return -1;
        }
        *length = (size_t)(p + 1 - s);
        return value;
    }
    static const struct {
        const char* name;
        int reg;
    } special[] = {{"lr", REG_LR}, {"ctr", REG_CTR}, {"cr", REG_CR}, {"xer", REG_XER}};
    for (size_t i = 0; i < sizeof(special) / sizeof(special[0]); ++i) {
        if (starts_word(name, special[i].name)) {
            *length = 5u + strlen(special[i].name);
            return special[i].reg;
        }
    }
    return -1;
}

static void write_local(FILE* out, int reg) {
    switch (reg) {
    case REG_LR: fputs("reg_lr", out); break;
    case REG_CTR: fputs("reg_ctr", out); break;
    case REG_CR: fputs("reg_cr", out); break;
    case REG_XER: fputs("reg_xer", out); break;
    default: fprintf(out, "reg_r%d", reg); break;
    }
}

static void write_context(FILE* out, int reg) {
    switch (reg) {
    case REG_LR: fputs("ctx->lr", out); break;
    case REG_CTR: fputs("ctx->ctr", out); break;
    case REG_CR: fputs("ctx->cr", out); break;
    case REG_XER: fputs("ctx->xer", out); break;
    default: fprintf(out, "ctx->gpr[%d]", reg); break;
    }
}

// What the text after a register does to it: 0 reads, 1 assigns, 2 both.
static int access_after(const char* s) {
    s = trim_start(s);
    if (s[0] == '=' && s[1] != '=')
        return 1;
    if ((s[0] == '+' && s[1] == '+') || (s[0] == '-' && s[1] == '-'))
        return 2;
    if (strchr("+-*/%&|^", s[0]) && s[0] && s[1] == '=')
        return 2;
    if ((strncmp(s, "<<=", 3) == 0) || (strncmp(s, ">>=", 3) == 0))
        return 2;
    return 0;
}

static bool opens_conditional(const char* s) {
    if (s[0] == '}')
        s = trim_start(s + 1);
    return starts_word(s, "if") || starts_word(s, "else") || starts_word(s, "for") ||
           starts_word(s, "while") || starts_word(s, "switch") || starts_word(s, "case") ||
           starts_word(s, "default");
}

typedef struct {
    const char* name;
    size_t length;
    u32 line;
} LabelEntry;

static int compare_names(const char* a, size_t a_length, const char* b, size_t b_length) {
    const int order = strncmp(a, b, a_length < b_length ? a_length : b_length);
    if (order != 0)
        return order;
    return a_length < b_length ? -1 : a_length > b_length ? 1 : 0;
}

static int compare_labels(const void* a, const void* b) {
    const LabelEntry* x = (const LabelEntry*)a;
    const LabelEntry* y = (const LabelEntry*)b;
    return compare_names(x->name, x->length, y->name, y->length);
}

static s32 find_label(const LabelEntry* labels, u32 count, const char* name, size_t length) {
    u32 low = 0, high = count;
    while (low < high) {
        const u32 middle = low + (high - low) / 2u;
        const int order = compare_names(labels[middle].name, labels[middle].length, name, length);
        if (order == 0)
            return (s32)labels[middle].line;
        if (order < 0)
            low = middle + 1u;
        else
            high = middle;
    }
    return -1;
}

static bool analyze_syntax(Func* f) {
    // Labels: a line that is a name and a colon.
    LabelEntry* labels = (LabelEntry*)malloc(sizeof(LabelEntry) * (f->count + 1u));
    u32* scopes = (u32*)malloc(sizeof(u32) * (f->count + 2u));
    bool* scope_conditional = (bool*)malloc(sizeof(bool) * (f->count + 2u));
    if (!labels || !scopes || !scope_conditional)
        return false;
    u32 label_count = 0;
    for (u32 i = 0; i < f->count; ++i) {
        const char* s = f->lines[i].text;
        if (*s == ' ' || !is_ident(*s))
            continue;
        const char* p = s;
        while (is_ident(*p))
            p++;
        if (*p == ':') {
            labels[label_count].name = s;
            labels[label_count].length = (size_t)(p - s);
            labels[label_count].line = i;
            label_count++;
        }
    }
    qsort(labels, label_count, sizeof(LabelEntry), compare_labels);

    u32 depth = 0, conditional_depth = 0;
    bool pending_condition = false;
    for (u32 i = 0; i < f->count; ++i) {
        Line* line = &f->lines[i];
        const char* s = trim_start(line->text);
        line->skip_to = -1;
        if (strcmp(s, "/*@flush*/") == 0) {
            line->flush = true;
            continue;
        }
        if (strcmp(s, "/*@reload*/") == 0) {
            line->reload = true;
            continue;
        }
        if (s[0] == '/' && s[1] == '/')
            continue;

        const bool opener = opens_conditional(s);
        const bool conditional_here = pending_condition || opener || conditional_depth > 0;

        // Registers.
        for (const char* p = s; (p = strstr(p, "ctx->")) != NULL;) {
            size_t length = 0;
            bool dynamic = false;
            const int reg = register_at(p, &length, &dynamic);
            if (dynamic)
                line->dynamic = true;
            if (reg < 0) {
                p += 5;
                continue;
            }
            const RegSet bit = (RegSet)1 << reg;
            const int access = access_after(p + length);
            if (access != 1)
                line->uses |= bit;
            if (access != 0) {
                line->defs |= bit;
                if (access == 1 && !conditional_here)
                    line->kills |= bit;
            }
            p += length;
        }

        // Exits and jumps.
        u32 goto_count = 0;
        for (const char* p = s; (p = strstr(p, "goto ")) != NULL; p += 5)
            goto_count++;
        if (goto_count) {
            line->gotos = (u32*)malloc(sizeof(u32) * goto_count);
            if (!line->gotos)
                return false;
            for (const char* p = s; (p = strstr(p, "goto ")) != NULL; p += 5) {
                const char* name = p + 5;
                size_t length = 0;
                while (is_ident(name[length]))
                    length++;
                const s32 target = find_label(labels, label_count, name, length);
                if (target < 0) {
                    fprintf(stderr, "error: %s: no label %.*s\n", f->name, (int)length, name);
                    return false;
                }
                line->gotos[line->goto_count++] = (u32)target;
            }
        }
        line->has_return = strstr(s, "return;") != NULL;
        line->ends = !pending_condition && (strcmp(s, "return;") == 0 ||
                                            (strncmp(s, "goto ", 5) == 0 && goto_count == 1));

        // Scopes. A close of a conditional scope is reached from its opening,
        // whatever the scope did: `} else {` closes one and opens another.
        for (const char* p = s; *p; ++p) {
            if (*p == '{') {
                const bool conditional = opener || pending_condition;
                scopes[depth] = i;
                scope_conditional[depth] = conditional;
                depth++;
                if (conditional)
                    conditional_depth++;
                pending_condition = false;
            } else if (*p == '}' && depth > 0) {
                depth--;
                if (scope_conditional[depth]) {
                    conditional_depth--;
                    f->lines[scopes[depth]].skip_to = (s32)i;
                }
            }
        }

        // A condition or a statement over several lines goes on to the next.
        const size_t size = strlen(s);
        const char last = size ? s[size - 1] : ';';
        if (last == '{' || last == '}' || last == ';' || last == ':')
            pending_condition = false;
        else if (opener || pending_condition)
            pending_condition = true;
    }
    free(labels);
    free(scopes);
    free(scope_conditional);
    return true;
}

// Forward: which registers may be newer in their local than in the context,
// and who holds them.
static bool transfer_forward(const Line* line, RegSet dirty, u8 hold, RegSet* out_dirty, u8* out_hold) {
    (void)hold;
    if (line->flush) {
        *out_dirty = 0;
        *out_hold = HOLD_CONTEXT;
    } else if (line->reload) {
        *out_dirty = 0;
        *out_hold = HOLD_LOCALS;
    } else {
        *out_dirty = (hold & HOLD_LOCALS) ? dirty | line->defs : dirty;
        *out_hold = hold;
    }
    return true;
}

static void push(u32* stack, u32* size, bool* queued, u32 line) {
    if (!queued[line]) {
        queued[line] = true;
        stack[(*size)++] = line;
    }
}

static bool flow_forward(Func* f) {
    u32* stack = (u32*)malloc(sizeof(u32) * (f->count + 1u));
    bool* queued = (bool*)calloc(f->count + 1u, sizeof(bool));
    if (!stack || !queued)
        return false;
    u32 size = 0;
    if (f->count) {
        f->lines[0].reached = true;
        f->lines[0].hold_in = HOLD_CONTEXT;
        push(stack, &size, queued, 0);
    }
    while (size) {
        const u32 i = stack[--size];
        queued[i] = false;
        Line* line = &f->lines[i];
        RegSet dirty;
        u8 hold;
        transfer_forward(line, line->dirty_in, line->hold_in, &dirty, &hold);
        const u32 extra = line->goto_count + 2u;
        for (u32 e = 0; e < extra; ++e) {
            s64 next;
            if (e < line->goto_count)
                next = line->gotos[e];
            else if (e == line->goto_count)
                next = line->ends ? -1 : (s64)i + 1;
            else
                next = line->skip_to;
            if (next < 0 || next >= (s64)f->count)
                continue;
            Line* target = &f->lines[next];
            const RegSet merged = target->dirty_in | dirty;
            const u8 held = target->hold_in | hold;
            if (!target->reached || merged != target->dirty_in || held != target->hold_in) {
                target->reached = true;
                target->dirty_in = merged;
                target->hold_in = held;
                push(stack, &size, queued, (u32)next);
            }
        }
    }
    free(stack);
    free(queued);
    return true;
}

// What must be in the context at each exit from locals. Mixed holders are
// fine only where no local is newer.
static RegSet exit_set(const Line* line) {
    return (line->hold_in & HOLD_LOCALS) ? line->dirty_in : 0;
}

// Code that runs guest code or reads its registers: the context must hold them.
static bool enters_guest(const char* s) {
    return strstr(s, "dolrecomp_enter_indirect(") || strstr(s, "dolrecomp_safepoint(") ||
           strstr(s, "ppc_fallback_instruction(") || strstr(s, "func_") || strstr(s, "loop_");
}

static bool check_states(const Func* f) {
    for (u32 i = 0; i < f->count; ++i) {
        const Line* line = &f->lines[i];
        if (!line->reached)
            continue;
        const bool mixed = line->hold_in == (HOLD_LOCALS | HOLD_CONTEXT);
        const char* problem = NULL;
        if (line->reload && (line->hold_in & HOLD_LOCALS) && line->dirty_in)
            problem = "a reload would lose registers";
        else if (line->flush && mixed && line->dirty_in)
            problem = "a flush where the context may be newer";
        else if (line->has_return && mixed && line->dirty_in)
            problem = "an exit where the context may be newer";
        else if (mixed && (line->uses | line->defs))
            problem = "a register where the context may be newer";
        else if (line->dynamic && (line->hold_in & HOLD_LOCALS))
            problem = "a run-time register index with registers in locals";
        else if ((line->hold_in & HOLD_LOCALS) && enters_guest(line->text))
            problem = "guest code entered with registers in locals";
        if (problem) {
            fprintf(stderr, "error: %s: %s:\n  %s\n", f->name, problem, line->text);
            return false;
        }
    }
    return true;
}

static void flow_backward(Func* f) {
    bool changed = true;
    while (changed) {
        changed = false;
        for (u32 i = f->count; i-- > 0;) {
            Line* line = &f->lines[i];
            if (!line->reached)
                continue;
            RegSet out = 0;
            if (!line->ends && i + 1u < f->count)
                out |= f->lines[i + 1u].live_in;
            for (u32 e = 0; e < line->goto_count; ++e)
                out |= f->lines[line->gotos[e]].live_in;
            if (line->skip_to >= 0)
                out |= f->lines[line->skip_to].live_in;
            RegSet in;
            if (line->reload) {
                in = 0;
            } else if (line->flush) {
                in = (line->hold_in & HOLD_LOCALS) ? line->dirty_in : 0;
                out = 0;
            } else if (!(line->hold_in & HOLD_LOCALS)) {
                in = out;  // the context's stretch: nothing here reads a local
            } else {
                in = line->uses | (out & ~line->kills);
                if (line->has_return)
                    in |= exit_set(line);
            }
            if (out != line->live_out || in != line->live_in) {
                line->live_out = out;
                line->live_in = in;
                changed = true;
            }
        }
    }
}

// Whether a local pays for its loads and stores: a register stays in the context
// unless it is touched well more often than it has to move. Lines in a loop
// count for more; a conditional return is assumed cold.
static void choose_registers(Func* f, bool all_registers) {
    s32* delta = (s32*)calloc(f->count + 2u, sizeof(s32));
    u64 benefit[REG_COUNT] = {0}, cost[REG_COUNT] = {0};
    if (!delta) {
        f->promoted = 0;
        return;
    }
    for (u32 i = 0; i < f->count; ++i) {
        for (u32 e = 0; e < f->lines[i].goto_count; ++e) {
            if (f->lines[i].gotos[e] <= i) {
                delta[f->lines[i].gotos[e]]++;
                delta[i + 1u]--;
            }
        }
    }
    s32 depth = 0;
    for (u32 i = 0; i < f->count; ++i) {
        depth += delta[i];
        const Line* line = &f->lines[i];
        if (!line->reached)
            continue;
        const u64 weight = (u64)1 << (3 * (depth > 3 ? 3 : depth));
        const bool locals = (line->hold_in & HOLD_LOCALS) != 0;
        RegSet moved = 0;
        if (line->reload)
            moved = line->live_out;
        else if (line->flush || (line->has_return && line->ends))
            moved = exit_set(line);
        for (int reg = 0; reg < REG_COUNT; ++reg) {
            const RegSet bit = (RegSet)1 << reg;
            if (moved & bit)
                cost[reg] += weight;
            if (locals && !line->reload && !line->flush && ((line->uses | line->defs) & bit))
                benefit[reg] += weight;
        }
    }
    free(delta);
    f->promoted = 0;
    for (int reg = 0; reg < REG_COUNT; ++reg) {
        if (all_registers || (benefit[reg] >= 2u * cost[reg] && benefit[reg] != 0))
            f->promoted |= (RegSet)1 << reg;
    }
}

// The distinct sets of registers exits store, each with its own exit block.
typedef struct {
    RegSet* sets;
    u32 count, capacity;
} ExitSets;

static s32 exit_index(ExitSets* exits, RegSet set) {
    for (u32 i = 0; i < exits->count; ++i) {
        if (exits->sets[i] == set)
            return (s32)i;
    }
    if (exits->count == exits->capacity) {
        const u32 capacity = exits->capacity ? exits->capacity * 2u : 16u;
        RegSet* sets = (RegSet*)realloc(exits->sets, sizeof(RegSet) * capacity);
        if (!sets)
            return -1;
        exits->sets = sets;
        exits->capacity = capacity;
    }
    exits->sets[exits->count] = set;
    return (s32)exits->count++;
}

// The line, with its registers named for who holds them.
static void write_registers(FILE* out, const char* text, bool locals, RegSet promoted) {
    if (!locals) {
        fputs(text, out);
        return;
    }
    for (const char* p = text; *p;) {
        size_t length = 0;
        bool dynamic = false;
        const int reg = *p == 'c' ? register_at(p, &length, &dynamic) : -1;
        if (reg >= 0 && (promoted >> reg & 1u)) {
            write_local(out, reg);
            p += length;
        } else {
            fputc(*p++, out);
        }
    }
}

static void write_stores(FILE* out, RegSet set) {
    for (int reg = 0; reg < REG_COUNT; ++reg) {
        if (set >> reg & 1u) {
            fputs("    ", out);
            write_context(out, reg);
            fputs(" = ", out);
            write_local(out, reg);
            fputs(";\n", out);
        }
    }
}

static void write_loads(FILE* out, RegSet set) {
    for (int reg = 0; reg < REG_COUNT; ++reg) {
        if (set >> reg & 1u) {
            fputs("    ", out);
            write_local(out, reg);
            fputs(" = ", out);
            write_context(out, reg);
            fputs(";\n", out);
        }
    }
}

static bool write_function(FILE* out, const Func* f, const char* header) {
    RegSet used = 0;
    for (u32 i = 0; i < f->count; ++i) {
        const Line* line = &f->lines[i];
        if (line->reached && (line->hold_in & HOLD_LOCALS))
            used |= line->uses | line->defs;
    }
    used &= f->promoted;
    fputs(header, out);
    fputc('\n', out);
    for (int reg = 0; reg < REG_COUNT; ++reg) {
        if (used >> reg & 1u) {
            fputs("    u32 ", out);
            write_local(out, reg);
            fputs(";\n", out);
        }
    }

    ExitSets exits = {NULL, 0, 0};
    for (u32 i = 0; i < f->count; ++i) {
        const Line* line = &f->lines[i];
        if (line->flush) {
            if (line->reached)
                write_stores(out, exit_set(line) & f->promoted);
            continue;
        }
        if (line->reload) {
            if (line->reached)
                write_loads(out, line->live_out & f->promoted);
            continue;
        }
        // Lines nothing reaches stay out of the data flow; their registers
        // stay the context's, and they exit as written.
        const bool locals = line->reached && (line->hold_in & HOLD_LOCALS);
        const RegSet stores = line->reached ? exit_set(line) & f->promoted : 0;
        const char* text = line->text;
        if (line->has_return && stores) {
            const s32 index = exit_index(&exits, stores);
            if (index < 0)
                return false;
            const char* at = strstr(text, "return;");
            const size_t before = (size_t)(at - text);
            char* head = (char*)malloc(before + 1u);
            if (!head)
                return false;
            memcpy(head, text, before);
            head[before] = '\0';
            write_registers(out, head, locals, f->promoted);
            free(head);
            fprintf(out, "goto reg_exit_%d;", (int)index);
            write_registers(out, at + 7, locals, f->promoted);
        } else {
            write_registers(out, text, locals, f->promoted);
        }
        fputc('\n', out);
    }
    if (exits.count) {
        fputs("    return;\n", out);
        for (u32 i = 0; i < exits.count; ++i) {
            fprintf(out, "reg_exit_%u:\n", i);
            write_stores(out, exits.sets[i]);
            fputs("    return;\n", out);
        }
    }
    free(exits.sets);
    fputs("}\n", out);
    return true;
}

static bool rewrite_function(FILE* out, char* header, char** lines, u32 count, bool all_registers) {
    Func f = {NULL, count, header, 0};
    f.lines = (Line*)calloc(count ? count : 1u, sizeof(Line));
    if (!f.lines)
        return false;
    for (u32 i = 0; i < count; ++i)
        f.lines[i].text = lines[i];
    bool ok = analyze_syntax(&f) && flow_forward(&f) && check_states(&f);
    if (ok) {
        flow_backward(&f);
        choose_registers(&f, all_registers);
        ok = write_function(out, &f, header);
    }
    for (u32 i = 0; i < count; ++i)
        free(f.lines[i].gotos);
    free(f.lines);
    return ok;
}

bool reg_locals_rewrite(FILE* out, char* text, bool all_registers) {
    char** lines = NULL;
    u32 count = 0, capacity = 0;
    char* header = NULL;
    for (char* line = text; line && *line;) {
        char* end = strchr(line, '\n');
        if (end) {
            *end = '\0';
            if (end > line && end[-1] == '\r')
                end[-1] = '\0';
        }
        char* next = end ? end + 1 : NULL;
        if (!header) {
            const size_t size = strlen(line);
            if (size && line[0] != ' ' && line[size - 1] == '{' && strstr(line, "(CPUState* restrict ctx)")) {
                header = line;
                count = 0;
            } else {
                fputs(line, out);
                fputc('\n', out);
            }
        } else if (strcmp(line, "}") == 0) {
            if (!rewrite_function(out, header, lines, count, all_registers)) {
                free(lines);
                return false;
            }
            header = NULL;
        } else {
            if (count == capacity) {
                const u32 grown = capacity ? capacity * 2u : 256u;
                char** bigger = (char**)realloc(lines, sizeof(char*) * grown);
                if (!bigger) {
                    free(lines);
                    return false;
                }
                lines = bigger;
                capacity = grown;
            }
            lines[count++] = line;
        }
        line = next;
    }
    free(lines);
    return header == NULL;
}
