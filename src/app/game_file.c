#include "app/game_file.h"

#include "common/types.h"
#include "platform/fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int read_whole(const char* path, u8** data, u32* size) {
    FILE* file = fopen(path, "rb");
    if (!file)
        return 0;
    long length = -1;
    if (fseek(file, 0, SEEK_END) == 0)
        length = ftell(file);
    u8* bytes = length >= 0 && length < 0x7FFFFFFF && fseek(file, 0, SEEK_SET) == 0
                    ? (u8*)malloc(length ? (size_t)length : 1u)
                    : NULL;
    const int ok = bytes && fread(bytes, 1, (size_t)length, file) == (size_t)length;
    fclose(file);
    if (!ok) {
        free(bytes);
        return 0;
    }
    *data = bytes;
    *size = (u32)length;
    return 1;
}

int game_file_unyaz0(u8** data, u32* size) {
    const u8* in = *data;
    if (*size < 16 || memcmp(in, "Yaz0", 4) != 0)
        return 1;
    const u32 out_size = read_be32(in + 4);
    u8* out = (u8*)malloc(out_size ? out_size : 1u);
    if (!out)
        return 0;
    u32 src = 16, dst = 0;
    while (dst < out_size) {
        if (src >= *size)
            goto bad;
        const u8 code = in[src++];
        for (int bit = 7; bit >= 0 && dst < out_size; bit--) {
            if (code & (1u << bit)) {
                if (src >= *size)
                    goto bad;
                out[dst++] = in[src++];
                continue;
            }
            if (src + 2 > *size)
                goto bad;
            const u8 b1 = in[src], b2 = in[src + 1];
            src += 2;
            const u32 distance = ((u32)(b1 & 0x0Fu) << 8 | b2) + 1u;
            u32 count = b1 >> 4;
            if (count == 0) {
                if (src >= *size)
                    goto bad;
                count = in[src++] + 0x12u;
            } else {
                count += 2u;
            }
            if (distance > dst || count > out_size - dst)
                goto bad;
            for (u32 i = 0; i < count; i++, dst++)
                out[dst] = out[dst - distance];
        }
    }
    free(*data);
    *data = out;
    *size = out_size;
    return 1;
bad:
    free(out);
    fprintf(stderr, "error: malformed Yaz0 data\n");
    return 0;
}

static int fits(u32 offset, u32 length, u32 size) {
    return offset <= size && length <= size - offset;
}

// The next component of a path: its length, and where the one after starts.
static size_t component(const char* path, const char** next) {
    size_t n = 0;
    while (path[n] && !is_path_sep(path[n]))
        n++;
    *next = path[n] ? path + n + 1 : path + n;
    return n;
}

static int name_is(const u8* archive, u32 size, u32 offset, const char* name, size_t length) {
    return fits(offset, (u32)length + 1u, size) && memcmp(archive + offset, name, length) == 0 &&
           archive[offset + length] == 0;
}

static int copy_out(const u8* bytes, u32 length, u8** data, u32* size) {
    u8* copy = (u8*)malloc(length ? length : 1u);
    if (!copy)
        return -1;
    memcpy(copy, bytes, length);
    *data = copy;
    *size = length;
    return 1;
}

// RARC: a header, then nodes (directories) listing entries (files and
// subdirectories); the root node is the first.
static int in_rarc(const u8* a, u32 size, const char* path, u8** data, u32* size_out) {
    if (size < 0x40)
        return -1;
    const u32 data_base = 0x20u + read_be32(a + 0x0C);
    const u32 node_count = read_be32(a + 0x20), nodes = 0x20u + read_be32(a + 0x24);
    const u32 entry_count = read_be32(a + 0x28), entries = 0x20u + read_be32(a + 0x2C);
    const u32 strings = 0x20u + read_be32(a + 0x34);
    if (!node_count || !fits(nodes, node_count * 0x10u, size) || !fits(entries, entry_count * 0x14u, size))
        return -1;
    u32 node = 0;
    const char* next;
    size_t length = component(path, &next);
    // The root directory's own name may lead the path.
    if (name_is(a, size, strings + read_be32(a + nodes + 4), path, length) && *next)
        length = component(path = next, &next);
    for (;;) {
        const u8* n = a + nodes + node * 0x10u;
        const u32 first = read_be32(n + 0x0C), count = read_be16(n + 0x0A);
        if (first > entry_count || count > entry_count - first)
            return -1;
        u32 found = entry_count;
        for (u32 i = first; i < first + count && found == entry_count; i++) {
            if (name_is(a, size, strings + read_be16(a + entries + i * 0x14u + 6), path, length))
                found = i;
        }
        if (found == entry_count)
            return 0;
        const u8* e = a + entries + found * 0x14u;
        const int directory = (e[4] & 0x02u) != 0;
        const u32 offset = read_be32(e + 8), length_in = read_be32(e + 0x0C);
        if (!*next) {
            if (directory || !fits(data_base + offset, length_in, size))
                return directory ? 0 : -1;
            return copy_out(a + data_base + offset, length_in, data, size_out);
        }
        if (!directory || offset >= node_count)
            return 0;
        node = offset;
        length = component(path = next, &next);
    }
}

