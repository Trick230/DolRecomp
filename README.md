# DolRecomp (DualizeRecomp fork)

A static recompiler for GameCube and Wii games. DolRecomp reads a game's
`main.dol` and its loadable modules (RSO and REL), finds every function, and
translates the PowerPC code (Gekko or Broadway) to C ahead of time. A host
runtime compiles that C with its own code and runs the game natively.

This is the fork used by [DualizeRecomp](https://github.com/Trick230/DualizeRecomp),
which builds it as a host tool. Upstream is
[ExpansionPak/DolRecomp](https://github.com/ExpansionPak/DolRecomp).

DolRecomp is the recompiler and the ABI its output compiles against, nothing
more: memory, the hardware, the OS and the dispatcher belong to the runtime
you write for the game, the way XenonRecomp leaves them to UnleashedRecomp.

- [Features](#features)
- [Building](#building)
- [Quick start](#quick-start)
- [Commands](#commands)
- [The config file](#the-config-file)
- [Modules](#modules)
- [Natives](#natives)
- [Output](#output)
- [Writing a runtime](#writing-a-runtime)
- [Limitations](#limitations)
- [Tests](#tests)
- [License](#license)

## Features

**Code generation**

- One C function per guest function (`func_XXXXXXXX(CPUState*)`), with an
  entry switch for every point other code can enter it at, written to a fixed
  number of shards so a build system can list the sources before the
  recompiler runs.
- Guest memory accesses go straight to the host's mapping of MEM1 and MEM2;
  anything else (MMIO, the gather pipe, the locked cache) takes a slow path
  into the runtime.
- Calls between recompiled functions are direct C calls, guarded by a flag
  per function that the runtime clears when that function's code changes.
- Guest time: each basic block subtracts its cycle cost from a downcount, and
  loops call a safepoint when it runs out, so the runtime can run timers and
  interrupts at exact guest times.
- Bit-exact floating point and paired singles, including `psq_l`/`psq_st`
  quantization, the FPSCR flags and the console's rounding. FPSCR[FPRF] is
  only computed when something reads it.
- Every guest function is hashed. The runtime checks guest RAM against the
  hash before running a function, so self-modifying code, overlays and
  anything the game loads over recompiled code fall back safely.

**Loading games**

- DOLs, RSO modules (Wii) and REL modules (GameCube and Wii), linked inside
  the recompiler the way the game's own linker links them at run time.
- Modules read straight from the extracted disc, including from inside RARC
  and U8 archives and Yaz0-compressed files.
- Position-independent modules: a module compiled for one address runs
  wherever the game loads it. A config needs no addresses and no RAM dumps.
- Overlays: modules the game loads one after another into the same memory
  (stages, menus) each get functions of their own; the runtime runs whichever
  matches guest RAM.
- Disc extraction from an ISO through Wiimms ISO Tools.

**Natives**

- SDK functions the runtime implements in host code are found by signature,
  not by address, so one signature file serves every game built with the same
  SDK version.
- Signatures can read game-specific addresses (globals, small-data offsets,
  call targets) off the matched code and hand them to the runtime as defines.
- With Dolphin's signature database, DolRecomp names the SDK functions in a
  game and proposes signatures for it.

## Building

You need CMake 3.16 or newer and Clang or GCC (the generated code uses their
builtins). On Windows, MSYS2's UCRT64 environment works.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

This builds the `dolrecomp` executable and two libraries for runtimes to link:

| Library | Contents |
|---|---|
| `dr_core` | `core/`: the CPU state, memory access, floating point and paired singles, exceptions, and the instructions generated code calls out for |
| `dr_guest` | `core/guest_module.c`: the glue to the generated tables; compile it together with the generated sources |

## Quick start

1. Extract the game from your own disc image:

   ```sh
   dolrecomp --setup                      # once: downloads Wiimms ISO Tools
   dolrecomp extract game.iso extracted/game
   ```

2. Write a config naming the DOL, the console and the modules (see
   [The config file](#the-config-file)). For a Wii game with no modules this
   is enough:

   ```toml
   [main]
   dol = "sys/main.dol"
   cpu = "broadway"
   ```

3. Recompile:

   ```sh
   dolrecomp game.toml extracted/game out/
   ```

4. Compile the shards in `out/` (they include the chunks) together with
   `core/guest_module.c` and your runtime, and link `dr_core`.

## Commands

| Command | What it does |
|---|---|
| `dolrecomp <config.toml> <extracted game> <output dir>` | Recompiles the game the config describes |
| `dolrecomp extract <iso> <output dir>` | Extracts a disc image (needs `--setup` first) |
| `dolrecomp --setup` | Downloads Wiimms ISO Tools into `extern/wit` |
| `dolrecomp sign <main.dol> <address> <size> ...` | Prints the signature fields of the code at each address, for writing natives |
| `dolrecomp rso-find <config.toml> <extracted game> <mem1 dump> [mem2 dump]` | Finds where the config's RSO modules are loaded in a RAM dump and prints their config lines |

Addresses and sizes are hexadecimal. RAM dumps are raw MEM1 and MEM2 images,
as Dolphin writes them.

## The config file

A config is a TOML file. Paths in `[main]`, `[rso]` and `[rel]` are relative
to the extracted game, except `natives` and `sigdb`, which are relative to the
config file. Any path may be absolute.

```toml
[main]
dol = "sys/main.dol"
cpu = "broadway"                          # or "gekko" for GameCube
natives = ["mwcc.sigs", "rvl.sigs"]       # optional
sigdb = "C:/Dolphin/Sys/totaldb.dsy"      # optional
shards = 64                               # optional

[rso]                                     # Wii games with RSO modules
sel = "files/rso/Game.sel"
modules = [
    { file = "files/rso/common.rso" },
    { file = "files/rso/menu.rso" },
]
overlays = [
    { file = "files/rso/stage01.rso" },
    { file = "files/rso/stage02.rso" },
]

[rel]                                     # games with REL modules
modules = [{ file = "files/RELS.arc/rels/d_a_common.rel" }]
overlays = [{ file = "files/RELS.szs/rels/d_stage01.rel" }]

[defines]                                 # optional
IDLE_PC = 0x8036623C
```

**`[main]`**

| Key | Meaning |
|---|---|
| `dol` | The main executable |
| `cpu` | `"gekko"` (GameCube) or `"broadway"` (Wii, the default). The generated header defines `DOLRECOMP_CPU_GEKKO` or `DOLRECOMP_CPU_BROADWAY` |
| `natives` | Signature files of the SDK functions your runtime implements (see [Natives](#natives)) |
| `sigdb` | Dolphin's signature database, to name SDK functions and propose signatures |
| `shards` | How many C sources the output is split into (default 64) |

**`[rso]`**

| Key | Meaning |
|---|---|
| `sel` | The static module: the symbol table RSO modules link against |
| `static_sections` | The address of each of the static module's sections, by section index. Optional for a DOL of the usual layout (2 text and 8 data sections), from which it is read |
| `modules` | Resident modules: loaded once and kept |
| `overlays` | Modules loaded at times into memory other modules use |
| `overlay_heap_top` | Optional: compile overlays with no `base` for where the SDK's expanded heap would place them below this address (see `src/app/config.h`) |

**`[rel]`**

| Key | Meaning |
|---|---|
| `modules` | Resident REL modules |
| `overlays` | REL modules loaded at times into memory other modules use |

**Module entries.** Each module is `{ file = "...", base = 0x..., bss = 0x... }`.
`file` may go through archives: `files/RELS.arc/rels/d_a_obj.rel` reads
`rels/d_a_obj.rel` from inside the RARC or U8 archive `files/RELS.arc` (a
RARC's root directory name may be left out). Any archive or file may be
Yaz0-compressed, and archives may nest. `base` and `bss` are optional (see
[Placement](#placement)).

**`[defines]`** are constants of the game that your runtime compiles against,
such as the OS idle loop's address. They go to `ppc_natives.h` next to the
defines the natives read off the code.

## Modules

### Residents and overlays

DolRecomp links every module listed in the config the way the game's linker
does: RSO modules against the static module's exports and each other, REL
modules against the DOL and each other. Imports of a module that isn't listed
stay as they are in the file, as the game's linker leaves them until that
module loads.

A resident module is linked against everything else that is resident. An
overlay is linked against the resident modules alone, as it finds them when
it loads, and gets functions of its own, named `func_<module>_<address>`. Its
functions can overlap other overlays' and the resident code's; at run time,
of the functions over a pc, the one whose hash matches guest RAM runs.

### Placement

Modules are compiled for one address and run wherever the game loads them:

- addresses in a module's own code come out as the compiled address plus
  `dolrecomp_overlay_shift[k]`, the runtime's record of how far module `k`
  moved;
- each `lis` of an address in a moving module adds that module's shift, or
  its bss's (`dolrecomp_overlay_bss_shift[k]`), and calls between modules land
  where the callee is;
- function hashes leave out the instruction bits a module's placement
  changes.

The runtime finds where the game linked each module, an RSO module in the RSO
library's list of loaded modules by file name and a REL module in the OS's
module list by module id, and sets the shifts from there.

So a config needs no addresses. A module with no `base` is compiled for a
place of DolRecomp's choosing: resident modules one after another past the
end of the DOL's image, overlays from the end of MEM1 down.

Giving a resident module a `base` (and its `bss`, if it has one) compiles it
for the address the game really loads it at, and its code then never moves.
It runs no faster for it. But where a module is compiled changes which data
words look like code pointers, so a few function boundaries, and with them
the points where guest time is charged, can differ between the two
placements: keep the same placement as any reference runs you compare
against. `dolrecomp rso-find` reads a module's real address and bss off a RAM
dump.

## Natives

A native is a guest function your runtime implements in host code: a thread
switch, `memcpy`, a matrix routine, any SDK call you want to handle natively.
Generated code calls the host function `bool <symbol>(CPUState*)` in place of
the guest one. It returns true after doing the whole call, or false, with no
side effects, to run the guest code instead.

Natives are listed in `.sigs` files, one per line:

```
<symbol> <size> <first> <hash> [at <expression>] [jump]
```

`size` is the function's size in bytes. `first` and `hash` are its first
instruction and the FNV-1a 64 hash of its code, with what the linker decides
masked out: branch targets outside the function, `lis` immediates and the low
halves paired with them, and small-data (`r2`/`r13`) offsets. Everything else
stays, struct offsets included, so a native only replaces the SDK build it was
written for. `dolrecomp sign` prints the three fields for the code at an
address.

- Without `at`, the code must occur exactly once in the game. Code too
  generic for that (a getter, a point inside a function) is found `at` an
  expression over other entries, and checked there.
- `jump` marks a native that sets the pc itself instead of returning.
- A symbol starting with `@` is an anchor: an entry that only helps locate
  others.
- `#` starts a comment. An entry the game doesn't have is skipped, with any
  define that needs it.

Defines read what differs between games on the same SDK off the matched code:

```
define <NAME> <expression>
```

| Expression | Value |
|---|---|
| `S`, `S+off` | An address in entry `S` |
| `call(S+off)` | The target of the branch there |
| `addr(S+hi,lo)` | The address the `lis` at `S+hi` and the instruction at `S+lo` build |
| `imm(S+off)` | The signed 16-bit immediate there |

Offsets are hexadecimal. Defines go to `ppc_natives.h`. The full format is
described in `src/app/natives.h`.

### Bringing natives to a new game

Point `sigdb` at Dolphin's signature database (`Sys/totaldb.dsy` in a Dolphin
install; it is GPL-2.0-or-later, so DolRecomp reads it from there rather than
shipping it). The output then also gets:

- `sdk_names.txt`: the SDK functions the database recognizes in the game;
- `sigs_candidates.txt`: for each of your signatures the game didn't match but
  whose function the database found, the `.sigs` line the game's code takes.

The database only checks opcodes, so a candidate is a function with the right
name and shape: compare it with the SDK build your native was written for
before using it.

## Output

| File | Contents |
|---|---|
| `ppc_recomp.N.c` | The shards, the sources to compile; each includes some of the chunks |
| `chunks/` | The generated functions |
| `ppc_recomp.h` | Their declarations, the entry point and the CPU defines |
| `ppc_tables.inc` | The runtime's view of the code: ranges, functions, hashes, modules and their relocations. Included by `core/guest_module.c` |
| `ppc_natives.h` | The natives' and the config's defines |
| `natives.txt` | The natives found, with their addresses |
| `functions.txt` | Every function's address and size, and its module, for comparing with symbol maps |
| `sdk_names.txt`, `sigs_candidates.txt` | With `sigdb` only (see above) |

A chunk is rewritten only when its contents change, and a shard only when one
of its chunks did, so an incremental build recompiles only what moved.

## Writing a runtime

The generated code expects a runtime around it. Through `core/cpu.h` and
`core/guest_module.h`, the runtime provides:

- **The CPU state.** A `CPUState` with `ram`/`exram` pointing at the host's
  MEM1 and MEM2 (then `ppc_build_fast_memory()`), and its callbacks:
  `external_read`/`external_write` (MMIO and anything outside RAM), `spr_read`
  and `spr_write`, `cache_control` and `cache_range`, `instruction_fallback`
  (instructions the generated code leaves to the host), `read_timebase` and
  `msr_written`.
- **The dispatcher.** `dolrecomp_lookup(ctx, pc)` returns the function to run
  at a pc, or NULL for the runtime to handle it (usually with an
  interpreter). `g_guest_module` lists every function with its range and
  hash. Check a function's hash against guest RAM before running it, and
  again after any icache invalidation that touches it.
- **Direct calls.** `guest_set_direct_calls(flags, idle_pc)` publishes one flag
  per function: whether generated code may call it directly. Clear a
  function's flag when its code stops matching.
- **Guest time.** `ctx->downcount` holds the cycles left before the next
  event. `dolrecomp_safepoint(ctx, pc)` is called when a loop runs it out;
  return nonzero to make the generated code leave for the dispatcher.
- **Module placement.** `dolrecomp_overlay_shift[k]` and
  `dolrecomp_overlay_bss_shift[k]` say how far module `k` loaded from where it
  was compiled. Read them off the game's module lists (each `GuestOverlay`
  gives its module's name or REL id, compiled base and bss section) and treat
  the module's functions as unverified while they change. The game may run a
  module right after linking it, so look again when a pc finds no function.
- **Natives.** One `bool <symbol>(CPUState*)` per native the signatures found.

[DualizeRecomp](https://github.com/Trick230/DualizeRecomp)'s `DualizeRecomp/cpu`
is a complete runtime to read alongside this list.

## Limitations

- **Native signatures are per SDK build.** A native matches only the SDK
  version it was written for. On another game its function stays recompiled
  guest code: correct, only slower until a signature for that build is
  written.
- **Function discovery is heuristic.** Functions are found from branches,
  symbols, module exports and data words that point into code. Code reached
  only through pointers computed at run time, or entered in the middle, is
  left to the runtime (usually its interpreter).
- **Natives are looked for in the DOL and in modules with a `base` only.** An
  SDK function inside a module compiled to run anywhere is not replaced.
- **A resident REL importing from a REL overlay.** When the overlay loads, the
  game's linker patches the resident module's code; those functions then no
  longer match their hashes and run on the runtime's fallback.

## Tests

```sh
cmake -S . -B build -DDOLRECOMP_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

The tests cover the instruction decoder and semantics, floating point, the
DOL and REL loaders, RSO and REL linking with module placement, archives and
Yaz0, natives and the signature database.

## License

GPL-3.0, as upstream (`LICENSE`). Dolphin's signature database is not part of
this repository.
