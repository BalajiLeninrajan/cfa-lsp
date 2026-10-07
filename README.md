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
- formatting of a file or a range, which only changes whitespace

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

The first `make` builds the translator too, which takes about 13 minutes at
`-j8`. You need g++ with C++20 support and the usual autotools build
dependencies of Cforall.

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

Put compiler flags in a `cfa_flags.txt` next to your sources or in a parent
directory, one or more per line:

```
-I include
-DDEBUG
-Wall
```

Without one, the flags are `-Wall -Wextra`. The server also takes
`initializationOptions` and `workspace/didChangeConfiguration` settings,
listed in [docs/configuration.md](docs/configuration.md). Set `CFA_LSP_LOG`
to `error`, `warn`, `info` or `debug` to log to stderr.

## Docs

- [Configuration](docs/configuration.md): `cfa_flags.txt`, server options
  and logging
- [How it works](docs/how-it-works.md): the check pipeline, the background
  index, column mapping and the formatter
- [Limits](docs/limits.md): check times and what the server gets wrong or
  refuses to do
- [Development](docs/development.md): layout, building, tests, releases and
  the translator fork
- [Dump format](docs/dump-format.md): the JSON the translator writes for the
  server
- [A long-running translator](docs/persistent-translator.md): design notes
  for checks that skip re-reading headers

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
