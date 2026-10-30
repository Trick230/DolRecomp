#include "app/recompile.h"

#include "analysis/code_section.h"
#include "analysis/embedded_data.h"
#include "analysis/functions.h"
#include "app/natives.h"
#include "app/module_link.h"
#include "app/sigdb.h"
#include "backend/codegen.h"
#include "backend/emitter.h"
#include "frontend/container/dol.h"
#include "frontend/decoder.h"
#include "platform/fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

#define CHUNK_INSTRUCTIONS 4096u
#define HEADER_NAME "ppc_recomp.h"

static u32 host_threads(void) {
#ifdef _WIN32
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    return info.dwNumberOfProcessors ? (u32)info.dwNumberOfProcessors : 1u;
#else
    const long count = sysconf(_SC_NPROCESSORS_ONLN);
    return count > 0 ? (u32)count : 1u;
#endif
}

static int compare_u32(const void* left, const void* right) {
    const u32 a = *(const u32*)left, b = *(const u32*)right;
    return a < b ? -1 : a > b;
}

static void add_entry_target(u32* targets, u32* count, const LoadedCodeSection* sections,
                             u32 section_count, u32 address) {
    for (u32 i = 0; i < section_count; i++) {
        if (address - sections[i].address < sections[i].size && (address & 3u) == 0) {
            targets[(*count)++] = address;
            return;
        }
    }
}

// Every address code can enter a function at from outside it, for the sparse
// entry switches: the program entry, every direct branch target, and every
// data word that points at an instruction (vtables, jump tables, callbacks).
// Code pointers built in registers that none of these cover are found at run
// time by the runtime stepping the instruction.
static u32* collect_entry_targets(u32 entry_point, const GuestRegion* data, u32 data_count,
                                  const LoadedCodeSection* sections, u32 section_count,
                                  u32* result_count) {
    size_t capacity = 1u;
    for (u32 i = 0; i < section_count; i++)
        capacity += sections[i].size / 4u;
    for (u32 i = 0; i < data_count; i++)
        capacity += data[i].size / 4u;
    u32* targets = (u32*)malloc(capacity * sizeof(*targets));
    if (!targets)
        return NULL;
    u32 count = 0;
    add_entry_target(targets, &count, sections, section_count, entry_point);
    for (u32 s = 0; s < section_count; s++) {
        const LoadedCodeSection* section = &sections[s];
        for (u32 offset = 0; offset + 4u <= section->size; offset += 4u) {
            // Text words are not scanned as pointers: every lwz r0-r3 reads as one.
            const PPCInst inst = ppc_decode(read_be32(section->data + offset), section->address + offset);
            if (inst.op == PPC_OP_B || inst.op == PPC_OP_BC)
                add_entry_target(targets, &count, sections, section_count, inst.branch_target);
        }
    }
    for (u32 i = 0; i < data_count; i++) {
        for (u32 offset = 0; data[i].bytes && offset + 4u <= data[i].size; offset += 4u)
            add_entry_target(targets, &count, sections, section_count, read_be32(data[i].bytes + offset));
    }
    qsort(targets, count, sizeof(*targets), compare_u32);
    u32 unique = 0;
    for (u32 i = 0; i < count; i++) {
        if (!unique || targets[i] != targets[unique - 1u])
            targets[unique++] = targets[i];
    }
    *result_count = unique;
    return targets;
}

static u64 fnv1a64(const u8* data, u32 size) {
    u64 hash = 0xCBF29CE484222325ull;
    for (u32 i = 0; i < size; i++) {
        hash ^= data[i];
        hash *= 0x100000001B3ull;
    }
    return hash;
}

static const u8* section_bytes(const LoadedCodeSection* sections, u32 count, u32 start, u32 end) {
    for (u32 i = 0; i < count; i++) {
        if (start - sections[i].address < sections[i].size && end - sections[i].address <= sections[i].size)
            return sections[i].data + (start - sections[i].address);
    }
    return NULL;
}

