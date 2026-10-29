# DolRecomp (DualizeRecomp fork)

A static recompiler for Wii PowerPC (Broadway) code: it translates a game's
`main.dol` and its RSO modules to C, ahead of time, for a host runtime to
compile and run. This fork serves
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
- Natives: SDK functions the runtime implements on the host, each pinned to
  the guest bytes it replaces by an FNV-1a hash, so a different build of the
  function stays recompiled.
- Paired singles, `psq_l`/`psq_st` and exact floating-point corner cases.

`runtime/core` holds the CPU state, memory helpers and an interpreter that
runs whatever the recompiled code does not cover, and the glue a host runtime
uses to find the generated functions (`guest_module.h`). `runtime/sdk` holds
natives any game can list: the Metrowerks register save/restore helpers and
`memcpy`, and the Revolution SDK's context, interrupt, mutex, cache and matrix
functions. What differs between games on the same SDK (where the linker put a
function or a global, small-data offsets) comes from `define` lines in the
game's natives list, which the recompiler writes to `ppc_natives.h`.

| Library | What a game's recompiled library gets from it |
|---|---|
| `dr_runtime` | CPU state, memory helpers, the interpreter |
| `dr_guest` | `guest_module.c`, compiled with the generated code |
| `dr_natives_mwcc` | `_savegpr_N`/`_restgpr_N` and the FPR forms, `memcpy`, `__fill_mem` |
| `dr_natives_rvl` | `OSSaveContext`/`OSLoadContext`, `OSDisableInterrupts` and friends, `OSLockMutex` and friends, `DC*Range`, `PSMTX*`/`PSVEC*` |

The interface libraries compile their sources inside the game's library with
its flags, so the natives inline across the generated code as if they were the
game's own.

## Use

```sh
dolrecomp <config.toml> <extracted game> <output dir>
dolrecomp extract <iso> <output dir>
dolrecomp --setup     # download Wiimms ISO Tools for extract
```

The config names the DOL, the natives list and the RSO modules with the
addresses they load at; DualizeRecomp's is
`DualizeRecompLib/config/R7EE8P.toml`. The output directory gets the shards,
`ppc_recomp.h`, `ppc_tables.inc` and `ppc_natives.h`.

`tools/natives_hash.py` prints natives-list entries (address, size, code
hash) for functions of a DOL; `tools/find_sdk_symbols.py` names a DOL's SDK
functions from Dolphin's signature database, to find a new game's natives.

## Build

CMake 3.16+ and a C11 compiler (Clang or GCC). DualizeRecomp builds it as a
host tool; on its own:

```sh
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

## Notes

- Self-modifying code is not handled; such functions need natives or patches.
- Licensed as upstream (`LICENSE`).
