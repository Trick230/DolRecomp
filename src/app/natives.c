#include "app/natives.h"

#include "platform/fs.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static u64 fnv1a64(u64 hash, const u8* data, u32 size) {
    for (u32 i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= 0x100000001B3ull;
    }
    return hash;
}

#define FNV_BASIS 0xCBF29CE484222325ull

static u32 read_word(const u8* p) {
    return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3];
}

// The bits of an instruction at pc the linker decides, as a mask of the ones
// it doesn't. Branches into [start, end) keep their offset; lis records the
// registers a lis loaded, whose low halves are the linker's too.
static u32 linker_mask(u32 word, u32 pc, u32 start, u32 end, u32* lis) {
    const u32 op = word >> 26;
    const u32 rd = (word >> 21) & 31;
    const u32 ra = (word >> 16) & 31;
    switch (op) {
    case 18: {  // b
        const u32 li = (u32)((s32)(word << 6) >> 6) & ~3u;
        const u32 target = (word & 2 ? 0 : pc) + li;
        return target >= start && target < end ? 0xFFFFFFFFu : 0xFC000003u;
    }
    case 16: {  // bc
        const u32 bd = (u32)(s32)(s16)(word & 0xFFFC);
        const u32 target = (word & 2 ? 0 : pc) + bd;
        return target >= start && target < end ? 0xFFFFFFFFu : 0xFFFF0003u;
    }
    case 15:  // addis: lis, or a small-data high half
        if (ra == 0)
            *lis |= 1u << rd;
        return 0xFFFF0000u;
    case 24:  // ori rA, rS: the low half of a lis'd rS
        return *lis >> rd & 1 ? 0xFFFF0000u : 0xFFFFFFFFu;
    case 56: case 57: case 60: case 61:  // psq_l(u), psq_st(u): 12-bit offset
        return ra == 2 || ra == 13 ? 0xFFFFF000u : 0xFFFFFFFFu;
    default:
        // addi and the D-form loads and stores, off r2, r13 or a lis'd base
        // (rA 0 reads as 0, not r0).
        if ((op == 14 || (op >= 32 && op <= 55)) && ra != 0 &&
            (ra == 2 || ra == 13 || *lis >> ra & 1))
            return 0xFFFF0000u;
        return 0xFFFFFFFFu;
    }
}

// The first word as a search compares it, without knowing the function's end.
static u32 masked_first(u32 word, u32 pc) {
    u32 lis = 0;
    return word & linker_mask(word, pc, 0, 0, &lis);
}

static u64 masked_hash(const u8* code, u32 address, u32 size) {
    u64 hash = FNV_BASIS;
    u32 lis = 0;
    for (u32 offset = 0; offset < size; offset += 4) {
        const u32 word = read_word(code + offset);
        const u32 masked = word & linker_mask(word, address + offset, address, address + size, &lis);
        const u8 bytes[4] = {(u8)(masked >> 24), (u8)(masked >> 16), (u8)(masked >> 8), (u8)masked};
        hash = fnv1a64(hash, bytes, 4);
    }
    return hash;
}

static const u8* code_at(const LoadedCodeSection* sections, u32 section_count, u32 address,
                         u32 size) {
    for (u32 s = 0; s < section_count; ++s) {
        if (address >= sections[s].address &&
            (u64)address + size <= (u64)sections[s].address + sections[s].size)
            return sections[s].data + (address - sections[s].address);
    }
    return NULL;
}

static int valid_symbol(const char* name) {
    if (!isalpha((unsigned char)name[0]) && name[0] != '_')
        return 0;
    for (const char* c = name; *c; ++c) {
        if (!isalnum((unsigned char)*c) && *c != '_')
            return 0;
    }
    return 1;
}

static char* copy_text(const char* text) {
    char* owned = malloc(strlen(text) + 1);
    if (owned)
        strcpy(owned, text);
    return owned;
}

static int grow(void** array, u32 count, size_t element) {
    // Doubles at powers of two.
    if (count & (count - 1))
        return 1;
    void* grown = realloc(*array, (count ? count * 2 : 8) * element);
    if (!grown)
        return 0;
    *array = grown;
    return 1;
}