#define MAX_GROUP_SECTIONS (DOL_NUM_TEXT + 64)

// One group's decoded text and the functions found in it: the resident code
// (the DOL and the modules always loaded) or one overlay.
typedef struct {
    char tag[64];      // what its C names carry: "" for the resident group
    const char* name;  // an overlay's config file name
    LoadedCodeSection sections[MAX_GROUP_SECTIONS];
    u32 section_count;
    PPCInst* insts[MAX_GROUP_SECTIONS];
    FunctionAnalysis analysis;
    u32* entry_targets;
    u32 entry_target_count;
    u32 first_id;  // its first function's index in the runtime's table
    // An overlay's: where it was linked, and the words of its code that
    // depend on where it loads, by address (see app/module_link.h).
    LinkedOverlay where;
    const LinkedReloc* relocs;
    u32 reloc_count;
    EmitHaReloc* ha_relocs;
    u32 ha_reloc_count;
} Module;

static void module_free(Module* module) {
    for (u32 s = 0; s < module->section_count; s++)
        free(module->insts[s]);
    function_analysis_free(&module->analysis);
    free(module->entry_targets);
    free(module->ha_relocs);
}

// FNV-1a 64 of a function's text with the bits its placement changes cleared;
// the runtime hashes guest RAM the same way.
static u64 function_hash(const Module* module, const GuestFunction* f) {
    const u8* bytes = section_bytes(module->sections, module->section_count, f->start, f->end);
    if (!bytes)
        return 0;
    if (!module->reloc_count)
        return fnv1a64(bytes, f->end - f->start);
    u64 hash = 0xCBF29CE484222325ull;
    const LinkedReloc* reloc = module->relocs;
    const LinkedReloc* relocs_end = module->relocs + module->reloc_count;
    while (reloc != relocs_end && reloc->address < f->start)
        reloc++;
    for (u32 address = f->start; address < f->end; address += 4u) {
        u32 word = read_be32(bytes + (address - f->start));
        for (; reloc != relocs_end && reloc->address == address; reloc++)
            word &= reloc->keep_mask;
        for (int shift = 24; shift >= 0; shift -= 8) {
            hash ^= (u8)(word >> shift);
            hash *= 0x100000001B3ull;
        }
    }
    return hash;
}

static const LoadedCodeSection* s_sort_sections;

static int compare_section_index(const void* left, const void* right) {
    const u32 a = s_sort_sections[*(const u32*)left].address;
    const u32 b = s_sort_sections[*(const u32*)right].address;
    return a < b ? -1 : a > b;
}

// A group's sections by address, for its code ranges.
static void sort_sections(const Module* module, u32* order) {
    for (u32 i = 0; i < module->section_count; i++)
        order[i] = i;
    s_sort_sections = module->sections;
    qsort(order, module->section_count, sizeof(u32), compare_section_index);
}

