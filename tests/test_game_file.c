#include "app/game_file.h"
#include "common/types.h"
#include "platform/fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        fprintf(stderr, __VA_ARGS__); \
        fprintf(stderr, "\n"); \
        return 0; \
    } \
} while (0)

static int write_file(const char* path, const u8* data, u32 size) {
    FILE* out = fopen(path, "wb");
    if (!out)
        return 0;
    int ok = fwrite(data, 1, size, out) == size;
    return fclose(out) == 0 && ok;
}

static const char TEXT[] = "abcabcabcabcabcabcabcabcabcabcabcabcabcabcXY";

// TEXT as Yaz0: 3 literals, a 2-byte back reference (7 bytes), a 3-byte one
// (0x12 + 0x0E = 32 bytes, overlapping its own output), then 2 literals.
static u32 yaz0(u8* out) {
    memset(out, 0, 16);
    memcpy(out, "Yaz0", 4);
    write_be32(out + 4, sizeof(TEXT) - 1);
    u8* p = out + 16;
    *p++ = 0xE6;  // 1 1 1 0 0 1 1 (0)
    *p++ = 'a';
    *p++ = 'b';
    *p++ = 'c';
    *p++ = 0x50;  // n = 5: 7 bytes, distance 3
    *p++ = 0x02;
    *p++ = 0x00;  // n = 0: the third byte + 0x12, distance 3
    *p++ = 0x02;
    *p++ = 0x0E;
    *p++ = 'X';
    *p++ = 'Y';
    return (u32)(p - out);
}

// A U8 archive: / -> rels/ -> {a.rel = "AAAA", b.rel = Yaz0 TEXT}.
static u32 u8_archive(u8* out, const u8* inner, u32 inner_size) {
    memset(out, 0, 0x200);
    write_be32(out, 0x55AA382Du);
    write_be32(out + 4, 0x20);
    static const char strings[] = "\0rels\0a.rel\0b.rel";
    u8* n = out + 0x20;
    n[0] = 1;  // root: children up to node 4
    write_be32(n + 8, 4);
    n += 12;
    write_be32(n, 1);  // rels/
    n[0] = 1;
    write_be32(n + 8, 4);
    n += 12;
    write_be32(n, 6);  // a.rel
    write_be32(n + 4, 0x100);
    write_be32(n + 8, 4);
    n += 12;
    write_be32(n, 12);  // b.rel
    write_be32(n + 4, 0x120);
    write_be32(n + 8, inner_size);
    n += 12;
    memcpy(n, strings, sizeof(strings));
    memcpy(out + 0x100, "AAAA", 4);
    memcpy(out + 0x120, inner, inner_size);
    return 0x120 + inner_size;
}

// A RARC archive: root "arc" -> {x.bin = "XXXX", sub/ -> y.bin = "YY"}.
static u32 rarc_archive(u8* out) {
    memset(out, 0, 0x200);
    memcpy(out, "RARC", 4);
    write_be32(out + 0x0C, 0xE0);  // file data at 0x100
    u8* info = out + 0x20;
    write_be32(info + 0x00, 2);     // nodes
    write_be32(info + 0x04, 0x20);  // at 0x40
    write_be32(info + 0x08, 6);     // entries
    write_be32(info + 0x0C, 0x40);  // at 0x60
    write_be32(info + 0x14, 0xC0);  // strings at 0xE0
    // arc 0, "." 4, ".." 6, x.bin 9, sub 15, y.bin 19
    static const char strings[] = "arc\0.\0..\0x.bin\0sub\0y.bin";
    memcpy(out + 0xE0, strings, sizeof(strings));
    u8* node = out + 0x40;
    memcpy(node, "ROOT", 4);
    write_be32(node + 4, 0);
    write_be16(node + 0x0A, 4);
    write_be32(node + 0x0C, 0);
    node += 0x10;
    memcpy(node, "SUB ", 4);
    write_be32(node + 4, 15);
    write_be16(node + 0x0A, 2);
    write_be32(node + 0x0C, 4);
    struct { u16 name; u8 flags; u32 offset, size; } entries[] = {
        {9, 0x01, 0x00, 4}, {15, 0x02, 1, 0}, {4, 0x02, 0, 0}, {6, 0x02, 0xFFFFFFFFu, 0},
        {19, 0x01, 0x10, 2}, {4, 0x02, 1, 0},
    };
    for (u32 i = 0; i < 6; i++) {
        u8* e = out + 0x60 + i * 0x14;
        write_be16(e, (u16)i);
        e[4] = entries[i].flags;
        write_be16(e + 6, entries[i].name);
        write_be32(e + 8, entries[i].offset);
        write_be32(e + 0x0C, entries[i].size);
    }
    memcpy(out + 0x100, "XXXX", 4);
    memcpy(out + 0x110, "YY", 2);
    return 0x120;
}