int natives_parse(NativeSignatures* signatures, const char* text, const char* origin) {
    u32 line_number = 0;
    while (*text) {
        ++line_number;
        const char* newline = strchr(text, '\n');
        const size_t length = newline ? (size_t)(newline - text) : strlen(text);
        char line[512];
        if (length >= sizeof(line)) {
            fprintf(stderr, "error: %s:%u: line too long\n", origin, line_number);
            return 0;
        }
        memcpy(line, text, length);
        line[length] = '\0';
        text += length + (newline != NULL);
        char* comment = strchr(line, '#');
        if (comment)
            *comment = '\0';

        char token[8][256];  // the eighth only catches extra fields
        const int count =
            sscanf(line, "%255s %255s %255s %255s %255s %255s %255s %255s", token[0], token[1],
                   token[2], token[3], token[4], token[5], token[6], token[7]);
        if (count <= 0)
            continue;
        if (strcmp(token[0], "define") == 0) {
            if (count != 3 || !valid_symbol(token[1])) {
                fprintf(stderr, "error: %s:%u: expected 'define <NAME> <expression>'\n", origin,
                        line_number);
                return 0;
            }
            if (!grow((void**)&signatures->defines, signatures->define_count,
                      sizeof(*signatures->defines)))
                goto out_of_memory;
            NativeDefine* define = &signatures->defines[signatures->define_count];
            define->name = copy_text(token[1]);
            define->expression = copy_text(token[2]);
            if (!define->name || !define->expression) {
                free(define->name);
                free(define->expression);
                goto out_of_memory;
            }
            ++signatures->define_count;
            continue;
        }

        NativeSignature entry = {0};
        const char* at = NULL;
        char* end = NULL;
        int ok = count >= 4 && valid_symbol(token[0] + (token[0][0] == '@'));
        if (ok) {
            entry.size = (u32)strtoul(token[1], &end, 16);
            ok = *end == '\0' && entry.size != 0 && (entry.size & 3u) == 0;
        }
        if (ok) {
            entry.first = (u32)strtoul(token[2], &end, 16);
            ok = *end == '\0';
        }
        if (ok) {
            entry.hash = strtoull(token[3], &end, 16);
            ok = *end == '\0';
        }
        int next = 4;
        if (ok && next < count && strcmp(token[next], "at") == 0) {
            ok = next + 1 < count;
            if (ok)
                at = token[next + 1];
            next += 2;
        }
        if (ok && next < count && strcmp(token[next], "jump") == 0) {
            entry.jump = 1;
            ++next;
        }
        if (!ok || next != count || (entry.jump && token[0][0] == '@')) {
            fprintf(stderr,
                    "error: %s:%u: expected '<symbol> <size> <first> <hash> [at <expression>] "
                    "[jump]'\n",
                    origin, line_number);
            return 0;
        }
        if (!grow((void**)&signatures->entries, signatures->count, sizeof(*signatures->entries)))
            goto out_of_memory;
        entry.symbol = copy_text(token[0]);
        entry.at = at ? copy_text(at) : NULL;
        if (!entry.symbol || (at && !entry.at)) {
            free(entry.symbol);
            free(entry.at);
            goto out_of_memory;
        }
        signatures->entries[signatures->count++] = entry;
    }
    return 1;

out_of_memory:
    fprintf(stderr, "error: out of memory reading %s\n", origin);
    return 0;
}

int natives_load(NativeSignatures* signatures, const char* path) {
    FILE* file = fopen(path, "rb");
    if (!file) {
        fprintf(stderr, "error: cannot open signatures %s\n", path);
        return 0;
    }
    fseek(file, 0, SEEK_END);
    const long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    char* text = size >= 0 ? malloc((size_t)size + 1) : NULL;
    const int read = text && fread(text, 1, (size_t)size, file) == (size_t)size;
    fclose(file);
    if (!read) {
        free(text);
        fprintf(stderr, "error: cannot read signatures %s\n", path);
        return 0;
    }
    text[size] = '\0';
    const int ok = natives_parse(signatures, text, path);
    free(text);
    return ok;
}

void natives_signatures_free(NativeSignatures* signatures) {
    for (u32 i = 0; i < signatures->count; ++i) {
        free(signatures->entries[i].symbol);
        free(signatures->entries[i].at);
    }
    for (u32 i = 0; i < signatures->define_count; ++i) {
        free(signatures->defines[i].name);
        free(signatures->defines[i].expression);
    }
    free(signatures->entries);
    free(signatures->defines);
    memset(signatures, 0, sizeof(*signatures));
}

int natives_sign(const LoadedCodeSection* sections, u32 section_count, u32 address, u32 size,
                 u32* first, u64* hash) {
    const u8* code = (address & 3u) == 0 && size && (size & 3u) == 0
                         ? code_at(sections, section_count, address, size)
                         : NULL;
    if (!code)
        return 0;
    *first = masked_first(read_word(code), address);
    *hash = masked_hash(code, address, size);
    return 1;
}