// The runtime's view of the code: what the dispatcher covers, and per
// function its range, its code and the hash of the text it was built from.
// The resident group's functions come first, then each overlay's, which also
// lists its own code ranges.
static int write_tables(const char* path, const Module* groups, u32 group_count) {
    OutputFile output;
    if (!output_open(&output, path, "w"))
        return 0;
    FILE* out = output.stream;
    u32 order[MAX_GROUP_SECTIONS];
    fprintf(out, "// Generated by DolRecomp.\n");
    fprintf(out, "static const GuestRange s_code_ranges[] = {\n");
    sort_sections(&groups[0], order);
    for (u32 i = 0; i < groups[0].section_count; i++) {
        const LoadedCodeSection* section = &groups[0].sections[order[i]];
        fprintf(out, "    {0x%08Xu, 0x%08Xu},\n", section->address, section->address + section->size);
    }
    fprintf(out, "};\n");
    fprintf(out, "static const GuestRange s_function_ranges[] = {\n");
    for (u32 g = 0; g < group_count; g++) {
        const FunctionAnalysis* analysis = &groups[g].analysis;
        for (u32 i = 0; i < analysis->function_count; i++)
            fprintf(out, "    {0x%08Xu, 0x%08Xu},\n", analysis->functions[i].start, analysis->functions[i].end);
    }
    fprintf(out, "};\n");
    fprintf(out, "static const GuestCode s_functions[] = {\n");
    for (u32 g = 0; g < group_count; g++) {
        const FunctionAnalysis* analysis = &groups[g].analysis;
        for (u32 i = 0; i < analysis->function_count; i++)
            fprintf(out, "    func_%s%08X,\n", groups[g].tag, analysis->functions[i].start);
    }
    fprintf(out, "};\n");
    fprintf(out, "static const u64 s_function_hashes[] = {\n");
    for (u32 g = 0; g < group_count; g++) {
        const Module* module = &groups[g];
        for (u32 i = 0; i < module->analysis.function_count; i++)
            fprintf(out, "    0x%016llXull,\n",
                    (unsigned long long)function_hash(module, &module->analysis.functions[i]));
    }
    fprintf(out, "};\n");
    fprintf(out, "#define DOLRECOMP_OVERLAY_COUNT %uu\n", group_count - 1u);
    if (group_count > 1) {
        fprintf(out, "static const GuestRange s_overlay_code_ranges[] = {\n");
        for (u32 g = 1; g < group_count; g++) {
            sort_sections(&groups[g], order);
            for (u32 i = 0; i < groups[g].section_count; i++) {
                const LoadedCodeSection* section = &groups[g].sections[order[i]];
                fprintf(out, "    {0x%08Xu, 0x%08Xu},\n", section->address, section->address + section->size);
            }
        }
        fprintf(out, "};\n");
        fprintf(out, "static const GuestReloc s_overlay_relocs[] = {\n");
        for (u32 g = 1; g < group_count; g++) {
            for (u32 i = 0; i < groups[g].reloc_count; i++)
                fprintf(out, "    {0x%08Xu, 0x%08Xu},\n", groups[g].relocs[i].address,
                        groups[g].relocs[i].keep_mask);
        }
        fprintf(out, "    {0, 0},\n");  // never empty
        fprintf(out, "};\n");
        fprintf(out, "static const GuestOverlay s_overlays[] = {\n");
        u32 ranges = 0, relocs = 0;
        for (u32 g = 1; g < group_count; g++) {
            const Module* module = &groups[g];
            fprintf(out, "    {\"%s\", s_overlay_code_ranges + %u, %u, %u, %u, 0x%08Xu, 0x%08Xu, %uu, "
                         "s_overlay_relocs + %u, %u},\n",
                    module->name, ranges, module->section_count, module->first_id,
                    module->analysis.function_count, module->where.base, module->where.bss,
                    module->where.bss_section, relocs, module->reloc_count);
            ranges += module->section_count;
            relocs += module->reloc_count;
        }
        fprintf(out, "};\n");
    }
    return output_close(&output);
}

// Fixed-count sources that include the chunks round-robin, so the build can
// name its sources before the recompiler has run. A shard whose chunk changed
// is rewritten even when its own text is the same: the build only checks the
// chunks a shard includes when the shard itself is newer.
static int write_shards(const char* out_dir, u32 shards, char (*chunk_names)[128], const int* replaced,
                        u32 chunk_count) {
    for (u32 shard = 0; shard < shards; shard++) {
        char name[64], path[1200];
        snprintf(name, sizeof(name), "ppc_recomp.%u.c", shard);
        if (!join_path(path, sizeof(path), out_dir, name))
            return 0;
        OutputFile output;
        if (!output_open(&output, path, "w"))
            return 0;
        fprintf(output.stream, "// Generated by DolRecomp.\n#include \"" HEADER_NAME "\"\n");
        for (u32 chunk = shard; chunk < chunk_count; chunk += shards) {
            fprintf(output.stream, "#include \"chunks/%s\"\n", chunk_names[chunk]);
            output.force |= replaced[chunk];
        }
        if (!output_close(&output))
            return 0;
    }
    return 1;
}

