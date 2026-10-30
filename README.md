# DolRecomp (DualizeRecomp fork)

A static recompiler for GameCube and Wii PowerPC (Gekko, Broadway) code: it
translates a game's `main.dol` and its RSO and REL modules to C, ahead of
time, for a host runtime to compile and run. This fork serves
[DualizeRecomp](https://github.com/Trick230/DualizeRecomp) and is built as
part of it; upstream is
[ExpansionPak/DolRecomp](https://github.com/ExpansionPak/DolRecomp).

## What it emits

- One C function per guest function (`func_XXXXXXXX(CPUState*)`), split into
  a fixed number of shards so the build can list them up front.
- Guest RAM accesses go straight to a flat host mapping; MMIO addresses take a
  slow path into the runtime.
- Calls between recompiled functions are direct C calls while no hook is
  active; anything else returns to the runtime's dispatcher with `ctx->pc`.
- Cycle counts per block against `ctx->downcount`, with safepoints on loop
  back-edges and dispatch so the runtime can run timers and interrupts.
- Natives: SDK functions the runtime implements on the host, found by
  signature: their code with what the linker decides masked (calls out,
  `lis` halves, small-data offsets). Struct offsets stay, so a different build
  of the function stays recompiled.
- Paired singles, `psq_l`/`psq_st` and exact floating-point corner cases.

DolRecomp is the recompiler and what its output needs to compile; the
runtime that runs it (memory, the dispatcher, the hardware, the natives'
implementations) is the game's, the way XenonRecomp leaves it to
UnleashedRecomp.

| Library | What it is |
|---|---|
| `dr_core` | `core/`: CPU state and memory access, the bit-exact floating point and paired singles, exceptions and the instructions the generated code calls out for |
| `dr_guest` | `core/guest_module.c`, the glue to the generated tables, compiled with the generated code |

The runtime provides what the generated code calls into (`core/guest_module.h`):
`dolrecomp_lookup` (the function holding a pc, or none), `dolrecomp_safepoint`
(a loop ran out of cycles) and CPUState's hooks for MMIO, SPRs, cache ops and
instructions left to it. `g_guest_module` hands it the functions, their
ranges and the hash of the text each was built from, the resident code's
then each overlay's.

Natives are SDK functions the runtime implements on the host. Their
`.sigs` files sit with those implementations; the recompiler finds them by
signature and reads what differs between games on the same SDK (where the
linker put a function or a global, small-data offsets) off the matched code
with `define` lines, which it writes to `ppc_natives.h`. The format is in
`src/app/natives.h`.

## Use

```sh
dolrecomp <config.toml> <extracted game> <output dir>
dolrecomp extract <iso> <output dir>
dolrecomp rso-find <config.toml> <extracted game> <mem1 dump> [mem2 dump]
dolrecomp --setup     # download Wiimms ISO Tools for extract
```

The config names the DOL, the console's CPU (`cpu = "gekko"` or
`"broadway"`), the natives' signature sets and the modules with
the addresses they load at (and their bss): RSO modules (`[rso]`), linked
against the static module's exports, and REL modules (`[rel]`), linked
against the DOL and each other as `OSLink` does, with imports of a module not
listed left as in the file. Their code is recompiled with the DOL's.
A module's `file` may run through archives, as in
`files/RELS.szs/rels/stage.rel`: RARC and U8 archives, and Yaz0 compression
of the archive or the file, are read as the game reads them.
RSO overlays (`[rso] overlays`) are modules the game loads at times into
memory other code uses at other times, like one stage after another: each
links against the resident modules alone and gets functions of its own
(`func_<module>_<address>`) next to the ones it overlaps; the runtime runs, of
the functions over a pc, the one whose hash matches guest RAM. An overlay
can leave its address to `overlay_heap_top`: the SDK's expanded heap hands
out its file and bss from the tail of the free space ending there. That
address is only where it is compiled for: its code runs wherever the game
links it. Addresses in its own code come out as the compiled ones plus
`dolrecomp_overlay_shift[k]`, and each `lis` of an address in the module
adds the shift of the module or of its bss. Its function hashes leave out
the instruction bits its placement changes (`GuestOverlay.relocs`). The
runtime reads where the game linked each overlay and sets the shifts.
Constants of the game's own its runtime compiles against (`[defines]`, such
as an idle loop's address) go to `ppc_natives.h` with the natives' defines.
`src/app/config.h` shows the format. DualizeRecomp's is
`DualizeRecompLib/config/R7EE8P.toml`. The output directory gets the shards,
`ppc_recomp.h`, `ppc_tables.inc`, `ppc_natives.h` and `natives.txt` (what the
signatures found).

`dolrecomp rso-find` reads where the config's RSO modules are loaded in a
RAM dump (MEM1 and MEM2 as Dolphin dumps them) and prints their config lines,
checked against the config, with the heap top an overlay implies.

`dolrecomp sign <main.dol> <address> <size>` prints a new native's signature
fields.

With `sigdb` in `[main]` pointing at Dolphin's signature database
(`Sys/totaldb.dsy` in a Dolphin install; GPL-2.0-or-later, so it is read from
there, not shipped here), the output directory also gets `sdk_names.txt`, the
SDK functions it names in the resident code, and `sigs_candidates.txt`: for
each signature the game didn't match whose SDK name the database found, the
`.sigs` line the code there takes. That is where bringing natives to a second
game starts; the database only checksums opcodes, so a candidate is a
function with the right name and shape, to check against the native's SDK
build before use. Code whose shape occurs twice in the game names nothing.

## Build

CMake 3.16+ and a C11 compiler (Clang or GCC). DualizeRecomp builds it as a
host tool; on its own:

```sh
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

## Notes

- Licensed as upstream (`LICENSE`).