enum { PENDING, FOUND, ABSENT };

typedef struct {
    const NativeSignatures* signatures;
    const LoadedCodeSection* sections;
    u32 section_count;
    u32* addresses;
    u8* states;
} Resolver;

// 1 with the value, 0 while an entry it needs is pending, -1 when it can't be
// had (an absent entry, or the code there isn't what the expression reads).
static int evaluate(const Resolver* r, const char* expression, long long* value,
                    const char** needs) {
    static const char* const kinds[] = {"call(", "addr(", "imm("};
    int kind = -1;
    char text[256];
    for (int k = 0; k < 3; ++k) {
        if (strncmp(expression, kinds[k], strlen(kinds[k])) == 0)
            kind = k;
    }
    snprintf(text, sizeof(text), "%s", kind < 0 ? expression : expression + strlen(kinds[kind]));
    if (kind >= 0) {
        char* close = strrchr(text, ')');
        if (!close || close[1] != '\0')
            return -1;
        *close = '\0';
    }
    u32 second = 0;
    char* comma = strchr(text, ',');
    if (comma) {
        char* end = NULL;
        second = (u32)strtoul(comma + 1, &end, 16);
        if (kind != 1 || *end != '\0')
            return -1;
        *comma = '\0';
    } else if (kind == 1) {
        return -1;
    }
    u32 offset = 0;
    char* plus = strchr(text, '+');
    if (plus) {
        char* end = NULL;
        offset = (u32)strtoul(plus + 1, &end, 16);
        if (*end != '\0')
            return -1;
        *plus = '\0';
    }
    u32 entry = r->signatures->count;
    for (u32 i = 0; i < r->signatures->count && entry == r->signatures->count; ++i) {
        if (strcmp(r->signatures->entries[i].symbol, text) == 0)
            entry = i;
    }
    if (entry == r->signatures->count)
        return -1;
    *needs = r->signatures->entries[entry].symbol;
    if (r->states[entry] != FOUND)
        return r->states[entry] == PENDING ? 0 : -1;

    const u32 address = r->addresses[entry] + offset;
    if (kind < 0) {
        *value = address;
        return 1;
    }
    const u8* code = code_at(r->sections, r->section_count, address, 4);
    if (!code)
        return -1;
    const u32 word = read_word(code);
    const u32 op = word >> 26;
    if (kind == 0) {  // call
        u32 target;
        if (op == 18)
            target = (u32)((s32)(word << 6) >> 6) & ~3u;
        else if (op == 16)
            target = (u32)(s32)(s16)(word & 0xFFFC);
        else
            return -1;
        *value = (u32)((word & 2 ? 0 : address) + target);
        return 1;
    }
    if (kind == 2) {  // imm
        *value = (s16)(word & 0xFFFF);
        return 1;
    }
    // addr: lis, then addi, ori or a load or store off it
    const u8* low = code_at(r->sections, r->section_count, r->addresses[entry] + second, 4);
    if (op != 15 || !low)
        return -1;
    const u32 low_word = read_word(low);
    const u32 low_op = low_word >> 26;
    if (low_op != 14 && low_op != 24 && !(low_op >= 32 && low_op <= 55))
        return -1;
    const u32 half = low_op == 24 ? (low_word & 0xFFFF) : (u32)(s32)(s16)(low_word & 0xFFFF);
    *value = (u32)((word << 16) + half);
    return 1;
}

typedef struct {
    u32 first;
    u32 entry;
} FirstWord;

static int compare_first(const void* a, const void* b) {
    const FirstWord* x = a;
    const FirstWord* y = b;
    return x->first < y->first ? -1 : x->first > y->first;
}

