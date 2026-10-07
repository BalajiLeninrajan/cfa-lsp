# How it works

On open, and after edits once `debounceMs` has passed, a worker thread checks
the buffer:

1. It writes the buffer to a temporary file that starts with
   `# 1 "/real/path.cfa"`, so every location in the output names the real
   file, and runs `cfa -E` on it with the user's flags. The real file's
   directory is added as a quote directory (`-iquote`, see [configuration](configuration.md#compiler-flags)), so
   `#include "x.hfa"` finds the file next to it and a project header named
   like a libcfa header doesn't hide that one from `#include <x.hfa>`.
2. It runs the forked translator: `cfa-cpp --lsp out.json --lsp-focus
   /real/path.cfa [--lsp-c-out out.c] ... in.i`. The translator runs its
   passes as usual, records errors instead of stopping at the first one, and
   after the resolver has run writes the declarations, resolved references,
   expression types and scopes to `out.json`. [dump-format.md](dump-format.md) describes
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

An edit usually lets the check in flight finish. It publishes its
diagnostics, mapped through the edits made since it started, and the next
check starts after it. A diagnostic on text that one of those edits changed
is left out, so an error you have just fixed doesn't come back; if it still
applies, the next check reports it again. Cancelling on every edit would mean
that while you type with pauses a little longer than `debounceMs`, every
check is cancelled before it finishes and the diagnostics never update.

An edit does cancel two kinds of check. One that has run less than half as
long as the file's last check is cancelled so that the check of the new text
starts sooner, but never two in a row, so while you keep typing every other
check still finishes. One that has run more than twice as long as the last
check is likely stuck on something the edit may have fixed. Closing the file
also cancels its check.

## The background index

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

## Columns

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

## Formatting

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