static int overlaps_group(const Module* group, u32 start, u32 end) {
    for (u32 s = 0; s < group->section_count; s++) {
        const LoadedCodeSection* section = &group->sections[s];
        if (start < section->address + section->size && section->address < end)
            return 1;
    }
    return 0;
}

// The functions a group's code calls directly: its own, and for an overlay
// the resident ones it does not lie over, merged by address.
typedef struct {
    u32* starts;
    u32* ends;
    u32* ids;
    const char** tags;
    u32 count;
} CallTable;

static int build_call_table(CallTable* table, const Module* groups, u32 group) {
    const FunctionAnalysis* resident = &groups[0].analysis;
    const FunctionAnalysis* own = &groups[group].analysis;
    const u32 capacity = resident->function_count + (group ? own->function_count : 0) + 1u;
    table->starts = (u32*)malloc(capacity * sizeof(u32));
    table->ends = (u32*)malloc(capacity * sizeof(u32));
    table->ids = (u32*)malloc(capacity * sizeof(u32));
    table->tags = (const char**)malloc(capacity * sizeof(const char*));
    table->count = 0;
    if (!table->starts || !table->ends || !table->ids || !table->tags)
        return 0;
    u32 r = 0, o = 0;
    const u32 own_count = group ? own->function_count : 0;
    while (r < resident->function_count || o < own_count) {
        const GuestFunction* rf = r < resident->function_count ? &resident->functions[r] : NULL;
        const GuestFunction* of = o < own_count ? &own->functions[o] : NULL;
        u32 n = table->count;
        if (rf && (!of || rf->start < of->start)) {
            r++;
            if (group && overlaps_group(&groups[group], rf->start, rf->end))
                continue;
            table->starts[n] = rf->start;
            table->ends[n] = rf->end;
            table->ids[n] = r - 1u;
            table->tags[n] = NULL;
        } else {
            table->starts[n] = of->start;
            table->ends[n] = of->end;
            table->ids[n] = groups[group].first_id + o;
            table->tags[n] = groups[group].tag;
            o++;
        }
        table->count++;
    }
    return 1;
}

static void call_table_free(CallTable* table) {
    free(table->starts);
    free(table->ends);
    free(table->ids);
    free(table->tags);
}

