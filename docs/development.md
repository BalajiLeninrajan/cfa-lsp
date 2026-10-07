# Development

```
src/server/          LSP transport, documents, the check pipeline (C++20)
src/analysis/        dump loading, SourceMap (column mapping), queries, the
                     formatter and the libcfa header index
cforall/             the CFA source, with the LSP dump in cforall/src/LSP
docs/                user and design docs, the translator's JSON format
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

The first `make` configures and builds the translator in `build/cforall`,
which takes about 13 minutes at `-j8`. Later builds only recompile what
changed. bison and flex are only needed if you edit
`cforall/src/Parser/parser.yy` or `lex.ll`. The submodule tracks their
output, and `make` regenerates it in place when git shows the grammar changed
after it, committed or not. `make parser` regenerates it unconditionally.

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

## Releasing

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

## The translator

The translator changes live in the submodule, on branch `balaji/lsp` of
[BalajiLeninrajan/cforall](https://github.com/BalajiLeninrajan/cforall). Most of
them are in `cforall/src/LSP/Lsp.cpp`; the rest record source locations in the
parser and keep going after errors. Change [dump-format.md](dump-format.md) first when
the JSON changes, then both sides.