// Finds the entries without `at`, each of which must occur once.
static int search(Resolver* r) {
    const NativeSignatures* signatures = r->signatures;
    FirstWord* index = malloc((signatures->count + 1) * sizeof(*index));
    u32* hits = calloc(signatures->count + 1, sizeof(*hits));
    if (!index || !hits) {
        free(index);
        free(hits);
        fprintf(stderr, "error: out of memory finding natives\n");
        return 0;
    }
    u32 searched = 0;
    for (u32 i = 0; i < signatures->count; ++i) {
        if (!signatures->entries[i].at)
            index[searched++] = (FirstWord){signatures->entries[i].first, i};
    }
    qsort(index, searched, sizeof(*index), compare_first);

    int ok = 1;
    for (u32 s = 0; s < r->section_count; ++s) {
        const LoadedCodeSection* section = &r->sections[s];
        for (u32 offset = 0; offset + 4 <= section->size; offset += 4) {
            const u32 address = section->address + offset;
            const FirstWord key = {masked_first(read_word(section->data + offset), address), 0};
            const FirstWord* match = bsearch(&key, index, searched, sizeof(*index), compare_first);
            if (!match)
                continue;
            while (match > index && match[-1].first == key.first)
                --match;
            for (; match < index + searched && match->first == key.first; ++match) {
                const NativeSignature* entry = &signatures->entries[match->entry];
                if (offset + entry->size > section->size ||
                    masked_hash(section->data + offset, address, entry->size) != entry->hash)
                    continue;
                if (hits[match->entry]++ == 0) {
                    r->addresses[match->entry] = address;
                } else {
                    fprintf(stderr,
                            "error: native %s matches both %08X and %08X: find it `at` another "
                            "entry\n",
                            entry->symbol, r->addresses[match->entry], address);
                    ok = 0;
                }
            }
        }
    }
    for (u32 i = 0; i < searched; ++i)
        r->states[index[i].entry] = hits[index[i].entry] ? FOUND : ABSENT;
    free(index);
    free(hits);
    return ok;
}

// Then those found `at` an expression, in as many passes as they need each other.
static int locate(Resolver* r) {
    const NativeSignatures* signatures = r->signatures;
    for (int progress = 1; progress;) {
        progress = 0;
        for (u32 i = 0; i < signatures->count; ++i) {
            const NativeSignature* entry = &signatures->entries[i];
            if (r->states[i] != PENDING)
                continue;
            long long value = 0;
            const char* needs = NULL;
            const int result = evaluate(r, entry->at, &value, &needs);
            if (result == 0)
                continue;
            progress = 1;
            r->states[i] = ABSENT;
            if (result < 0 && !needs) {
                fprintf(stderr, "error: native %s: cannot read 'at %s'\n", entry->symbol,
                        entry->at);
                return 0;
            }
            if (result < 0)
                continue;
            const u32 address = (u32)value;
            const u8* code = (address & 3u) == 0
                                 ? code_at(r->sections, r->section_count, address, entry->size)
                                 : NULL;
            if (code && masked_first(read_word(code), address) == entry->first &&
                masked_hash(code, address, entry->size) == entry->hash) {
                r->addresses[i] = address;
                r->states[i] = FOUND;
            } else {
                printf("note: native %s at %s (%08X) is other code, skipped\n", entry->symbol,
                       entry->at, address);
            }
        }
    }
    // What's left needs itself.
    for (u32 i = 0; i < signatures->count; ++i) {
        if (r->states[i] == PENDING)
            r->states[i] = ABSENT;
    }
    return 1;
}

static int compare_natives(const void* a, const void* b) {
    const DolRecompNative* x = a;
    const DolRecompNative* y = b;
    return x->address < y->address ? -1 : x->address > y->address;
}

