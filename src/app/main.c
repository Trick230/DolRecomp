#include "app/config.h"
#include "app/module_link.h"
#include "app/natives.h"
#include "app/recompile.h"
#include "app/setup.h"
#include "frontend/container/dol.h"
#include "frontend/container/disc_extract.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(void) {
    printf("usage: dolrecomp <config.toml> <extracted game> <output dir>\n"
           "       dolrecomp extract <iso> <output dir>\n"
           "       dolrecomp sign <main.dol> <address> <size> ...\n"
           "                    natives' signature fields for the code there\n"
           "       dolrecomp rso-find <config.toml> <extracted game> <mem1 dump> [mem2 dump]\n"
           "                    where the config's RSO modules are loaded in a RAM dump\n"
           "       dolrecomp --setup    download Wiimms ISO Tools for extract\n");
}

// Prints "<size> <first> <hash>" for each function, as a .sigs line takes them.
static int sign(int argc, char** argv) {
    if (argc < 4 || argc % 2 != 0) {
        usage();
        return 1;
    }
    DOLFile dol;
    if (!dol_load(&dol, argv[1]))
        return 1;
    LoadedCodeSection sections[DOL_NUM_TEXT];
    u32 section_count = 0;
    for (u32 i = 0; i < DOL_NUM_TEXT; i++) {
        const u8* data = dol.header.text_sizes[i] ? dol_get_text_section(&dol, (int)i) : NULL;
        if (data)
            sections[section_count++] = (LoadedCodeSection){
                "text", NULL, data, i, dol.header.text_offsets[i], dol.header.text_addresses[i],
                dol.header.text_sizes[i], EMBEDDED_DATA_DOL};
    }
    int ok = 1;
    for (int i = 2; i + 1 < argc; i += 2) {
        const u32 address = (u32)strtoul(argv[i], NULL, 16);
        const u32 size = (u32)strtoul(argv[i + 1], NULL, 16);
        u32 first = 0;
        u64 hash = 0;
        if (natives_sign(sections, section_count, address, size, &first, &hash)) {
            printf("%08X %X %08X %016llX\n", address, size, first, (unsigned long long)hash);
        } else {
            fprintf(stderr, "error: no code at %08X..+%X\n", address, size);
            ok = 0;
        }
    }
    dol_free(&dol);
    return ok ? 0 : 1;
}

static u8* read_dump(const char* path, u32* size) {
    FILE* file = fopen(path, "rb");
    if (!file) {
        fprintf(stderr, "error: can't open %s\n", path);
        return NULL;
    }
    fseek(file, 0, SEEK_END);
    const long length = ftell(file);
    fseek(file, 0, SEEK_SET);
    u8* bytes = length > 0 ? (u8*)malloc((size_t)length) : NULL;
    if (bytes && fread(bytes, 1, (size_t)length, file) != (size_t)length) {
        free(bytes);
        bytes = NULL;
    }
    fclose(file);
    if (!bytes)
        fprintf(stderr, "error: can't read %s\n", path);
    *size = (u32)length;
    return bytes;
}

static int rso_find_main(int argc, char** argv) {
    if (argc != 4 && argc != 5) {
        usage();
        return 1;
    }
    RecompConfig config;
    if (!config_load(&config, argv[1]))
        return 1;
    u32 mem1_size = 0, mem2_size = 0;
    u8* mem1 = read_dump(argv[3], &mem1_size);
    u8* mem2 = argc == 5 ? read_dump(argv[4], &mem2_size) : NULL;
    const int ok = mem1 && (argc == 4 || mem2) &&
                   rso_find(&config, argv[2], mem1, mem1_size, mem2, mem2_size);
    free(mem1);
    free(mem2);
    return ok ? 0 : 1;
}

int main(int argc, char** argv) {
    if (argc > 1 && strcmp(argv[1], "extract") == 0)
        return disc_extract_main(argc - 1, argv + 1);
    if (argc > 1 && strcmp(argv[1], "sign") == 0)
        return sign(argc - 1, argv + 1);
    if (argc > 1 && strcmp(argv[1], "rso-find") == 0)
        return rso_find_main(argc - 1, argv + 1);
    if (argc == 2 && strcmp(argv[1], "--setup") == 0)
        return run_setup() ? 0 : 1;
    if (argc != 4) {
        usage();
        return argc == 2 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) ? 0 : 1;
    }

    RecompConfig config;
    if (!config_load(&config, argv[1]))
        return 1;
    return recompile(&config, argv[2], argv[3]) ? 0 : 1;
}
