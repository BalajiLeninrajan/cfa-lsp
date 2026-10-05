# A long-running translator

Status: design, not implemented. Part of issue #1.

## Why

Every check starts `cfa-cpp` from scratch. It parses the four prelude files
(`gcc-builtins.cfa`, `extras.cfa`, `prelude.cfa`, `builtins.cfa`) and every
libcfa header the program includes, runs about 40 passes over all of it, and
resolves all of it. The user's file is a small part of that input. A check
takes about 2 s on a 30-line program and 5.4 s on an 80-line coroutine
program, and `Resolve` is about half of it. clangd avoids the same cost by
parsing headers once into a preamble. This note says how cfa-cpp could do
something similar, and why it shouldn't try to reset itself in process.

## What makes it hard

I read through `cforall/src` for state that one translation leaves behind
for the next, and for ways a translation ends the process.

Global state:

- the parser: `linkage`, `typedefTable` and `parseTree` in
  `Parser/RunParser.cpp`, `yyfilename`, `strtext` and `yylineno` in `lex.ll`,
  `linkageStack` and `forall` in `parser.yy`;
- errors: `SemanticErrorSink`, `SemanticErrorThrow` and the warning levels set
  from `-W` flags (`Common/SemanticError.cpp`);
- counters that name generated code: `lastUniqueId` (`AST/Decl.cpp`),
  `globalResnSlot` (`ResolvExpr/CandidateFinder.cpp`), `renaming`
  (`ResolvExpr/RenameVars.cpp`) and 23 function-local `static UniqueName`s in
  the passes;
- interned symbols (`Common/Symbol.cpp`) and statistics (`Common/Stats`);
- the LSP module's own state in `LSP/Lsp.cpp`: diagnostics, recorded typedefs,
  renames, exception uses, the snapshot.

Ways out of the process: 24 `exit()` or `abort()` calls in hand-written
sources (more in the generated lexer), and 779 `assert`/`assertf` calls, which
abort. The LSP mode turns user errors into a dump, but an internal assertion
still kills the process.

The AST is changed in place. `ast::mutate` copies a node only when it is
shared, and passes splice declarations into `TranslationUnit::decls`. So a
resolved prelude can't be handed to the next check without a deep copy.

Passes are whole-unit. `ast::Pass<Core>::run` builds a fresh core and visits
every declaration in order (`accept_all` in `AST/Pass.impl.hpp`). Passes that
need names keep them in the core's symbol table (`WithSymbolTable`), built as
declarations are visited. Running a pass on only the new declarations means
keeping that core alive.

Resetting all of that in process is not something I'd trust: one missed
global gives wrong answers that depend on what was checked before, and an
assertion would still take down the long-running process. `fork()` gives
both a full reset and crash isolation for free, so the design is built on it.

## Design

### Stage 1: a fork server with the prelude loaded

`cfa-cpp --lsp-server [usual cfa-cpp flags]` parses the prelude the way
`main` does now, then reads requests from stdin, one JSON object per line:

```jsonc
{ "in": "/tmp/cfa-lsp-x/in.i", "json": "/tmp/cfa-lsp-x/out.json",
  "focus": ["/abs/file.cfa"], "cOut": "/tmp/cfa-lsp-x/out.c", "stopAfterResolve": false }
```

For each request it forks. The child sets `LSP::options` from the request,
parses `in`, and runs the rest of `main` as today, ending in `lspFinish` and
`_exit`. The parent writes `{"pid": N}` as soon as it has forked and
`{"status": S}` (an exit status, or `"signal": N`) when the child is reaped.
cfa-cpp is single-threaded, so forking is safe.

`main` gets split into `loadPrelude()` and `translate( FILE * )`, which the
normal one-shot mode calls back to back. That keeps the normal path as it is
and makes the server mode a loop around `fork` and `translate`. `parse()`
already accumulates into `parseTree` across calls (it is called once per
prelude file), so parsing the input in the child appends to the prelude's
declarations.

