#ifndef DOLRECOMP_APP_RSO_LINK_H
#define DOLRECOMP_APP_RSO_LINK_H

#include "app/config.h"
#include "common/types.h"

// RSO modules linked at their known load addresses, the way the SDK's RSO
// linker does at run time, so their code can be recompiled ahead of time.
// The game still loads the RSOs itself; if one ever lands elsewhere, the
// runtime sees different bytes in the chunk hashes and leaves that code alone.

typedef struct {
    const char* module;  // the config's file name
    u32 address;
    u32 size;
    u8* bytes;  // relocated, owned
} RsoSection;

typedef struct {
    RsoSection* text;
    u32 count;
    RsoSection* data;  // the other loaded sections, where code pointers live
    u32 data_count;
    u32* exports;      // every exported address, the static module's too
    u32 export_count;
} RsoLink;

int rso_link(RsoLink* link, const RecompConfig* config, const char* game_dir);
void rso_link_free(RsoLink* link);

#endif
