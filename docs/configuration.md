# Configuration

## Compiler flags

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

## initializationOptions

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
| `skipSystemBodies` | `true` | Give the translator empty bodies for the functions defined in libcfa and system headers before it resolves them (see [limits](limits.md)). |
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
[dump-format.md](dump-format.md) has the details.

## Logging

Set `CFA_LSP_LOG` to `error`, `warn`, `info` or `debug` to log to stderr. At
`info` it logs how long each stage of a check took.