// One C function per guest function, about CHUNK_INSTRUCTIONS of them to a
// source. Every function of a group is known before any is written, so a call
// between two of them, or from an overlay to resident code, is a direct C
// call.
static int emit_groups(const Module* groups, u32 group_count, const char* out_dir, u32 entry_point,
                       u32 shards) {
    char header_path[1200], chunks_dir[1200], tables_path[1200];
    if (!join_path(header_path, sizeof(header_path), out_dir, HEADER_NAME) ||
        !join_path(chunks_dir, sizeof(chunks_dir), out_dir, "chunks") ||
        !join_path(tables_path, sizeof(tables_path), out_dir, "ppc_tables.inc") ||
        !make_dir_tree(chunks_dir)) {
        fprintf(stderr, "error: output path is too long or can't be created\n");
        return 0;
    }
    u32 function_count = 0;
    for (u32 g = 0; g < group_count; g++)
        function_count += groups[g].analysis.function_count;
    const size_t slots = function_count ? function_count : 1u;
    ChunkJob* jobs = (ChunkJob*)calloc(slots, sizeof(ChunkJob));
    char (*chunk_names)[128] = (char (*)[128])malloc(slots * 128u);
    int* replaced = (int*)calloc(slots, sizeof(int));
    OutputFile header_output;
    int ok = jobs && chunk_names && replaced && output_open(&header_output, header_path, "w");
    if (!ok) {
        free(jobs);
        free(chunk_names);
        free(replaced);
        return 0;
    }
    FILE* header = header_output.stream;
    emit_header_for_cpu(header, DOLRECOMP_CPU_BROADWAY);
    fprintf(header, "\n#define DOLRECOMP_ENTRY_POINT 0x%08Xu\n", entry_point);
    if (group_count > 1) {
        fprintf(header, "\n/* How far each overlay, and its bss, loaded from where it was compiled\n"
                        "   for; the runtime's. */\n");
        fprintf(header, "extern u32 dolrecomp_overlay_shift[];\nextern u32 dolrecomp_overlay_bss_shift[];\n");
    }
    fprintf(header, "\n// Function entry points\n");
    for (u32 g = 0; g < group_count; g++) {
        for (u32 i = 0; i < groups[g].analysis.function_count; i++)
            fprintf(header, "void func_%s%08X(CPUState* ctx);\n", groups[g].tag,
                    groups[g].analysis.functions[i].start);
    }

    const u32 threads = host_threads();
    u32 job_count = 0;
    for (u32 g = 0; ok && g < group_count; g++) {
        const Module* module = &groups[g];
        const FunctionAnalysis* analysis = &module->analysis;
        CallTable calls = {0};
        ok = build_call_table(&calls, groups, g);
        u32 order[MAX_GROUP_SECTIONS], starts[MAX_GROUP_SECTIONS], ends[MAX_GROUP_SECTIONS];
        if (ok) {
            emit_set_function_table(calls.starts, calls.ends, g ? calls.ids : NULL,
                                    g ? calls.tags : NULL, calls.count);
            emit_set_symbol_tag(module->tag);
            emit_set_entry_targets(module->entry_targets, module->entry_target_count);
            sort_sections(module, order);
            for (u32 s = 0; s < module->section_count; s++) {
                starts[s] = module->sections[order[s]].address;
                ends[s] = starts[s] + module->sections[order[s]].size;
            }
            if (g)
                emit_set_overlay(g, starts, ends, module->section_count, module->ha_relocs,
                                 module->ha_reloc_count);
        }
        const u32 first_job = job_count;
        for (u32 s = 0; ok && s < module->section_count; s++) {
            const LoadedCodeSection* section = &module->sections[s];
            const u32 section_end = section->address + section->size;
            for (u32 f = 0; ok && f < analysis->function_count;) {
                const GuestFunction* first = &analysis->functions[f];
                if (first->start < section->address || first->start >= section_end) {
                    f++;
                    continue;
                }
                u32 last = f + 1;
                while (last < analysis->function_count && analysis->functions[last].start < section_end &&
                       analysis->functions[last].end - first->start <= CHUNK_INSTRUCTIONS * 4u)
                    last++;
                ChunkJob* job = &jobs[job_count];
                job->insts = module->insts[s] + (first->start - section->address) / 4u;
                job->functions = first;
                job->function_count = last - f;
                snprintf(chunk_names[job_count], 128, "chunk_%04u_%s%s%u_%08X.c", job_count, module->tag,
                         section->label, section->index, first->start);
                snprintf(job->include_name, sizeof(job->include_name), HEADER_NAME);
                job->replaced = &replaced[job_count];
                ok = join_path(job->path, sizeof(job->path), chunks_dir, chunk_names[job_count]) != 0;
                job_count++;
                f = last;
            }
        }
        if (ok) {
            const u32 group_jobs = job_count - first_job;
            printf("writing %u %sfunctions in %u chunks with %u jobs\n", analysis->function_count,
                   g ? "overlay " : "", group_jobs, effective_chunk_jobs(group_jobs, threads));
            ok = run_chunk_jobs(jobs + first_job, group_jobs, threads);
        }
        emit_set_function_table(NULL, NULL, NULL, NULL, 0);
        emit_set_symbol_tag(NULL);
        emit_set_overlay(0, NULL, NULL, 0, NULL, 0);
        call_table_free(&calls);
    }
    emit_footer(header);

    ok = ok && write_tables(tables_path, groups, group_count) &&
         write_shards(out_dir, shards, chunk_names, replaced, job_count);
    if (ok) {
        printf("done: %u chunks in %u shards\n", job_count, shards);
        ok = output_close(&header_output);
    } else {
        output_discard(&header_output);
    }
    free(jobs);
    free(chunk_names);
    free(replaced);
    return ok;
}

