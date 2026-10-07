# cfa-lsp

A language server for [Cforall](https://cforall.uwaterloo.ca) (CFA). It gets
its answers from the CFA translator itself: a fork of `cfa-cpp` (in the
`cforall/` submodule) parses and resolves the file the way the compiler does,
then writes a JSON description of what it found. Hover on an overloaded call
shows the overload the resolver picked, including overloads on the return
type, and definition jumps to that declaration.

Features:

- diagnostics from cpp, the translator (syntax, resolver and checking errors,
  translator warnings) and, optionally, gcc on the generated C
- hover with the declaration, doc comment, location and overload count, and
  for macros the `#define`
- go to definition and declaration, including into libcfa and the prelude,
  for macros, and on `#include` lines
- find references, across the workspace
- document symbols
- completion of locals, `with` fields, globals and keywords, and of members
  after `.` and `->`
- signature help
- semantic tokens, including keywords, so they stay coloured where a C or
  C++ grammar loses its place at CFA syntax such as `for () {`
- document highlight
- inlay hints: parameter names at call sites, and the type a call of a
  `forall` function returns when it differs from the declared return type
- rename, across the workspace
- workspace symbols
- call hierarchy (incoming and outgoing calls)
- clangd's `textDocument/switchSourceHeader`, between `x.cfa` and `x.hfa`
- quick fixes for an undeclared identifier: a visible name a typo away from
  it, and the `#include` of the libcfa header that declares it (also for a
  type such as `string` used without its header, which is a syntax error)
- formatting of a file or a range, which only changes whitespace (see below)

It runs on Linux and needs an installed CFA 1.0.0 (`cfa` on `PATH`). The
translator fork is based on the same version (upstream commit `fade1b55`).

## Install

### From a release

Releases have the server and the translator built for Linux x86-64 (on
Ubuntu 24.04, the same OS as the cs343 student server). This installs the
latest one into `~/.local`:

```sh
curl -fsSL https://raw.githubusercontent.com/BalajiLeninrajan/cfa-lsp/main/scripts/install-release.sh | bash
```

In a clone, `make install-release` does the same, `V=0.2.0` picks a version
and `PREFIX=...` another directory. The script checks the download's SHA-256
and needs only curl. `cfa-lsp --version` shows what is installed.

### From source

```sh
git submodule update --init
make -j8
make install PREFIX=~/.local
```

The first `make` configures and builds the translator in `build/cforall`,
which takes about 13 minutes at `-j8`. Later builds only recompile what
changed. You need g++ with C++20 support and the usual autotools build
dependencies of Cforall. bison and flex are only needed if you edit
`cforall/src/Parser/parser.yy` or `lex.ll`. The submodule tracks their
output, and `make` regenerates it in place when git shows the grammar changed
after it, committed or not. `make parser` regenerates it unconditionally.

`make install` puts the server in `$PREFIX/bin/cfa-lsp` and the translator in
`$PREFIX/libexec/cfa-lsp/cfa-cpp`, both stripped. `make uninstall` removes
them. The server finds the translator next to itself, so nothing else needs
configuring if `cfa` is on `PATH`.

## Editor setup

The server speaks LSP over stdin and stdout. In Neovim 0.11:

```lua
vim.filetype.add( { extension = { cfa = 'cfa', hfa = 'cfa' } } )
vim.lsp.config( 'cfa_lsp', {
  cmd = { 'cfa-lsp' },
  filetypes = { 'cfa' },
  root_markers = { 'cfa_flags.txt', '.git' },
} )
vim.lsp.enable( 'cfa_lsp' )
```

Inlay hints are off by default in Neovim; `vim.lsp.inlay_hint.enable()` turns
them on. Switching between a source file and its header is a clangd extension,
so it needs a command of its own:

```lua
vim.api.nvim_create_user_command( 'CfaSwitch', function()
  local client = vim.lsp.get_clients( { bufnr = 0, name = 'cfa_lsp' } )[1]
  if not client then return end
  client:request( 'textDocument/switchSourceHeader', { uri = vim.uri_from_bufnr( 0 ) }, function( _, uri )
    if uri then vim.cmd.edit( vim.uri_to_fname( uri ) ) end
  end, 0 )
end, {} )
```

Any client that can start a stdio server for `.cfa` and `.hfa` files works
the same way.

## Configuration

### Compiler flags

The server looks for a `cfa_flags.txt` in the file's directory and then in
each parent directory, and uses the nearest one. Put one flag per line, or
several separated by spaces (`-I ../include` works). Lines starting with `#`
are comments. Relative paths resolve against the directory of the
`cfa_flags.txt`. Without one, the flags are `-Wall -Wextra`.

```
-I include
-DDEBUG
-Wall
```

The flags are split between the stages the way the `cfa` driver splits them:
`-D`, `-U`, `-I` and friends go to the preprocessor, `-Wall`, `-Werror`, `-w`
and CFA's own warning names (`-Wself-assign`, ...) to the translator, and the
rest of the warnings and `-f`, `-O`, `-std` options to gcc. `-iquote DIR` is
passed to `cfa -E` as `-Wp,-iquoteDIR`, because the `cfa` driver mistakes the
directory in `-iquote DIR` for an input file. A directory with a comma in its
name can't go through `-Wp,`, so it is passed as `-I DIR` instead.

### initializationOptions

All optional.

| Option | Default | Meaning |
|---|---|---|
| `cfa` | `cfa` on `PATH` | The CFA driver. |
| `translator` | next to the server, then `CFA_LSP_TRANSLATOR` | The forked `cfa-cpp`. Without it the server falls back to `cfa -c` and reports only compiler errors. |
| `preludeDir` | derived from `cfa` | Directory with `prelude.cfa` (e.g. `<prefix>/lib/cfa/x64-debug`). |
| `flags` | from `cfa_flags.txt` | Flags as an array, or a string in `cfa_flags.txt` format. Overrides the file. |
| `backend` | `true` | Run gcc `-fsyntax-only` on the generated C for gcc's warnings. |
| `cc` | `gcc` | The C compiler for the backend check. |
| `stopAfterResolve` | `false` | Stop the translator after `Resolve`. Checks take 40 to 45% less time, but you lose the warnings and errors of the later passes and gcc's warnings (see below). |
| `skipSystemBodies` | `true` | Give the translator empty bodies for the functions defined in libcfa and system headers before it resolves them (see below). |
| `debounceMs` | `500` | Wait after the last edit before checking. |
| `index` | `true` | Check every `.cfa` file under the workspace root in the background, for workspace-wide references, rename, symbols and call hierarchy. |
| `timeoutMs` | `120000` | Limit for each child process. |

The same options can be changed while the server runs, through
`workspace/didChangeConfiguration`. Settings under a `cfa-lsp` key (or the
whole settings object, if it has no such key) override `initializationOptions`
one key at a time, and a `null` value goes back to the
`initializationOptions` value. Clients that use `workspace/configuration`
are asked for the `cfa-lsp` section at startup and after each change. A
change re-checks every open file. In Neovim:

```lua
vim.lsp.config( 'cfa_lsp', { settings = { ['cfa-lsp'] = { backend = false } } } )
```

With `stopAfterResolve`, the translator skips `Fix Init` and the passes after
it. Their checks are the ones you lose: the self-assignment warning, jumps
past an initialization, fields used before they are constructed or never
constructed, `waitfor` without `monitor.hfa`, a second `main`, bad virtual
casts, the rvalue to reference conversion warning and unbound type variables
in `Box`. No C is generated, so `backend` has no effect.
`docs/dump-format.md` has the details.

### Logging

Set `CFA_LSP_LOG` to `error`, `warn`, `info` or `debug` to log to stderr. At
`info` it logs how long each stage of a check took.

## How it works

On open, and after edits once `debounceMs` has passed, a worker thread checks
the buffer:

1. It writes the buffer to a temporary file that starts with
   `# 1 "/real/path.cfa"`, so every location in the output names the real
   file, and runs `cfa -E` on it with the user's flags. The real file's
   directory is added as a quote directory (`-iquote`, see above), so
   `#include "x.hfa"` finds the file next to it and a project header named
   like a libcfa header doesn't hide that one from `#include <x.hfa>`.
2. It runs the forked translator: `cfa-cpp --lsp out.json --lsp-focus
   /real/path.cfa [--lsp-c-out out.c] ... in.i`. The translator runs its
   passes as usual, records errors instead of stopping at the first one, and
   after the resolver has run writes the declarations, resolved references,
   expression types and scopes to `out.json`. `docs/dump-format.md` describes
   the format.
3. The server loads the JSON (`src/analysis`) and publishes diagnostics.
   An error in an included file goes on that file, and a summary goes on the
   `#include` line of the main file that leads to it. In libcfa and system
   headers only errors are published, not warnings.
4. If the translator wrote C, it runs gcc `-fsyntax-only` on it and adds
   gcc's warnings, mapped back to source lines through the line markers.
   Errors that stop translation also stop code generation, so then the
   warnings of the last gcc run stay, except the ones on lines edited since.
   When the translator reported errors but still wrote C (errors from the
   passes that only check the program), gcc's errors are left out: they are
   about code the translator already rejected.

When an open header has unsaved edits, checks of the files that include it
read the buffer: the server writes it to an overlay directory and puts that
directory on the include path right before the directory it shadows (the
file's own, then each `-iquote` and `-I` directory). Editing such a header
re-checks the open files that include it.

Requests are answered right away from the last good result, in the order
they arrive. A request cancelled with `$/cancelRequest` before its turn comes
gets the `RequestCancelled` error instead of an answer. After a syntax error
the translator skips the broken statement or declaration and translates the
rest, so a check of a half-typed line still covers the rest of the file. A
`{` still open at the end of the file is closed there, so a block typed
without its `}` yet keeps the function it is in. A check that finds no
declarations in the file at all leaves the previous result in use. Edits made
since that result are tracked, and positions are mapped through them in both
directions. Names that the result doesn't have (declared since, or in code
that didn't parse) get member completion, hover and definition from a scan of
the buffer text for their declaration (`Rect r2;`), as long as the type is
one the result knows.

Macros never reach the translator, so hover and definition on a macro read
the `#define` lines of the files in the translation unit. A definition in a
branch of `#if` that cpp dropped doesn't count. The server tells which branch
cpp kept from its output: a branch with code that left no line in it was
dropped. A branch with only directives in it is decided by evaluating the
condition with the macros defined so far, and counts as kept when the
condition depends on something the source doesn't show, such as a compiler
macro or a `-D` flag.

An edit does not cancel the check in flight. It finishes and publishes its
diagnostics, mapped through the edits made since it started, and the next
check starts after it. Otherwise, while you type with pauses a little longer
than `debounceMs`, every check would be cancelled before it finished and the
diagnostics would never update. The exception is a check that has already
run more than twice as long as the file's last one, which is likely stuck on
something the edit may have fixed; an edit cancels that one. Closing the file
also cancels its check.

### The background index

When the client gives a workspace root, the server looks for `.cfa` and
`.hfa` files under it (skipping hidden directories) and checks each `.cfa`
file from disk, one at a time, whenever no open file is waiting for a check.
These checks stop after `Resolve`, and every `.hfa` under the root is a focus
file in them, so uses inside headers are recorded too. A check of an open file
cancels the index check in flight, which runs again later.

From each check the server keeps a small table: the declarations of each
function, variable and type, the uses of each, and for calls the function
they are in. Two tables talk about the same entity when they share a
declaration (file and position of the name), so a prototype in a header links
the uses in every file that includes it to the definition in the file that
has the body. References, definition, rename, workspace symbols and call
hierarchy use these tables together with those of the open files. For an open
file, its own check wins over the index.

The index is brought up to date when a file is saved, when the configuration
changes and when the client reports changed files
(`workspace/didChangeWatchedFiles`, which the server registers for if the
client supports it): files whose own text or included headers changed since
their check are checked again, new files are added and deleted ones dropped.
The walk stops after 50000 directory entries and 1000 `.cfa` files.

### Columns

`cfa -E` collapses whitespace between tokens and expands macros, so the
translator's columns are offsets into the preprocessed line, not the line in
your file. Line numbers are right, thanks to the line markers. The server
tokenizes both lines and aligns the tokens to map each column back. Tokens
produced by a macro map to the whole macro invocation, and a macro argument
maps to where it is written.

The translator also gives each position's line in the preprocessed text. When
a system-header macro such as `assert` or `isdigit` makes cpp split a line
into pieces that share one line number, that line says which piece a column
is in, and for a header included twice it says which copy. A `#line`
directive in your file changes the line numbers in the markers; the server
reads the file's directives to get the real lines back.

To tell an identifier in a macro's body from the same name passed as an
argument (`tmp` in `SWAP( x, tmp )` when the body declares its own `tmp`),
the server redoes the expansion from the `#define`. That works for a macro
defined in the same file or in a header the file includes directly, whose
arguments and body use no other macros, and whose body has no `#` or `##`.
For other macros, such an identifier maps to the argument.

### Formatting

The formatter only touches whitespace, so it can't mangle `?{}`, `^?{}`,
`with` clauses or `sout | x` chains the way a C formatter would. It indents
each line that starts a statement or declaration by its brace depth, with one
more level for the statements under a `case` label and for the body of an
`if`, `for`, `while`, `with`, `else` or `do` written without braces. A line
that continues a statement (arguments split over lines, a `| x` chain) moves
by as much as the statement's first line moved, so alignment within the
statement is kept. It also removes trailing whitespace and applies the
client's `insertFinalNewline` and `trimFinalNewlines` options. Spacing within
a line, preprocessor directives and the inside of strings are left alone, and
lines inside a block comment move with the line the comment starts on.

## Limits

- A check runs the translator over the file and every header it includes.
  The translator skips the bodies of the functions in libcfa and system
  headers (`skipSystemBodies`), which makes a check of a cs343 assignment 2
  to 4 times faster, but it still parses and resolves every declaration in
  them: about 2 seconds for a small program that includes `fstream.hfa`,
  `string.hfa` and `stdlib.hfa`. `stopAfterResolve` cuts a further part of
  that. `docs/persistent-translator.md` describes how a long-running
  translator could avoid re-reading the headers on every check.
- With `skipSystemBodies`, an error that your code causes inside a libcfa
  function body (a macro or an overload declared before the `#include` that
  breaks the header) is not reported. Set it to `false` to get those back.
- Each file is checked on its own. Open headers are checked as if they were
  the main file.
- The index matches declarations by location. A function declared separately
  in two `.cfa` files, without a shared header, is two functions to it, so
  references and call hierarchy show only one side. Rename refuses such a
  name.
- The index reads files from disk, so a file changed outside the editor is
  only picked up on the next save or watched-file notification.
- An unsaved header is found through the include path. A header on disk that
  includes it with `#include "..."` from the same directory still reads the
  copy on disk.
- Uses inside macro bodies have no references. Inside a `cofor` body, uses of
  the loop variable have none either: they name the copy the translator makes
  in the function it generates for the body.
- Rename of a name declared in a header or used in other files needs the
  index, and is refused until the index has checked every file, while the
  walk was cut short by the limits above, and when the name appears in a file
  whose check failed. Without the index, rename works within one file and
  refuses such names. It always refuses names declared
  in libcfa or the prelude, operators, names used in a macro body, and names
  spelled somewhere the dump has no reference for, such as a designator, an
  array dimension in a typedef, cast or `sizeof`, an `#if 0` block or a
  function that failed to resolve. It does not check whether the new name
  clashes with another one in scope.
- Completion does not know about type-only contexts, `inline` member
  embedding or qualified enumerators (`Colour.Red`). libcfa names containing
  `$` are hidden unless the prefix has a `$`.
- Type and signature text comes from the translator's pretty printer, so it
  can differ from what you wrote (`Fib &f`, assertions left out).
- If the translator fails an internal assertion or crashes, the check reports
  `internal translator error` on line 1, and unless the crash came after
  resolution the previous results stay in use.
- The formatter counts braces in every branch of an `#if`, so branches that
  each open a brace (two versions of a function header) indent what follows
  one level too deep.
- The `#include` fix knows the names declared at file scope in the `.hfa`
  files of `<prefix>/include/cfa` and its `concurrency/` and `collections/`
  directories, found by scanning their tokens, so names that only a macro
  declares are missing.

## Development

```
src/server/          LSP transport, documents, the check pipeline (C++20)
src/analysis/        dump loading, SourceMap (column mapping), queries, the
                     formatter and the libcfa header index
cforall/             the CFA source, with the LSP dump in cforall/src/LSP
docs/                the translator's JSON format, design notes
tests/               doctest tests, one binary; fixtures in tests/fixtures
translator.mk        builds the translator
compile-commands.mk  translator entries for compile_commands.json
```

Build and test:

```sh
make -j8                  # server and translator
make -j8 test             # every test
make test ARGS='-ts=integration'
make BUILD=build/mine test   # a separate object directory
make compile_commands.json   # for clangd
```

`compile_commands.json` covers the server, the analysis code and the tests,
and the translator sources in `cforall/src` once `make translator` has
configured `build/cforall`. The translator entries use the compile command
from that configured build. Run it again after the first `make translator`
to pick them up.

`make test` builds the server and the test binary, then runs everything.
Tests that need `cfa` or the translator skip themselves when those are
missing. The suites are:

- `tests/analysis`: SourceMap against real `cfa -E` output, the tokenizer,
  and the queries on hand-written dumps;
- `tests/server`: transport, position mapping, flags, compiler output parsing
  and LSP sessions against a fake toolchain (`tests/server/fake`);
- `tests/translator`: the forked translator on small programs, checking the
  dump, plus a fuzz test that runs it on truncated and mutated fixtures. Set
  `CFA_LSP_FUZZ=all` to run every variant instead of a sample (about 10
  minutes on CI);
- `tests/integration`: the built server, the translator and `cfa` on the
  project in `tests/fixtures/project`, driven over pipes the way an editor
  would.

To see what the server answers on any program, the probe test opens a file
and prints hover and definition for every identifier. `CFA_LSP_PROBE_FULL=1`
prints whole hovers:

```sh
make test ARGS='-tc=probe -s' CFA_LSP_PROBE=path/to/prog.cfa
```

### Releasing

```sh
make release V=0.2.0
```

`scripts/release.sh` tags `main` as `v0.2.0` and pushes the tag. It refuses
unless the checkout is `main`, clean and equal to `origin/main`, CI passed on
that commit, and the version is higher than the last tag. The tag's CI run
builds and tests it like any other, then the `release` job checks that
`cfa-lsp --version` says `0.2.0` and publishes a GitHub release with the
tarball and its SHA-256. The release notes list the PRs merged since the last
release. A version with a suffix (`0.3.0-rc1`) is published as a prerelease.

The version comes from `git describe`: `0.2.0` on the tagged commit,
`0.2.0-3-gabc1234` three commits later, and `-dirty` with uncommitted changes.
The server reports it in `--version` and in `serverInfo`.

### The translator

The translator changes live in the submodule, on branch `balaji/lsp` of
[BalajiLeninrajan/cforall](https://github.com/BalajiLeninrajan/cforall). Most of
them are in `cforall/src/LSP/Lsp.cpp`; the rest record source locations in the
parser and keep going after errors. Change `docs/dump-format.md` first when
the JSON changes, then both sides.

## License

The code in this repository is MIT licensed; see [LICENSE](LICENSE).

The `cforall/` submodule is Cforall, copyright the University of Waterloo,
under its own 3-clause BSD license (`cforall/LICENSE`). Its changes on the
`balaji/lsp` branch are under that license too. The `cfa-cpp` that
`make install` puts in `libexec/cfa-lsp` is built from it, so if you hand out
that binary, include `cforall/LICENSE` with it.

`third_party/` holds [nlohmann/json](https://github.com/nlohmann/json) and
[doctest](https://github.com/doctest/doctest), both MIT licensed, with their
license files next to them.