static int is(const u8* data, u32 size, const void* want, u32 want_size) {
    return size == want_size && memcmp(data, want, size) == 0;
}

static int test_yaz0(void) {
    u8 packed[64];
    const u32 size = yaz0(packed);
    u8* data = (u8*)malloc(size);
    memcpy(data, packed, size);
    u32 data_size = size;
    CHECK(game_file_unyaz0(&data, &data_size), "unyaz0 failed");
    CHECK(is(data, data_size, TEXT, sizeof(TEXT) - 1), "Yaz0 decoded wrong");
    free(data);

    // A reference reaching before the start is malformed.
    packed[17 + 3] = 0x5F;
    data = (u8*)malloc(size);
    memcpy(data, packed, size);
    data_size = size;
    CHECK(!game_file_unyaz0(&data, &data_size), "bad distance accepted");
    free(data);
    return 1;
}

static int test_archives(void) {
    u8 packed[64], u8a[0x200], rarc[0x200];
    const u32 inner = yaz0(packed);
    const u32 u8_size = u8_archive(u8a, packed, inner);
    const u32 rarc_size = rarc_archive(rarc);
    u8* data = NULL;
    u32 size = 0;

    CHECK(game_file_in_archive(u8a, u8_size, "rels/a.rel", &data, &size) == 1 && is(data, size, "AAAA", 4),
          "U8 a.rel");
    free(data);
    CHECK(game_file_in_archive(u8a, u8_size, "rels/c.rel", &data, &size) == 0, "U8 missing file");
    CHECK(game_file_in_archive(u8a, u8_size, "a.rel", &data, &size) == 0, "U8 file outside its dir");
    CHECK(game_file_in_archive(rarc, rarc_size, "x.bin", &data, &size) == 1 && is(data, size, "XXXX", 4),
          "RARC x.bin");
    free(data);
    CHECK(game_file_in_archive(rarc, rarc_size, "arc/sub/y.bin", &data, &size) == 1 && is(data, size, "YY", 2),
          "RARC arc/sub/y.bin");
    free(data);
    CHECK(game_file_in_archive(rarc, rarc_size, "sub/x.bin", &data, &size) == 0, "RARC wrong dir");
    CHECK(game_file_in_archive(packed, inner, "x", &data, &size) == -1, "not an archive");

    // Through the file system: a Yaz0-compressed U8 (.szs) holding a Yaz0 file.
    u8 szs[0x400];
    memset(szs, 0, 16);
    memcpy(szs, "Yaz0", 4);
    write_be32(szs + 4, u8_size);
    u32 at = 16;
    for (u32 i = 0; i < u8_size; i += 8) {
        szs[at++] = 0xFF;
        for (u32 j = i; j < i + 8 && j < u8_size; j++)
            szs[at++] = u8a[j];
    }
    CHECK(make_dir("game_file_test") || path_is_directory("game_file_test"), "can't make the dir");
    CHECK(write_file("game_file_test/RELS.szs", szs, at) && write_file("game_file_test/plain.bin", packed, inner),
          "can't write the files");
    CHECK(game_file_read("game_file_test", "plain.bin", &data, &size) && is(data, size, TEXT, sizeof(TEXT) - 1),
          "plain Yaz0 file");
    free(data);
    CHECK(game_file_read("game_file_test", "RELS.szs/rels/b.rel", &data, &size) &&
              is(data, size, TEXT, sizeof(TEXT) - 1),
          "Yaz0 file in a Yaz0 U8");
    free(data);
    CHECK(game_file_read("game_file_test", "RELS.szs/rels/a.rel", &data, &size) && is(data, size, "AAAA", 4),
          "file in a Yaz0 U8");
    free(data);
    CHECK(!game_file_read("game_file_test", "RELS.szs/rels/z.rel", &data, &size), "missing file in archive");
    CHECK(!game_file_read("game_file_test", "none.bin", &data, &size), "missing file");
    remove("game_file_test/RELS.szs");
    remove("game_file_test/plain.bin");
    return 1;
}

int main(void) {
    if (!test_yaz0() || !test_archives())
        return 1;
    printf("game_file: ok\n");
    return 0;
}