Flags that change the prelude or the warnings (`--prelude-dir`, `-W...`,
`-w`, `-l`, `-n`) are fixed for the life of a server. The server process is
keyed by the translator path and these flags, and cfa-lsp starts a new one
when they change.

This stage saves only the prelude's parse time, which I haven't measured.
Its value is that it builds the process model the later stages need.

### Stage 2: cache the preamble's parse

The preamble is the start of `in.i` up to the line marker that returns to
the main file after its leading `#include`s: everything before the main
file's own code. cfa-lsp splits `in.i` there, hashes the preamble, and sends
both parts.

The server keeps a warm process per preamble hash (one or two of them): a
child forked from the prelude-only parent that has also parsed the preamble
and now forks once per request. A request whose hash has no warm process
gets one first. The rest of the input starts with a line marker naming the
main file, because `parse()` resets `yylineno`.

The split must fall between top-level declarations. A header that ends in
the middle of one is legal C but rare. If the preamble doesn't parse on its
own, the server falls back to parsing the whole input in the child.

This saves the headers' parse time. Every pass still runs over the whole
unit.

### Stage 3: pass checkpoints

The real cost is running the passes, `Resolve` above all, over the
preamble's declarations on every check. The warm process could run them
once:

1. For each pass up to and including `Resolve`, in order, construct the
   `ast::Pass` object, visit the preamble's declarations with it, and keep it.
2. For a request, the child visits only the new declarations with each kept
   pass, in the same order, then runs the passes after `Resolve` (or stops,
   with `--lsp-stop-after-resolve`) over the whole unit as today.

The order differs from today's. Now pass k sees every declaration after
pass k - 1 has run on all of them; with checkpoints, pass k runs on the
preamble before pass k - 1 has seen the main file. That is the same thing
as long as no pass looks at declarations that come after the one it is
visiting, and header declarations can't name main-file declarations. Each
pass still has to be checked for:

- entry points that run several passes, each of which needs its own
  checkpoint (`Concurrency::implementKeywords` runs five,
  `Validate::decayForallPointers` three, `InitTweak::genInit` several);
- work done on the whole unit outside `Pass::run`;
- `TranslationUnit::global`, which `Validate::findGlobalDecls` fills and
  `Resolve` reads (the special declarations it finds are in the prelude, so
  they would be found during the preamble);
- state in the core besides the symbol table that a later declaration
  depends on.

A pass that fails these checks runs over the whole unit, which is still
correct but costs its time on the preamble again. The LSP snapshot walks
`unit.decls`, so it keeps dumping the preamble's global declarations.

This is where most of the gain is, and most of the risk: about 40 call sites
in `main.cpp`, each pass read for the cases above.

### Server side

A `TranslatorServer` class in `src/server` owns one `cfa-cpp --lsp-server`
process per key. `Checker::front` sends it a request instead of running the
one-shot command. Cancelling kills the child pid the server reported, the
same timeout applies, and the temp files stay in the check's `TempDir`. If
the server process exits or its pipe closes, the check falls back to the
one-shot command and the next check starts a new server. `cfa -E` still runs
per check; it is not where the time goes.

It starts behind an `initializationOptions` flag (`persistentTranslator`,
off by default) until it has been used for a while.

## Measure first

`cfa-cpp -S time` prints the time of each pass. Before stage 3, measure the
demo and the coroutine program: parse against passes, and how the pass time
splits between the preamble and the main file (by checking an empty main
file with the same `#include`s). If the main file's share is small, stage 3
is worth its risk; if parsing is a large share, stage 2 alone may be enough.

## Tests

- The fake toolchain in `tests/server/fake` gets a `--lsp-server` mode, so
  the session tests cover requests, cancellation and a server that dies.
- A child that aborts on an assertion must leave the server working for the
  next request.
- `tests/translator` runs every fixture both one-shot and through the server
  (each stage) and requires identical dumps.