// Decodes a group's sections and finds its functions (see analysis/functions.h
// for where their starts come from).
static int load_module(Module* module, const GuestRegion* data, u32 data_count, const u32* seeds,
                       u32 seed_count) {
    int ok = 1;
    for (u32 s = 0; ok && s < module->section_count; s++) {
        const LoadedCodeSection* section = &module->sections[s];
        const u32 count = section->size / 4u;
        printf("decoding %s%s[%u]: %u instructions at 0x%08X\n", module->tag, section->label, section->index,
               count, section->address);
        PPCInst* insts = (PPCInst*)malloc((count ? count : 1u) * sizeof(PPCInst));
        module->insts[s] = insts;
        ok = insts != NULL;
        for (u32 i = 0; ok && i < count; i++) {
            const u32 raw = read_be32(section->data + i * 4u);
            insts[i] = ppc_decode(raw, section->address + i * 4u);
            if (insts[i].op == PPC_OP_UNKNOWN && embedded_data_word(section->embedded_data_mode, raw))
                insts[i].embedded_data = true;
        }
    }
    FunctionAnalysisInput input = {module->sections, (const PPCInst* const*)module->insts,
                                   module->section_count, data, data_count, seeds, seed_count};
    ok = ok && analyze_functions(&module->analysis, &input);
    if (!ok)
        return 0;
    const FunctionAnalysis* analysis = &module->analysis;
    u32 entries = 0;
    for (u32 i = 0; i < analysis->table_count; i++)
        entries += analysis->tables[i].count;
    printf("functions: %u, jump tables: %u (%u cases)\n", analysis->function_count,
           analysis->table_count, entries);
    return 1;
}

// For comparing against symbol maps: the resident functions, then each
// overlay's with its tag.
static int write_function_list(const Module* groups, u32 group_count, const char* out_dir) {
    char path[1200];
    FILE* out = make_dir_tree(out_dir) && join_path(path, sizeof(path), out_dir, "functions.txt")
                    ? fopen(path, "w") : NULL;
    if (!out)
        return 0;
    for (u32 g = 0; g < group_count; g++) {
        const FunctionAnalysis* analysis = &groups[g].analysis;
        for (u32 i = 0; i < analysis->function_count; i++) {
            fprintf(out, "%08X %08X%s%s\n", analysis->functions[i].start,
                    analysis->functions[i].end - analysis->functions[i].start, g ? " " : "",
                    groups[g].name ? groups[g].name : "");
        }
    }
    return fclose(out) == 0;
}

static int compare_reloc(const void* left, const void* right) {
    const LinkedReloc* a = (const LinkedReloc*)left;
    const LinkedReloc* b = (const LinkedReloc*)right;
    if (a->group != b->group)
        return a->group < b->group ? -1 : 1;
    return a->address < b->address ? -1 : a->address > b->address;
}

// What the emitter needs of an overlay's relocations: its address-highs, each
// of which must be an addis for the shift to land in its register.
static int overlay_ha_relocs(Module* module) {
    module->ha_relocs = (EmitHaReloc*)malloc((module->reloc_count ? module->reloc_count : 1u) *
                                             sizeof(EmitHaReloc));
    if (!module->ha_relocs)
        return 0;
    for (u32 i = 0; i < module->reloc_count; i++) {
        const LinkedReloc* reloc = &module->relocs[i];
        if (reloc->kind == LINKED_RELOC_PLAIN)
            continue;
        const u8* word =
            section_bytes(module->sections, module->section_count, reloc->address, reloc->address + 4u);
        if (!word || read_be32(word) >> 26 != 15u || reloc->keep_mask != 0xFFFF0000u) {
            fprintf(stderr, "error: %s: address-high at 0x%08X is not an addis\n", module->name,
                    reloc->address);
            return 0;
        }
        module->ha_relocs[module->ha_reloc_count++] =
            (EmitHaReloc){reloc->address, reloc->kind == LINKED_RELOC_HA_BSS};
    }
    return 1;
}

