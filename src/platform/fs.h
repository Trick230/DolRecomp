#ifndef DOLRECOMP_PLATFORM_FS_H
#define DOLRECOMP_PLATFORM_FS_H

#include <stddef.h>
#include <stdio.h>

int make_dir(const char* path);
int make_dir_tree(const char* path);
const char* path_basename(const char* path);
int path_dirname(const char* path, char* dir, size_t dir_size);
int is_path_sep(char ch);
int join_path(char* out, size_t out_size, const char* dir, const char* name);
int file_exists(const char* path);
int path_is_directory(const char* path);

// A generated file, written beside its path and moved over it on close only
// when the contents changed, so what depends on it doesn't rebuild for an
// identical result.
typedef struct {
    FILE* stream;
    char path[1200];
    char temp[1210];
} OutputFile;

int output_open(OutputFile* out, const char* path, const char* mode);
int output_close(OutputFile* out);
void output_discard(OutputFile* out);

#endif
