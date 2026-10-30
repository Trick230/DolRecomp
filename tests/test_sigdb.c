// Dolphin's signature database: the checksum against values from an
// independent implementation, and the lookup over a small database file.
#include "app/sigdb.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                \
        }                                                              \
    } while (0)

static void put_be(u8* out, const u32* words, u32 count) {
    for (u32 i = 0; i < count; i++) {
        out[i * 4] = (u8)(words[i] >> 24);
        out[i * 4 + 1] = (u8)(words[i] >> 16);
        out[i * 4 + 2] = (u8)(words[i] >> 8);
        out[i * 4 + 3] = (u8)words[i];
    }
}

static void put_le(FILE* file, u32 value) {
    const u8 bytes[4] = {(u8)value, (u8)(value >> 8), (u8)(value >> 16), (u8)(value >> 24)};
    fwrite(bytes, 1, 4, file);
}

static void put_record(FILE* file, u32 checksum, u32 size, const char* name) {
    char text[128] = {0};
    snprintf(text, sizeof(text), "%s", name);
    put_le(file, checksum);
    put_le(file, size);
    fwrite(text, 1, sizeof(text), file);
}

int main(void) {
    // stwu, mflr, li, fmr, ps_mr, fadds, lwz, blr; then other registers and
    // immediates, which only the D-form opcodes' rD/rA fields keep.
    static const u32 code[] = {0x9421FFE0, 0x7C0802A6, 0x38600000, 0xFC000090,
                               0x10000420, 0xEC21102A, 0x80630004, 0x4E800020};
    static const u32 other[] = {0x9421FFE0, 0x7C0802A6, 0x38800010, 0xFC200090,
                                0x10220420, 0xEC63202A, 0x80840008, 0x4E800020};
    u8 bytes[sizeof(code)], other_bytes[sizeof(other)];
    put_be(bytes, code, 8);
    put_be(other_bytes, other, 8);
    CHECK(sigdb_checksum(bytes, sizeof(bytes)) == 0x3D4C3C5Du);
    CHECK(sigdb_checksum(other_bytes, sizeof(other_bytes)) == 0x3D4C2193u);

    const char* path = "test_sigdb.dsy";
    FILE* file = fopen(path, "wb");
    CHECK(file != NULL);
    if (!file)
        return 1;
    put_le(file, 4);
    put_record(file, 0x3D4C3C5Du, 0x20, "OSGetThing \tos.a OSThing.o");
    put_record(file, 0x11111111u, 0x10, "PSVECDot");
    put_record(file, 0x11111111u, 0x10, "PSQUATDot");
    put_record(file, 0x22222222u, 0x08, "Same");
    fclose(file);

    SigDB db;
    CHECK(sigdb_load(&db, path));
    CHECK(db.count == 4);
    const SigDBEntry* found = sigdb_find(&db, 0x3D4C3C5Du, 0x20);
    CHECK(found && strncmp(found->name, "OSGetThing", 10) == 0);
    CHECK(sigdb_find(&db, 0x3D4C3C5Du, 0x24) == NULL);  // size differs
    CHECK(sigdb_find(&db, 0x11111111u, 0x10) == NULL);  // two names, one checksum
    CHECK(sigdb_find(&db, 0x22222222u, 0x08) != NULL);
    CHECK(sigdb_find(&db, 0x33333333u, 0x08) == NULL);
    sigdb_free(&db);
    remove(path);

    if (failures == 0)
        printf("sigdb: ok\n");
    return failures != 0;
}
