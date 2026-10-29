#include "app/natives.h"
#include "backend/emitter.h"
#include "frontend/decoder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "check failed: %s:%d: %s\n", \
    __FILE__, __LINE__, #x); return 1; } } while (0)

static char* read_text(const char* path) {
    FILE* file = fopen(path, "rb");
    if (!file)
        return NULL;
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    char* text = malloc((size_t)size + 1);
    if (text && fread(text, 1, (size_t)size, file) != (size_t)size) {
        free(text);
        text = NULL;
    }
    if (text)
        text[size] = '\0';
    fclose(file);
    return text;
}

static void put_words(u8* out, const u32* words, u32 count) {
    for (u32 i = 0; i < count; ++i) {
        out[i * 4] = (u8)(words[i] >> 24);
        out[i * 4 + 1] = (u8)(words[i] >> 16);
        out[i * 4 + 2] = (u8)(words[i] >> 8);
        out[i * 4 + 3] = (u8)words[i];
    }
}

static int resolve_text(DolRecompNativeList* list, const char* text,
                        const LoadedCodeSection* sections, u32 count) {
    NativeSignatures signatures = {0};
    const int ok = natives_parse(&signatures, text, "test") &&
                   natives_resolve(list, &signatures, sections, count);
    natives_signatures_free(&signatures);
    return ok;
}