// U8: a flat node table; a directory's children run up to the index in its
// size field.
static int in_u8(const u8* a, u32 size, const char* path, u8** data, u32* size_out) {
    if (size < 0x20)
        return -1;
    const u32 root = read_be32(a + 4);
    if (!fits(root, 12, size))
        return -1;
    const u32 count = read_be32(a + root + 8);
    if (!count || !fits(root, count * 12u, size))
        return -1;
    const u32 strings = root + count * 12u;
    while (path[0] == '.' && is_path_sep(path[1]))
        path += 2;
    u32 dir = 0;
    const char* next;
    size_t length = component(path, &next);
    for (;;) {
        const u32 end = read_be32(a + root + dir * 12u + 8);
        u32 i = dir + 1u, found = 0;
        while (i < end && i < count) {
            const u8* n = a + root + i * 12u;
            if (name_is(a, size, strings + (read_be32(n) & 0x00FFFFFFu), path, length)) {
                found = i;
                break;
            }
            i = n[0] == 1 ? read_be32(n + 8) : i + 1u;
        }
        if (!found)
            return 0;
        const u8* n = a + root + found * 12u;
        if (!*next) {
            if (n[0] != 0 || !fits(read_be32(n + 4), read_be32(n + 8), size))
                return n[0] != 0 ? 0 : -1;
            return copy_out(a + read_be32(n + 4), read_be32(n + 8), data, size_out);
        }
        if (n[0] != 1)
            return 0;
        dir = found;
        length = component(path = next, &next);
    }
}

int game_file_in_archive(const u8* archive, u32 archive_size, const char* path, u8** data, u32* size) {
    if (archive_size >= 4 && memcmp(archive, "RARC", 4) == 0)
        return in_rarc(archive, archive_size, path, data, size);
    if (archive_size >= 4 && read_be32(archive) == 0x55AA382Du)
        return in_u8(archive, archive_size, path, data, size);
    return -1;
}

// The rest of a path inside an archive, through any archives nested in it.
static int lookup(u8* archive, u32 archive_size, const char* rest, u8** data, u32* size) {
    int found = game_file_in_archive(archive, archive_size, rest, data, size);
    for (const char* sep = rest; found == 0 && *sep; sep++) {
        if (!is_path_sep(*sep))
            continue;
        char outer[1024];
        if ((size_t)(sep - rest) >= sizeof(outer))
            break;
        memcpy(outer, rest, (size_t)(sep - rest));
        outer[sep - rest] = 0;
        u8* inner = NULL;
        u32 inner_size = 0;
        if (game_file_in_archive(archive, archive_size, outer, &inner, &inner_size) != 1)
            continue;
        found = game_file_unyaz0(&inner, &inner_size) ? lookup(inner, inner_size, sep + 1, data, size) : -1;
        free(inner);
    }
    return found == 1 && game_file_unyaz0(data, size) ? 1 : found == 1 ? -1 : found;
}

int game_file_read(const char* game_dir, const char* path, u8** data, u32* size) {
    char full[1200];
    if (!join_path(full, sizeof(full), game_dir, path))
        return 0;
    if (file_exists(full) && !path_is_directory(full))
        return read_whole(full, data, size) && game_file_unyaz0(data, size);
    // The longest leading part that is a file is an archive holding the rest.
    for (size_t i = strlen(full); i > 0; i--) {
        if (!is_path_sep(full[i - 1]))
            continue;
        full[i - 1] = 0;
        if (!file_exists(full) || path_is_directory(full)) {
            full[i - 1] = '/';
            continue;
        }
        u8* archive = NULL;
        u32 archive_size = 0;
        const int found = read_whole(full, &archive, &archive_size) &&
                                  game_file_unyaz0(&archive, &archive_size)
                              ? lookup(archive, archive_size, full + i, data, size)
                              : -1;
        free(archive);
        return found == 1;
    }
    return 0;
}
