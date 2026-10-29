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

`runtime/core` holds the CPU state, memory helpers and an interpreter that
runs whatever the recompiled code does not cover, and the glue a host runtime
uses to find the generated functions (`guest_module.h`). `runtime/sdk` holds
natives any game can list: the Metrowerks register save/restore helpers and
`memcpy`, and the Revolution SDK's context, interrupt, mutex, cache and matrix
functions, with their signatures (`mwcc.sigs`, `rvl.sigs`, built into the
recompiler as `@mwcc` and `@rvl`). What differs between games on the same SDK
(where the linker put a function or a global, small-data offsets) is read off
the matched code by `define` lines there, which the recompiler writes to
`ppc_natives.h`. The format is in `src/app/natives.h`.

| Library | What a game's recompiled library gets from it |
|---|---|
| `dr_runtime` | CPU state, memory helpers, the interpreter |
| `dr_guest` | `guest_module.c`, compiled with the generated code |
| `dr_natives_mwcc` | `_savegpr_N`/`_restgpr_N` and the FPR forms, `memcpy`, `__fill_mem` |
| `dr_natives_rvl` | `OSSaveContext`/`OSLoadContext`, `OSDisableInterrupts` and friends, `OSLockMutex` and friends, `DC*Range`, `ICInvalidateRange`, `PSMTX*`/`PSVEC*` |
| `dr_host` | The host runtime's CPU layer (`runtime/host`), for the host executable |

The interface libraries compile their sources inside the game's library with
its flags, so the natives inline across the generated code as if they were the
game's own.

`runtime/host` is the CPU layer of a host runtime, in C++20 (`dr_host`,
compiled into the host executable):

- `cpu.hpp`: the dispatcher. It runs a generated function where one covers
  the pc and still holds the code it was built from (an FNV-1a hash checked on
  first entry, dropped again by icache invalidation: `icbi`, a data-cache op on
  a code line or a device's DMA), through a direct-mapped
  cache in front of a per-line function table, and the interpreter
  (`interpreter.cpp`, with the generated code's semantics and cycle costs)
  everywhere else. It owns the SPRs, timebase and decrementer, the gather
  pipe, the locked L1 cache and its DMA, and safepoints that run due events
  inside generated loops. Code run with translation off (the OS's BAT setup)
  runs as its cached mirror's generated code.
- Self-modifying code: a function whose bytes no longer match its hash runs
  on the interpreter; one that matches again returns to generated code. An
  invalidation that retires code the guest has run leaves every caller to the
  dispatcher, so none resumes over code that changed.
- Interrupts and FP-unavailable save the OSContext in host code and start in
  the OS's handler; `CallGuest` runs a guest function to its return from host
  code; guest threads run on host fibers (Windows), so a thread switch inside
  generated code parks the host stack instead of unwinding it. These follow
  the Dolphin/Revolution OS, the same on GameCube and Wii.
- `scheduler.hpp`: guest time in CPU cycles, events in a heap, slices that end
  at the next one.
- `memory.hpp`: the 32-bit effective space as one host reservation, with MEM1,
  MEM2 and the locked cache as views of one section at the addresses the OS's
  BATs give them, so generated code reaches guest memory as base + address.
- `state_io.hpp`: the savestate stream.

The host derives its machine from `dolrecomp::CpuHost` and defines its six
members (hardware loads and stores, gather-pipe bursts, what runs after
events and before idling); they bind at link time. A GameCube host defines
`DOLRECOMP_GAMECUBE` (no MEM2, Gekko's 486 MHz).

## Use

```sh
dolrecomp <config.toml> <extracted game> <output dir>
dolrecomp extract <iso> <output dir>
dolrecomp rso-find <config.toml> <extracted game> <mem1 dump> [mem2 dump]
dolrecomp --setup     # download Wiimms ISO Tools for extract
```

The config names the DOL, the natives' signature sets and the modules with
the addresses they load at (and their bss): RSO modules (`[rso]`), linked
against the static module's exports, and REL modules (`[rel]`), linked
against the DOL and each other as `OSLink` does, with imports of a module not
listed left as in the file. Their code is recompiled with the DOL's.
RSO overlays (`[rso] overlays`) are modules the game loads at times into
memory other code uses at other times, like one stage after another: each
links against the resident modules alone and gets functions of its own
(`func_<module>_<address>`) next to the ones it overlaps; the runtime runs, of
the functions over a pc, the one whose hash matches guest RAM. An overlay
can leave its address to `overlay_heap_top`: the SDK's expanded heap hands
out its file and bss from the tail of the free space ending there.
`src/app/config.h` shows the format. DualizeRecomp's is
`DualizeRecompLib/config/R7EE8P.toml`. The output directory gets the shards,
`ppc_recomp.h`, `ppc_tables.inc`, `ppc_natives.h` and `natives.txt` (what the
signatures found).

`dolrecomp rso-find` reads where the config's RSO modules are loaded in a
RAM dump (MEM1 and MEM2 as Dolphin dumps them) and prints their config lines,
checked against the config, with the heap top an overlay implies.

`dolrecomp sign <main.dol> <address> <size>` prints a new native's signature
fields; `tools/find_sdk_symbols.py` names a DOL's SDK functions from Dolphin's
signature database, to find where a game's SDK functions are.

## Build

CMake 3.16+ and a C11 compiler (Clang or GCC); a C++20 compiler for
`runtime/host`. DualizeRecomp builds it as a host tool; on its own:

```sh
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

## Notes

- Licensed as upstream (`LICENSE`).