// The C name tag of an overlay: its file name without the directory and
// extension, as an identifier, then '_'.
static void overlay_tag(char* tag, size_t size, const char* file) {
    const char* name = file;
    for (const char* p = file; *p; p++) {
        if (*p == '/' || *p == '\\')
            name = p + 1;
    }
    size_t n = 0;
    for (; name[n] && name[n] != '.' && n + 2 < size; n++) {
        const char c = name[n];
        tag[n] = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ? c : '_';
    }
    tag[n++] = '_';
    tag[n] = 0;
}

int recompile(const RecompConfig* config, const char* game_dir, const char* out_dir) {
    char dol_path[1200];
    DOLFile dol;
    if (!join_path(dol_path, sizeof(dol_path), game_dir, config->dol) || !dol_load(&dol, dol_path))
        return 0;

    const u32 group_count = 1u + config->rso_overlay_count;
    Module* groups = (Module*)calloc(group_count, sizeof(Module));
    ModuleLink link = {0};
    if (!groups || (config->sel[0] && !rso_link(&link, config, game_dir)) || !rel_link(&link, config, game_dir)) {
        free(groups);
        module_link_free(&link);
        dol_free(&dol);
        return 0;
    }

    // The resident group: the DOL's text, then the resident modules',
    // numbered on from its sections; each overlay's text in its own group.
    Module* resident = &groups[0];
    for (u32 i = 0; i < DOL_NUM_TEXT; i++) {
        const u8* data = dol.header.text_sizes[i] ? dol_get_text_section(&dol, (int)i) : NULL;
        if (!data)
            continue;
        resident->sections[resident->section_count++] = (LoadedCodeSection){
            "text", NULL, data, i, dol.header.text_offsets[i], dol.header.text_addresses[i],
            dol.header.text_sizes[i], EMBEDDED_DATA_DOL};
    }
    int ok = 1;
    for (u32 g = 1; g < group_count; g++) {
        groups[g].name = config->rso_overlays[g - 1u].file;
        overlay_tag(groups[g].tag, sizeof(groups[g].tag), groups[g].name);
    }
    for (u32 i = 0; ok && i < link.count; i++) {
        Module* module = &groups[link.text[i].group];
        if (module->section_count == MAX_GROUP_SECTIONS) {
            fprintf(stderr, "error: too many module text sections\n");
            ok = 0;
            break;
        }
        module->sections[module->section_count] = (LoadedCodeSection){
            "text", link.text[i].module, link.text[i].bytes, module->section_count, 0,
            link.text[i].address, link.text[i].size, EMBEDDED_DATA_DOL};
        module->section_count++;
    }

    if (link.reloc_count)
        qsort(link.relocs, link.reloc_count, sizeof(LinkedReloc), compare_reloc);
    for (u32 i = 0; ok && i < link.reloc_count;) {
        Module* module = &groups[link.relocs[i].group];
        u32 end = i;
        while (end < link.reloc_count && link.relocs[end].group == link.relocs[i].group)
            end++;
        module->relocs = link.relocs + i;
        module->reloc_count = end - i;
        ok = overlay_ha_relocs(module);
        i = end;
    }
    for (u32 g = 1; g < group_count; g++)
        groups[g].where = link.overlays[g - 1u];

    DolRecompNativeList natives = {0};
    NativeSignatures signatures = {0};
    GuestRegion* data = NULL;
    u32* seeds = NULL;
    for (u32 i = 0; ok && i < config->natives_count; i++)
        ok = natives_load(&signatures, config->natives[i]);
    ok = ok && natives_resolve(&natives, &signatures, resident->sections, resident->section_count);
    if (ok && signatures.count)
        printf("natives: %u guest functions with host implementations, of %u signatures\n",
               natives.count, signatures.count);
    for (u32 i = 0; ok && i < config->define_count; i++)
        ok = natives_add_define(&natives, config->defines[i].name, config->defines[i].value);
    emit_set_natives(natives.entries, natives.count);
    char natives_header[1200];
    char natives_list[1200];
    ok = ok && join_path(natives_header, sizeof(natives_header), out_dir, "ppc_natives.h") &&
         join_path(natives_list, sizeof(natives_list), out_dir, "natives.txt") && make_dir_tree(out_dir) &&
         natives_write_header(&natives, natives_header) && natives_write_list(&natives, natives_list);
    if (ok) {
        data = (GuestRegion*)malloc((DOL_NUM_DATA + link.data_count + 1u) * sizeof(GuestRegion));
        seeds = (u32*)malloc((1u + link.entry_count + natives.count) * sizeof(u32));
        ok = data && seeds;
    }

    u32 first_id = 0;
    for (u32 g = 0; ok && g < group_count; g++) {
        Module* module = &groups[g];
        // Code pointers live in the group's data: the DOL's for the resident
        // group, with the resident modules', and each overlay's own.
        u32 data_count = 0, seed_count = 0;
        if (g == 0) {
            for (int i = 0; i < DOL_NUM_DATA; i++) {
                if (dol.header.data_sizes[i])
                    data[data_count++] = (GuestRegion){dol.header.data_addresses[i], dol.header.data_sizes[i],
                                                       dol_get_data_section(&dol, i)};
            }
            seeds[seed_count++] = dol.header.entry_point;
        }
        const u32 dol_data = data_count;
        for (u32 i = 0; i < link.data_count; i++) {
            if (link.data[i].group == g)
                data[data_count++] = (GuestRegion){link.data[i].address, link.data[i].size, link.data[i].bytes};
        }
        for (u32 i = 0; i < link.entry_count; i++) {
            if (link.entries[i].group == g)
                seeds[seed_count++] = link.entries[i].address;
        }
        for (u32 i = 0; g == 0 && i < natives.count; i++)
            seeds[seed_count++] = natives.entries[i].address;
        ok = load_module(module, data, data_count, seeds, seed_count);
        // The resident entry targets come from the DOL's data alone.
        module->entry_targets =
            ok ? collect_entry_targets(g == 0 ? dol.header.entry_point : 0, g == 0 ? data : data + dol_data,
                                       g == 0 ? dol_data : data_count - dol_data, module->sections,
                                       module->section_count, &module->entry_target_count)
               : NULL;
        ok = ok && module->entry_targets;
        if (ok && g == 0)
            printf("entry targets: %u\n", module->entry_target_count);
        if (ok && g == 0 && config->sigdb[0]) {
            SigDB db;
            ok = sigdb_load(&db, config->sigdb) &&
                 sigdb_report(&db, &module->analysis, module->sections, module->section_count, &signatures,
                              &natives, out_dir);
            sigdb_free(&db);
        }
        module->first_id = first_id;
        first_id += module->analysis.function_count;
    }
    ok = ok && write_function_list(groups, group_count, out_dir) &&
         emit_groups(groups, group_count, out_dir, dol.header.entry_point, config->shards);

    emit_set_entry_targets(NULL, 0);
    emit_set_natives(NULL, 0);
    for (u32 g = 0; g < group_count; g++)
        module_free(&groups[g]);
    free(groups);
    free(data);
    free(seeds);
    natives_free(&natives);
    natives_signatures_free(&signatures);
    module_link_free(&link);
    dol_free(&dol);
    return ok;
}
