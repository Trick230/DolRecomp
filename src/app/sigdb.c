#include "app/sigdb.h"

#include "platform/fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SIGDB_RECORD_SIZE 136u

static u32 read_le32(const u8* p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static int compare_checksum(const void* left, const void* right) {
    const SigDBEntry* a = (const SigDBEntry*)left;
    const SigDBEntry* b = (const SigDBEntry*)right;
    return a->checksum < b->checksum ? -1 : a->checksum > b->checksum;
}

int sigdb_load(SigDB* db, const char* path) {
    memset(db, 0, sizeof(*db));
    FILE* file = fopen(path, "rb");
    if (!file) {
        fprintf(stderr, "error: can't open sigdb '%s'\n", path);
        return 0;
    }
    u8 header[4];
    const int has_count = fread(header, 1, 4, file) == 4;
    const u32 count = has_count ? read_le32(header) : 0;
    db->entries = count && count < (1u << 22) ? (SigDBEntry*)calloc(count, sizeof(SigDBEntry)) : NULL;
    int ok = db->entries != NULL;
    for (u32 i = 0; ok && i < count; i++) {
        u8 record[SIGDB_RECORD_SIZE];
        ok = fread(record, 1, sizeof(record), file) == sizeof(record);
        if (!ok)
            break;
        SigDBEntry* entry = &db->entries[i];
        entry->checksum = read_le32(record);
        entry->size = read_le32(record + 4);
        memcpy(entry->name, record + 8, sizeof(entry->name) - 1u);
    }
    fclose(file);
    if (!ok) {
        fprintf(stderr, "error: '%s' is not a signature database\n", path);
        sigdb_free(db);
        return 0;
    }
    db->count = count;
    qsort(db->entries, count, sizeof(SigDBEntry), compare_checksum);
    for (u32 i = 0; i + 1u < count; i++) {
        SigDBEntry* a = &db->entries[i];
        for (u32 j = i + 1u; j < count && db->entries[j].checksum == a->checksum; j++) {
            SigDBEntry* b = &db->entries[j];
            if (a->size != b->size || strcmp(a->name, b->name) != 0)
                a->ambiguous = b->ambiguous = 1;
        }
    }
    return 1;
}

void sigdb_free(SigDB* db) {
    free(db->entries);
    memset(db, 0, sizeof(*db));
}

u32 sigdb_checksum(const u8* code, u32 size) {
    u32 sum = 0;
    for (u32 offset = 0; offset + 4u <= size; offset += 4u) {
        const u32 word = ((u32)code[offset] << 24) | ((u32)code[offset + 1] << 16) |
                         ((u32)code[offset + 2] << 8) | code[offset + 3];
        const u32 primary = word >> 26;
        u32 op2 = 0, op3 = 0;
        switch (primary) {
        case 4:
            op2 = word & 0x3Fu;
            if (op2 == 0 || op2 == 8 || op2 == 16 || op2 == 21 || op2 == 22)
                op3 = word & 0x7C0u;
            break;
        case 7: case 8: case 10: case 11: case 12: case 13: case 14: case 15:
            op2 = word & 0x03FF0000u;
            break;
        case 19: case 31: case 63:
            op2 = word & 0x7FFu;
            break;
        case 59:
            op2 = word & 0x3Fu;
            if (op2 < 16)
                op3 = word & 0x7C0u;
            break;
        default:
            if (primary >= 32 && primary < 56)
                op2 = word & 0x03FF0000u;
            break;
        }
        sum = (sum << 17) | (sum >> 15);
        sum ^= (word & 0xFC000000u) | op2 | op3;
    }
    return sum;
}

const SigDBEntry* sigdb_find(const SigDB* db, u32 checksum, u32 size) {
    u32 lo = 0, hi = db->count;
    while (lo < hi) {
        const u32 mid = lo + (hi - lo) / 2u;
        if (db->entries[mid].checksum < checksum)
            lo = mid + 1u;
        else
            hi = mid;
    }
    if (lo == db->count || db->entries[lo].checksum != checksum)
        return NULL;
    const SigDBEntry* entry = &db->entries[lo];
    return entry->ambiguous || entry->size != size ? NULL : entry;
}

static const u8* code_at(const LoadedCodeSection* sections, u32 section_count, u32 address, u32 size) {
    for (u32 i = 0; i < section_count; i++) {
        const LoadedCodeSection* s = &sections[i];
        if (address >= s->address && size <= s->size && address - s->address <= s->size - size)
            return s->data + (address - s->address);
    }
    return NULL;
}

static int compare_u32(const void* left, const void* right) {
    const u32 a = *(const u32*)left, b = *(const u32*)right;
    return a < b ? -1 : a > b;
}

static u32 occurrences(const u32* sorted, u32 count, u32 value) {
    u32 lo = 0, hi = count;
    while (lo < hi) {
        const u32 mid = lo + (hi - lo) / 2u;
        if (sorted[mid] < value)
            lo = mid + 1u;
        else
            hi = mid;
    }
    u32 end = lo;
    while (end < count && sorted[end] == value)
        end++;
    return end - lo;
}

// The length of a database name before its " \t<library> <object>".
static size_t name_length(const char* name) {
    const char* tab = strchr(name, '\t');
    size_t length = tab ? (size_t)(tab - name) : strlen(name);
    while (length && name[length - 1u] == ' ')
        length--;
    return length;
}

// The name without that and its leading underscores.
static size_t bare_name(const char* name, const char** start) {
    while (*name == '_')
        name++;
    *start = name;
    return name_length(name);
}

static int same_sdk_name(const char* symbol, const char* sdk) {
    const char* under = strchr(symbol, '_');
    const char *a, *b;
    const size_t la = bare_name(under ? under + 1 : symbol, &a);
    const size_t lb = bare_name(sdk, &b);
    return la == lb && memcmp(a, b, la) == 0;
}

int sigdb_report(const SigDB* db, const FunctionAnalysis* analysis, const LoadedCodeSection* sections,
                 u32 section_count, const NativeSignatures* signatures, const DolRecompNativeList* natives,
                 const char* out_dir) {
    char names_path[1200], candidates_path[1200];
    if (!join_path(names_path, sizeof(names_path), out_dir, "sdk_names.txt") ||
        !join_path(candidates_path, sizeof(candidates_path), out_dir, "sigs_candidates.txt"))
        return 0;
    const SigDBEntry** named = (const SigDBEntry**)calloc(analysis->function_count + 1u, sizeof(*named));
    u32* checksums = (u32*)malloc((analysis->function_count + 1u) * sizeof(u32));
    u32* sorted = (u32*)malloc((analysis->function_count + 1u) * sizeof(u32));
    FILE* names = fopen(names_path, "w");
    FILE* candidates = fopen(candidates_path, "w");
    int ok = named && checksums && sorted && names && candidates;
    u32 named_count = 0;
    if (ok)
        fprintf(names, "# Generated by DolRecomp: the functions Dolphin's signature database names.\n"
                       "# <address> <size> <name> [<library> <object>]\n");
    for (u32 i = 0; ok && i < analysis->function_count; i++) {
        const GuestFunction* f = &analysis->functions[i];
        const u8* code = code_at(sections, section_count, f->start, f->end - f->start);
        checksums[i] = sorted[i] = code ? sigdb_checksum(code, f->end - f->start) : 0;
    }
    if (ok)
        qsort(sorted, analysis->function_count, sizeof(u32), compare_u32);
    for (u32 i = 0; ok && i < analysis->function_count; i++) {
        // Code of one shape in two places (a getter, a thunk) names neither.
        const GuestFunction* f = &analysis->functions[i];
        named[i] = checksums[i] && occurrences(sorted, analysis->function_count, checksums[i]) == 1
                       ? sigdb_find(db, checksums[i], f->end - f->start)
                       : NULL;
        if (named[i]) {
            const char* tab = strchr(named[i]->name, '\t');
            fprintf(names, "%08X %04X %.*s%s%s\n", f->start, f->end - f->start,
                    (int)name_length(named[i]->name), named[i]->name, tab ? " " : "", tab ? tab + 1 : "");
            named_count++;
        }
    }

    // What the database says of the natives found, and lines for the rest.
    u32 agree = 0, other = 0, candidate_count = 0;
    for (u32 n = 0; ok && n < natives->count; n++) {
        const GuestFunction* f = function_analysis_find(analysis, natives->entries[n].address);
        const SigDBEntry* entry =
            f && f->start == natives->entries[n].address ? named[f - analysis->functions] : NULL;
        if (entry && same_sdk_name(natives->entries[n].name, entry->name))
            agree++;
        else if (entry)
            other++;
    }
    if (ok)
        fprintf(candidates, "# Generated by DolRecomp: .sigs lines for the code Dolphin's signature\n"
                            "# database names as a signature this game didn't match. Review each\n"
                            "# against the native's SDK build before using it.\n");
    for (u32 s = 0; ok && s < signatures->count; s++) {
        const NativeSignature* sig = &signatures->entries[s];
        int found = sig->symbol[0] == '@' || sig->at != NULL;
        for (u32 n = 0; !found && n < natives->count; n++)
            found = strcmp(natives->entries[n].name, sig->symbol) == 0;
        for (u32 i = 0; !found && i < analysis->function_count; i++) {
            if (!named[i] || !same_sdk_name(sig->symbol, named[i]->name))
                continue;
            const GuestFunction* f = &analysis->functions[i];
            u32 first = 0;
            u64 hash = 0;
            if (natives_sign(sections, section_count, f->start, f->end - f->start, &first, &hash)) {
                fprintf(candidates, "%s %X %08X %016llX  # at %08X, was %X %08X %016llX\n", sig->symbol,
                        f->end - f->start, first, (unsigned long long)hash, f->start, sig->size,
                        sig->first, (unsigned long long)sig->hash);
                candidate_count++;
            }
        }
    }
    if (ok)
        printf("sigdb: %u functions named; natives: %u agree, %u named otherwise; %u candidate signatures\n",
               named_count, agree, other, candidate_count);
    else
        fprintf(stderr, "error: can't write the sigdb report to %s\n", out_dir);
    if (names)
        fclose(names);
    if (candidates)
        fclose(candidates);
    free(named);
    free(checksums);
    free(sorted);
    return ok;
}
