#include "app/config.h"

#include "toml.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int copy_string(char* out, size_t size, toml_datum_t value, const char* key) {
    if (!value.ok) {
        fprintf(stderr, "error: config needs %s\n", key);
        return 0;
    }
    const int fits = strlen(value.u.s) < size;
    if (fits)
        strcpy(out, value.u.s);
    else
        fprintf(stderr, "error: config %s is too long\n", key);
    free(value.u.s);
    return fits;
}

static int read_u32(toml_datum_t value, u32* out, const char* key) {
    if (!value.ok || value.u.i < 0 || value.u.i > 0xFFFFFFFFll) {
        fprintf(stderr, "error: config %s must be a 32-bit unsigned integer\n", key);
        return 0;
    }
    *out = (u32)value.u.i;
    return 1;
}

// A table's `<list> = [{ file, base, bss }, ...]`; base may be left out where
// the modules have a placement rule.
static int load_modules(const toml_table_t* table, const char* name, const char* list,
                        bool placed_by_rule, RecompModule* out, u32* count) {
    char key[64];
    const toml_array_t* modules = toml_array_in(table, list);
    const int module_count = modules ? toml_array_nelem(modules) : -1;
    if (module_count <= 0 || (u32)module_count > RECOMP_MAX_MODULES) {
        fprintf(stderr, "error: config %s.%s needs 1..%u modules\n", name, list, RECOMP_MAX_MODULES);
        return 0;
    }
    for (int i = 0; i < module_count; i++) {
        const toml_table_t* entry = toml_table_at(modules, i);
        RecompModule* module = &out[i];
        if (!entry)
            return 0;
        snprintf(key, sizeof(key), "%s.%s.file", name, list);
        if (!copy_string(module->file, sizeof(module->file), toml_string_in(entry, "file"), key))
            return 0;
        module->base = 0;
        snprintf(key, sizeof(key), "%s.%s.base", name, list);
        if ((!placed_by_rule || toml_key_exists(entry, "base")) &&
            !read_u32(toml_int_in(entry, "base"), &module->base, key))
            return 0;
        module->bss = 0;
        snprintf(key, sizeof(key), "%s.%s.bss", name, list);
        if (toml_key_exists(entry, "bss") && !read_u32(toml_int_in(entry, "bss"), &module->bss, key))
            return 0;
    }
    *count = (u32)module_count;
    return 1;
}

static int load_rso(RecompConfig* config, const toml_table_t* rso) {
    if (!copy_string(config->sel, sizeof(config->sel), toml_string_in(rso, "sel"), "rso.sel"))
        return 0;

    const toml_array_t* sections = toml_array_in(rso, "static_sections");
    const int section_count = sections ? toml_array_nelem(sections) : -1;
    if (section_count <= 0 || (u32)section_count > RECOMP_MAX_STATIC_SECTIONS) {
        fprintf(stderr, "error: config rso.static_sections needs 1..%u addresses\n",
                RECOMP_MAX_STATIC_SECTIONS);
        return 0;
    }
    for (int i = 0; i < section_count; i++) {
        if (!read_u32(toml_int_at(sections, i), &config->static_sections[i],
                      "rso.static_sections"))
            return 0;
    }
    config->static_section_count = (u32)section_count;
    const bool heap = toml_key_exists(rso, "overlay_heap_top");
    if (heap && !read_u32(toml_int_in(rso, "overlay_heap_top"), &config->rso_overlay_heap_top,
                          "rso.overlay_heap_top"))
        return 0;
    return load_modules(rso, "rso", "modules", false, config->rso_modules, &config->rso_module_count) &&
           (!toml_key_exists(rso, "overlays") ||
            load_modules(rso, "rso", "overlays", heap, config->rso_overlays, &config->rso_overlay_count));
}

