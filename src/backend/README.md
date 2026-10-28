# Backend

The C backend turns decoded functions into C sources.

- `c_cfg.c` builds each function's control flow: block leaders, block cycle
  counts, loops that can run as local `goto`s and return targets.
- `emitter.c` writes the C: one function per guest function, the shard files
  and the dispatch tables the runtime looks functions up in.
- `codegen.c` drives a whole run: functions, natives and RSO modules.
- `symbols.c` names functions and their tables.

Generated code enters through a guard on `ctx->pc`: with no guest hooks
active, execution stays native; a hook or an unknown target returns to the
runtime with the architectural state in `ctx`.
