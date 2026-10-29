# Writes OUT, a C source returning the text of each SETS entry's
# runtime/sdk/<name>.sigs (DIR): natives_builtin_signatures("<name>").
set(source [=[// Generated from runtime/sdk/*.sigs by cmake/embed_signatures.cmake.

#include "app/natives.h"

#include <string.h>

const char* natives_builtin_signatures(const char* name) {
]=])
foreach(set IN LISTS SETS)
    file(READ "${DIR}/${set}.sigs" text)
    string(REPLACE "\r" "" text "${text}")
    string(REPLACE [=[\]=] [=[\\]=] text "${text}")
    string(REPLACE [=["]=] [=[\"]=] text "${text}")
    string(REPLACE "\n" [=[\n"
           "]=] text "${text}")
    string(APPEND source "    if (strcmp(name, \"${set}\") == 0)\n        return \"${text}\";\n")
endforeach()
string(APPEND source "    return NULL;\n}\n")
file(WRITE "${OUT}.tmp" "${source}")
file(COPY_FILE "${OUT}.tmp" "${OUT}" ONLY_IF_DIFFERENT)
file(REMOVE "${OUT}.tmp")