// A file the config names: relative to the config file unless absolute.
static int beside_config(char* out, size_t size, const char* config_path, const char* file) {
    const bool absolute = file[0] == '/' || file[0] == '\\' || (file[0] && file[1] == ':');
    const char* slash = strrchr(config_path, '/');
    const char* backslash = strrchr(config_path, '\\');
    if (backslash > slash)
        slash = backslash;
    const int dir = absolute || !slash ? 0 : (int)(slash - config_path + 1);
    if (snprintf(out, size, "%.*s%s", dir, config_path, file) >= (int)size) {
        fprintf(stderr, "error: path too long: %s\n", file);
        return 0;
    }
    return 1;
}

static int load_defines(RecompConfig* config, const toml_table_t* defines) {
    const int count = toml_table_nkval(defines);
    if (count < 0 || (u32)count > RECOMP_MAX_DEFINES) {
        fprintf(stderr, "error: config [defines] holds up to %u constants\n", RECOMP_MAX_DEFINES);
        return 0;
    }
    for (int i = 0; i < count; i++) {
        const char* key = toml_key_in(defines, i);
        RecompDefine* define = &config->defines[config->define_count++];
        if (!key || strlen(key) >= sizeof(define->name)) {
            fprintf(stderr, "error: config [defines] needs short names\n");
            return 0;
        }
        strcpy(define->name, key);
        if (!read_u32(toml_int_in(defines, key), &define->value, key))
            return 0;
    }
    return 1;
}

int config_load(RecompConfig* config, const char* path) {
    memset(config, 0, sizeof(*config));
    FILE* file = fopen(path, "rb");
    if (!file) {
        fprintf(stderr, "error: can't open config '%s'\n", path);
        return 0;
    }
    char error[256];
    toml_table_t* root = toml_parse_file(file, error, sizeof(error));
    fclose(file);
    if (!root) {
        fprintf(stderr, "error: %s: %s\n", path, error);
        return 0;
    }

    int ok = 0;
    const toml_table_t* main = toml_table_in(root, "main");
    if (!main) {
        fprintf(stderr, "error: config needs a [main] table\n");
        goto done;
    }
    if (!copy_string(config->dol, sizeof(config->dol), toml_string_in(main, "dol"), "main.dol"))
        goto done;

    if (toml_key_exists(main, "natives")) {
        const toml_array_t* natives = toml_array_in(main, "natives");
        const int count = natives ? toml_array_nelem(natives) : -1;
        if (count < 0 || (u32)count > RECOMP_MAX_NATIVES_FILES) {
            fprintf(stderr, "error: config main.natives needs a list of up to %u signature files\n",
                    RECOMP_MAX_NATIVES_FILES);
            goto done;
        }
        for (int i = 0; i < count; i++) {
            char file[512];
            if (!copy_string(file, sizeof(file), toml_string_at(natives, i), "main.natives") ||
                !beside_config(config->natives[config->natives_count++], sizeof(config->natives[0]),
                               path, file))
                goto done;
        }
    }
    if (toml_key_exists(main, "sigdb")) {
        char file[512];
        if (!copy_string(file, sizeof(file), toml_string_in(main, "sigdb"), "main.sigdb") ||
            !beside_config(config->sigdb, sizeof(config->sigdb), path, file))
            goto done;
    }

    config->shards = 64;
    if (toml_key_exists(main, "shards") &&
        (!read_u32(toml_int_in(main, "shards"), &config->shards, "main.shards") ||
         config->shards == 0)) {
        fprintf(stderr, "error: config main.shards must be at least 1\n");
        goto done;
    }

    const toml_table_t* rso = toml_table_in(root, "rso");
    const toml_table_t* rel = toml_table_in(root, "rel");
    const toml_table_t* defines = toml_table_in(root, "defines");
    ok = (!rso || load_rso(config, rso)) &&
         (!rel || load_modules(rel, "rel", "modules", false, config->rel_modules, &config->rel_module_count)) &&
         (!defines || load_defines(config, defines));

done:
    toml_free(root);
    return ok;
}
