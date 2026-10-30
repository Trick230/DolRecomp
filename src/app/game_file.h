#ifndef DOLRECOMP_APP_GAME_FILE_H
#define DOLRECOMP_APP_GAME_FILE_H

#include "common/types.h"

// Reads a file of the extracted game the way the game reaches it: a path may
// run through archives ("files/RELS.arc/rels/d_a_obj.rel": a RARC or U8
// archive, its root directory's name optional), and any file or archive may
// be Yaz0-compressed. 1 with a malloc'd buffer the caller frees.
int game_file_read(const char* game_dir, const char* path, u8** data, u32* size);

// The same over bytes already read: decompresses a Yaz0 stream in place of
// the buffer (taking ownership), or leaves the rest alone.
int game_file_unyaz0(u8** data, u32* size);

// A file inside a RARC or U8 archive: 1 with a malloc'd copy, 0 not there, -1
// not an archive or malformed.
int game_file_in_archive(const u8* archive, u32 archive_size, const char* path, u8** data, u32* size);

#endif