int natives_resolve(DolRecompNativeList* list, const NativeSignatures* signatures,
                    const LoadedCodeSection* sections, u32 section_count) {
    memset(list, 0, sizeof(*list));
    const u32 count = signatures->count;
    Resolver r = {signatures, sections, section_count, calloc(count + 1, sizeof(u32)),
                  calloc(count + 1, 1)};
    int ok = r.addresses && r.states;
    if (!ok)
        fprintf(stderr, "error: out of memory finding natives\n");
    ok = ok && search(&r) && locate(&r);

    u32 found = 0;
    for (u32 i = 0; ok && i < count; ++i)
        found += r.states[i] == FOUND && signatures->entries[i].symbol[0] != '@';
    list->entries = ok ? malloc((found + 1) * sizeof(*list->entries)) : NULL;
    list->sizes = ok ? malloc((found + 1) * sizeof(*list->sizes)) : NULL;
    list->hashes = ok ? malloc((found + 1) * sizeof(*list->hashes)) : NULL;
    if (ok && (!list->entries || !list->sizes || !list->hashes)) {
        fprintf(stderr, "error: out of memory finding natives\n");
        ok = 0;
    }
    for (u32 i = 0; ok && i < count; ++i) {
        const NativeSignature* entry = &signatures->entries[i];
        if (r.states[i] != FOUND || entry->symbol[0] == '@')
            continue;
        char* name = copy_text(entry->symbol);
        if (!name) {
            fprintf(stderr, "error: out of memory finding natives\n");
            ok = 0;
            break;
        }
        list->entries[list->count++] = (DolRecompNative){r.addresses[i], name, entry->jump};
    }
    if (ok) {
        qsort(list->entries, list->count, sizeof(*list->entries), compare_natives);
        for (u32 i = 0; i < list->count; ++i) {
            const DolRecompNative* native = &list->entries[i];
            u32 size = 0;
            for (u32 e = 0; e < count && !size; ++e) {
                if (strcmp(signatures->entries[e].symbol, native->name) == 0)
                    size = signatures->entries[e].size;
            }
            list->sizes[i] = size;
            list->hashes[i] = fnv1a64(FNV_BASIS, code_at(sections, section_count, native->address, size), size);
            if (i && list->entries[i - 1].address == native->address) {
                fprintf(stderr, "error: natives %s and %s are both at %08X\n",
                        list->entries[i - 1].name, native->name, native->address);
                ok = 0;
            }
        }
    }

    if (ok && signatures->define_count) {
        list->define_names = malloc(signatures->define_count * sizeof(*list->define_names));
        list->define_values = malloc(signatures->define_count * sizeof(*list->define_values));
        ok = list->define_names && list->define_values;
        if (!ok)
            fprintf(stderr, "error: out of memory finding natives\n");
    }
    for (u32 i = 0; ok && i < signatures->define_count; ++i) {
        const NativeDefine* define = &signatures->defines[i];
        long long value = 0;
        const char* needs = NULL;
        const int result = evaluate(&r, define->expression, &value, &needs);
        if (result <= 0) {
            if (!needs) {
                fprintf(stderr, "error: define %s: cannot read '%s'\n", define->name,
                        define->expression);
                ok = 0;
            }
            continue;  // what it reads isn't in this game, nor are its natives
        }
        char* name = copy_text(define->name);
        if (!name) {
            fprintf(stderr, "error: out of memory finding natives\n");
            ok = 0;
            break;
        }
        list->define_names[list->define_count] = name;
        list->define_values[list->define_count++] = value;
    }

    free(r.addresses);
    free(r.states);
    if (!ok)
        natives_free(list);
    return ok;
}

int natives_write_list(const DolRecompNativeList* list, const char* path) {
    OutputFile output;
    if (!output_open(&output, path, "w")) {
        fprintf(stderr, "error: cannot write %s\n", path);
        return 0;
    }
    fputs("# Generated by DolRecomp: the natives its signatures found in this game.\n"
          "# <address> <size> <fnv1a64 of the code bytes> <symbol> [jump]\n",
          output.stream);
    for (u32 i = 0; i < list->count; ++i)
        fprintf(output.stream, "%08X %04X %016llX %s%s\n", list->entries[i].address,
                list->sizes[i], (unsigned long long)list->hashes[i], list->entries[i].name,
                list->entries[i].jump ? " jump" : "");
    return output_close(&output);
}

int natives_add_define(DolRecompNativeList* list, const char* name, long long value) {
    char** names = realloc(list->define_names, (list->define_count + 1u) * sizeof(*names));
    if (names)
        list->define_names = names;
    long long* values = realloc(list->define_values, (list->define_count + 1u) * sizeof(*values));
    if (values)
        list->define_values = values;
    char* copy = names && values ? copy_text(name) : NULL;
    if (!copy) {
        fprintf(stderr, "error: out of memory adding define %s\n", name);
        return 0;
    }
    list->define_names[list->define_count] = copy;
    list->define_values[list->define_count++] = value;
    return 1;
}

int natives_write_header(const DolRecompNativeList* list, const char* path) {
    OutputFile output;
    if (!output_open(&output, path, "w")) {
        fprintf(stderr, "error: cannot write %s\n", path);
        return 0;
    }
    fputs("// Generated by DolRecomp: the constants the natives list defines for this\n"
          "// game's build, and the config's [defines].\n\n#pragma once\n",
          output.stream);
    if (list->define_count)
        fputc('\n', output.stream);
    for (u32 i = 0; i < list->define_count; ++i) {
        const long long value = list->define_values[i];
        if (value < 0)
            fprintf(output.stream, "#define %s (%lld)\n", list->define_names[i], value);
        else
            fprintf(output.stream, "#define %s 0x%llXu\n", list->define_names[i], value);
    }
    return output_close(&output);
}

void natives_free(DolRecompNativeList* list) {
    for (u32 i = 0; i < list->count; ++i)
        free((char*)list->entries[i].name);
    for (u32 i = 0; i < list->define_count; ++i)
        free(list->define_names[i]);
    free(list->entries);
    free(list->sizes);
    free(list->hashes);
    free(list->define_names);
    free(list->define_values);
    memset(list, 0, sizeof(*list));
}