int main(void) {
    // A getter (lis; lwz off it; lwz off r13; blr) and a caller (bl; blr),
    // then the same getter linked elsewhere, and one reading another base.
    static const u32 getter[] = {0x3C608040u, 0x80631234u, 0x806D8000u, 0x4E800020u,
                                 0x4BFFFFF1u, 0x4E800020u};
    static const u32 relinked[] = {0x3C608050u, 0x80635678u, 0x806D9000u, 0x4E800020u};
    static const u32 other[] = {0x3C608040u, 0x80631234u, 0x806C8000u, 0x4E800020u};
    u8 code[3][24];
    put_words(code[0], getter, 6);
    put_words(code[1], relinked, 4);
    put_words(code[2], other, 4);
    LoadedCodeSection sections[3] = {{0}};
    for (u32 i = 0; i < 3; ++i) {
        sections[i].data = code[i];
        sections[i].address = 0x80001000u + i * 0x1000u;
        sections[i].size = i ? 16 : 24;
    }

    // The linker's bits are masked: the relinked copy signs the same.
    u32 first = 0, first_relinked = 0, first_caller = 0;
    u64 hash = 0, hash_relinked = 0, hash_caller = 0;
    CHECK(natives_sign(sections, 3, 0x80001000u, 16, &first, &hash));
    CHECK(natives_sign(sections, 3, 0x80002000u, 16, &first_relinked, &hash_relinked));
    CHECK(first == first_relinked && hash == hash_relinked && first == 0x3C600000u);
    CHECK(natives_sign(sections, 3, 0x80001010u, 8, &first_caller, &hash_caller));
    CHECK(!natives_sign(sections, 3, 0x80001014u, 8, &first_caller, &hash_caller));

    char sigs[1024];
    DolRecompNativeList list;
    // Found once: in the first section alone; both copies are ambiguous; the
    // other base is other code.
    snprintf(sigs, sizeof(sigs), "host_get 10 %08X %016llX\n", first, (unsigned long long)hash);
    CHECK(resolve_text(&list, sigs, sections, 1));
    CHECK(list.count == 1 && list.entries[0].address == 0x80001000u && !list.entries[0].jump);
    CHECK(list.sizes[0] == 16 && list.hashes[0] == 0x09817D2F071E01E3ull);
    natives_free(&list);
    CHECK(!resolve_text(&list, sigs, sections, 2));
    CHECK(resolve_text(&list, sigs, sections + 2, 1) && list.count == 0);
    natives_free(&list);

    // Found at an anchor's call, with defines read off it.
    snprintf(sigs, sizeof(sigs),
             "# comment\n"
             "@caller 8 %08X %016llX\n"
             "host_get 10 %08X %016llX at call(@caller)  # trailing comment\n"
             "host_jump 4 4E800020 %016llX at host_get+C jump\n"
             "define GETTER host_get\n"
             "define GLOBAL addr(host_get,4)\n"
             "define SDA_GLOBAL imm(host_get+8)\n"
             "define MISSING absent\n"
             "absent 4 0 0\n",
             first_caller, (unsigned long long)hash_caller, first, (unsigned long long)hash,
             0xB6F2E0C4BC8017BBull);
    CHECK(resolve_text(&list, sigs, sections, 2));
    CHECK(list.count == 2 && strcmp(list.entries[0].name, "host_get") == 0);
    CHECK(list.entries[1].address == 0x8000100Cu && list.entries[1].jump);
    CHECK(list.define_count == 3 && strcmp(list.define_names[2], "SDA_GLOBAL") == 0);
    CHECK(list.define_values[0] == 0x80001000 && list.define_values[1] == 0x80401234 &&
          list.define_values[2] == -32768);
    const char* header_path = "test_natives.h";
    CHECK(natives_write_header(&list, header_path));
    char* header = read_text(header_path);
    CHECK(header && strstr(header, "#define SDA_GLOBAL (-32768)") &&
          strstr(header, "#define GLOBAL 0x80401234u"));
    free(header);
    remove(header_path);
    natives_free(&list);

    // Malformed lines and expressions.
    static const char* const bad[] = {
        "define 1BAD host_get\n", "a 6 0 0\n", "a 4 0 0 leap\n", "a 4 0 0 jump extra\n",
        "@a 4 0 0 jump\n", "a 4 0 0 at\n", "not-a-symbol 4 0 0\n",
        "a 4 0 0\nb 4 0 0 at nowhere\n", "a 4 0 0\ndefine X addr(a)\n",
    };
    for (u32 i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i)
        CHECK(!resolve_text(&list, bad[i], sections + 2, 1));

    // A bl to a native resumes after the call when the native accepts, and a
    // native function entry checks it before running its guest body.
    static const DolRecompNative natives[] = {{0x80001008u, "host_leaf", false}};
    PPCInst insts[3];
    const u32 words[3] = {0x48000009u /* bl +8 */, 0x60000000u, 0x4E800020u};
    for (u32 i = 0; i < 3; ++i)
        insts[i] = ppc_decode(words[i], 0x80001000u + i * 4u);
    const char* out_path = "test_natives.c.out";
    FILE* out = fopen(out_path, "wb");
    CHECK(out);
    emit_set_natives(natives, 1);
    CHECK(emit_function(out, insts, 3, 0x80001000u));
    emit_set_natives(NULL, 0);
    fclose(out);
    char* text = read_text(out_path);
    CHECK(text);
    CHECK(strstr(text, "if (host_leaf(ctx)) {\n"
                       "                    if (dolrecomp_idle_pc != 0x80001004u)\n"
                       "                        goto label_80001004;\n"
                       "                    ctx->pc = 0x80001004u;\n"
                       "                    return;"));
    CHECK(strstr(text, "goto native_declined_80001008;"));
    CHECK(strstr(text, "label_80001008:\n    {\n        bool host_leaf(CPUState* ctx);"));
    free(text);

    // A jump native's callers leave to the dispatcher at the pc it set.
    static const DolRecompNative jumps[] = {{0x80001008u, "host_jump", true}};
    out = fopen(out_path, "wb");
    CHECK(out);
    emit_set_natives(jumps, 1);
    CHECK(emit_function(out, insts, 3, 0x80001000u));
    emit_set_natives(NULL, 0);
    fclose(out);
    text = read_text(out_path);
    CHECK(text);
    CHECK(strstr(text, "if (host_jump(ctx)) {\n"
                       "                    if (ctx->pc == 0x80001004u && dolrecomp_idle_pc != 0x80001004u)\n"
                       "                        goto label_80001004;\n"
                       "                    return;"));
    CHECK(strstr(text, "if (host_jump(ctx)) {\n            return;"));
    free(text);
    remove(out_path);
    return 0;
}
