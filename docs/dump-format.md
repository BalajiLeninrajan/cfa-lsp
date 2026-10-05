# Translator dump format (version 1)

The forked translator (`cforall/`, branch `balaji/lsp`) has an LSP mode that
writes one JSON document describing a translation unit. The server reads it
through `src/analysis/`. This file is the contract between the two; change it
here first, then on both sides.

## Invocation

```
cfa-cpp --lsp OUT.json [--lsp-focus PATH]... [--lsp-c-out OUT.c] [--lsp-stop-after-resolve]
        [usual cfa-cpp flags] INPUT.i
```

- `INPUT.i` is the output of `cfa -E` (preprocessed, with `# N "file"` line
  markers). The server writes the buffer to a temp file whose first line is
  `# 1 "/abs/path/of/real/file.cfa"`, so every file name in the dump is the
  real path, never the temp path.
- `--lsp OUT.json` turns LSP mode on. Errors and warnings go into the JSON,
  not stderr. The exit status is 0 whenever `OUT.json` was written, even if it
  contains errors. A failed assertion, `abort` or fatal signal (`SIGSEGV`,
  `SIGBUS`, `SIGFPE`, `SIGABRT`) still writes `OUT.json`, with an internal
  error (see Diagnostic), and exits with 0. Non-zero means the translator
  could not write the dump at all.
- `--lsp-focus PATH` (repeatable) names the files the user is editing. Local
  declarations, `refs`, `exprs` and `scopes` are emitted only for focus files.
  Global declarations are emitted for every file, including libcfa headers and
  the prelude, because completion and hover need them. With no
  `--lsp-focus`, the focus is the main file: the file of the first line of
  code outside any `#include` and outside cpp's `<built-in>` and
  `<command-line>`. A line marker at the top of the input renames it, so for
  the server's temp file this is the real path.
- `--lsp-c-out OUT.c` also runs code generation and writes the generated C
  there, so the server can run the C compiler on it for backend warnings.
  Without it, the translator stops before code generation: every pass that can
  report a user error (the last one is `Box`) still runs.
- `--lsp-stop-after-resolve` ends the run right after the snapshot (see
  below), skipping the passes after `Resolve` and code generation. That saves
  40 to 45% of the time, but those passes' diagnostics are lost (listed
  below). `--lsp-c-out` writes nothing then. `complete` is unaffected.

The AST snapshot is taken right after the `Resolve` pass. Passes after it
still run (to report their errors) but don't change the dump. If `Resolve`
reports errors, translation stops after the snapshot (see `complete`).

The diagnostics that only the passes after `Resolve` report, and so
`--lsp-stop-after-resolve` drops:

- `Fix Init`: the self-assignment warning (`x = x`), "jump to label crosses
  initialization", and in constructors and destructors "field used before
  being constructed" and "field not explicitly constructed and no default
  constructor found";
- `Gen Waitfor`: `waitfor` without `monitor.hfa`;
- `Fix Main Linkage`: more than one `main`;
- `Virtual Expand Casts`: errors in virtual casts;
- `Convert L-Value`: the rvalue to reference conversion warning;
- `Box`: unbound type variables;
- and, since there is no generated C, the backend's gcc warnings.

## Coordinates

Every position in the dump is a **translator coordinate**:

- `file`: the path from the line marker, as written.
- `line`: 1-based line in that file (line markers keep this equal to the
  original line). After a `#line` directive in the source it is the number
  the directive gave; the server maps it back to the real line.
- `col`: 0-based **byte** offset within the *preprocessed* line, which is not
  the original line. `cfa -E` collapses whitespace and expands macros, so the
  server maps columns back with `SourceMap`.
- `pline` (optional): 1-based line of the position in `INPUT.i` itself,
  counting every line, line markers included. Line markers don't change it.
- End positions (`endLine`, `endCol`, `endPline`) are exclusive.

cpp sometimes splits one source line into several physical lines: around the
expansion of a macro defined in a system header (`bool` from `stdbool.h`, for
example) it emits `# N "file" 3`, the expansion, then `# N "file"` and the
rest of the line, repeating the same line number. Each physical line is a
preprocessed line of its own and columns restart at 0 in each, so one
(`line`, `col`) can name a position in more than one piece. For example,
`bool positive( int v );` becomes

```
# 8 "f.cfa" 3
_Bool 
# 8 "f.cfa"
    positive( int v );
```

and `positive` is at line 8, column 4 (of the second piece). Its `pline` is
the line of `    positive( int v );` in the input, which says which piece the
column is in. A header included twice (without a guard) also gives two input
lines for one (`file`, `line`), and `pline` tells the copies apart.

The translator writes `pline` for every position it read from the input file.
It is missing where the translator only knows a line (a diagnostic without a
column), for declarations from the prelude (which the translator reads from
separate files) and for code the translator made up without a source
location. The server must also work without it: it then picks the likeliest
piece and the first copy of a header.

A range object is `{"file", "line", "col", "endLine", "endCol", "pline",
"endPline"}`, where `pline` and `endPline` are optional. If only a start is
known, `endLine == line`, `endCol == col` and `endPline == pline`. A range can
start and end on different pieces of one split line; then `endPline` is
greater than `pline`, and `endCol` can be less than `col`.

## Document

```jsonc
{
  "format": 1,
  "complete": true,        // false when parsing, a pre-Resolve pass or Resolve
                           // failed; decls/refs/exprs may then be partial or empty.
                           // Errors in passes after Resolve leave it true: the
                           // snapshot was already taken from a resolved unit
  "diagnostics": [Diagnostic],
  "decls": [Decl],
  "refs": [Ref],
  "exprs": [Expr],
  "scopes": [Scope]
}
```

When `complete` is false:

- after a syntax error, there are no decls, refs, exprs or scopes;
- after an error in a pass before `Resolve`, decls (and type refs) come from
  the partly validated AST, and nothing is resolved;
- after errors in `Resolve`, the statements and global declarations that
  failed stay unresolved (no refs or exprs inside them), and everything else
  is dumped normally. One type error in a function does not hide the rest of
  that function. A declaration whose initializer fails stays in scope, so
  later uses of it are not reported as undeclared.

Errors from passes that only check the program (`Verify Ctor, Dtor &
Assign`, `Check Assertions`, `Check Function Returns`) are reported but do not
stop translation or clear `complete`. Errors from the passes after `Resolve`
(`Fix Init`, for example) end translation, so there is no generated C, but
`complete` stays true.

### Diagnostic

```jsonc
{ "file", "line", "col", "endLine", "endCol",
  "severity": "error" | "warning" | "note",
  "message": "No alternatives for expression Name: sepDisable",
  "detail": "…"            // optional: the long resolver dump, if any
}
```

`message` is the first line of the translator's message. Multi-line resolver
explanations go in `detail`. Bytes that are not part of valid UTF-8 (a single
byte of a multi-byte character the lexer rejected, a Latin-1 string literal)
are written as `\xNN`. A diagnostic that repeats an earlier one (same range,
severity and text) is dropped: the translator copies some code, such as the
range of a `for` loop, and reports its errors once per copy.

A diagnostic whose location has a line but no column covers the whole line. A
diagnostic with no location at all is put at line 1, column 0 of the first
focus file. A translator failure that is not a user error is an error with no
location whose message starts with `internal translator error:`, followed by
what failed: an uncaught exception, `assertion "EXPR" failed in FUNCTION
(FILE:LINE)` and the assertion's message, `segmentation fault`, `bus error`,
`arithmetic exception (SIGFPE)`, `aborted` or `out of memory`. After a crash
the dump has the diagnostics collected until then. It keeps the snapshot (and
`complete: true`) only if the crash came after a `Resolve` without errors;
otherwise `complete` is false and decls, refs, exprs and scopes are empty.
`severity` is `error` or `warning`; the translator has no notes.

### Decl

```jsonc
{
  "id": 17,                     // unique within this dump only
  "name": "push",               // source name, not the mangled name
  "kind": "function",           // see list below
  "file", "line", "col", "endLine", "endCol",   // whole declaration
  "nameRange": {"line", "col", "endLine", "endCol", "pline", "endPline"},  // just the name token
  "type": "void (Stack(T) &, T)",   // CFA-style type text
  "signature": "forall( T ) void push( Stack(T) & s, T x )",  // source-like declaration text
  "parent": 4 | null,           // enclosing function / aggregate / trait decl id
  "typeDecl": 9 | null,         // for variables, params, fields, functions and typedefs:
                                // the struct/union/enum/coroutine/monitor/thread/exception
                                // decl of the declared (or returned) type after stripping
                                // references, pointers, qualifiers and generic args
  "params": ["s", "x"],         // functions only: parameter names in order
  "body": {"line","col","endLine","endCol","pline","endPline"} | null,  // function body / aggregate braces
  "generated": false,           // true for autogenerated ctors/dtors/assign and
                                // declarations introduced by desugaring
  "local": false                // true for parameters and block-scope declarations
}
```

`kind` is one of: `function`, `variable`, `parameter`, `field`, `struct`,
`union`, `enum`, `enumerator`, `trait`, `typedef`, `typeParam`, `coroutine`,
`monitor`, `thread`, `generator`, `exception`, `label`.

The whole-declaration range runs from the start of the declaration to its end
(including the body or initializer, and the `;` for declarations in a list).
Declarations in one list (`int x, y;`) share it. If it is unknown, it equals
`nameRange`.

A declaration is `generated` if the translator made it: its linkage is
autogen (constructors, destructors, assignment) or the source at `nameRange`
does not spell its name (desugaring reuses the location of the code it came
from). Generated declarations are dumped only for focus files, and never as
the target of a ref. Anonymous aggregates get generated names
(`__anonymous3`), so they are generated, but their members are not. Other names
the translator changes are dumped as written: a nested aggregate (renamed
`__Outer__Inner`), the parameter of a generic aggregate (`__T_generic_`) and a
postfix function (`__postfix_func_len`, dumped as ``?`len``). The parameters
of a generic aggregate or a trait are `local`, with the aggregate as parent.

A label is a declaration of kind `label` in the focus files. Both its range and
`nameRange` are the label's name in `L: stmt`. Its `parent` is the enclosing
function, it is `local`, its `type` is empty and its `signature` is `L:`.
Labels are not in any scope. Labels the translator makes (multi-level exits,
array initialization) are not dumped.

`type` and `signature` come from the translator's pretty printer, tidied: basic
types are spelled the short way (`int`, `long`, `unsigned long`, `bool`), the
generated name of an anonymous aggregate is shown as `(anonymous)` (or, for
`typedef struct { ... } Foo`, as `Foo`), the other renamed names above are
shown as written, and a `forall` type parameter shows its kind as written (`T`,
`T &`, `T *`, `T ...`). Assertions are left out. A parameter with a default
argument shows it (`const char *mode = "r"`). Array and function parameters
show as written (`int a[]`, `int b[N]`) in the function's `signature` and in
the parameter's `type` and `signature`. The function's `type` shows them
decayed to pointers.

Typedefs are replaced before `Resolve`, so they are dumped from what the
translator recorded: global typedefs for every file, local ones for focus
files.

Doc comments are not in the dump; the server reads them from the source above
`nameRange`.

### Ref

A use of a declaration in a focus file, after resolution.

```jsonc
{ "file", "line", "col", "endLine", "endCol",   // the name token as written
  "decl": 17,
  "role": "read" | "call" | "member" | "type" | "with" }
```

`call` is the function name in a resolved call; `member` is the field name
in `x.f` or `x->f`, or a field reached through `with`; `type` is a type name
in a declaration or cast; `with` is a name resolved through a `with` clause
(the server shows these like `member`). A member is `with` when its aggregate
in the resolved AST is the expression of a `with` clause, and `member`
otherwise.

An operator written as one (`v + w`, `-x`, `i++`) is a `call` ref at the
operator token, whose text is the operator's name without the `?`s. The token
can be on a different line from its operands, but it has to be the only
occurrence of the operator between them. A postfix call ``x`len`` is a `call`
ref at `len`. `?[?]`, `?()` and `?{}` are `call` refs at their opening bracket:
`a[i]`, `f( x )`, `p{ 1, 2 }`, and `Point p = { 1, 2 }` when that calls a
constructor. Autogenerated constructors are generated declarations, so calls
to them are not refs. Other implicit operator and constructor calls are not
refs.

A label named by `goto`, `break`, `continue` or `fallthrough` is a `read` ref
at the name. The names in an array dimension (`int a[N]`) are refs in the
declarations of variables, fields and parameters in a focus file, but not in
typedefs, casts or `sizeof`. The type names in an assertion written in a
`forall` (`T` in `forall( T | { int ?<?( T, T ); } )`) and in the arguments
of a trait instance (`S` in `forall( S | Shape( S ) )`) are `type` refs.

A ref is dumped only if the source at its range spells the declaration's name,
which drops uses inside generated code that reuse a source location. This
holds inside code that desugaring moves into generated declarations (tuple
assignment, `with` on an rvalue, compound literals such as
`throw (E){ &vt, x }`), inside `sizeof( e )` and `typeof( e )`, whose
expressions the translator keeps after replacing them with their types, and in
the bodies of nested functions that desugaring makes, such as the one holding
the statement of a `corun` or the body of a `cofor`. Those functions are not
dumped, and their braces are a scope only when the user wrote them. Inside a
`cofor` body, the loop variable is a generated copy, so its uses there are not
refs. The target is the declaration the resolver chose, which for a function
or variable declared more than once can be a prototype rather than the
definition. A use of a typedef refers to the typedef; a use of a trait in a
`forall` assertion refers to the trait.

### Expr

A resolved expression in a focus file, for hover on expressions and member
completion.

```jsonc
{ "file", "line", "col", "endLine", "endCol",
  "type": "struct string &",
  "typeDecl": 9 | null }        // same stripping rule as Decl.typeDecl
```

The translator emits names (the name token), member accesses (the member
name token), calls whose function name is spelled in the source (the whole
call, arguments included) and explicit casts (the whole cast). Literals,
operators and implicit conversions are skipped.

### Scope

Lexical blocks in focus files, for local-name completion.

```jsonc
{ "file", "line", "col", "endLine", "endCol",
  "parent": 2 | null,           // index into "scopes"
  "decls": [31, 32] }           // decl ids declared directly in this block
                                // (parameters belong to the function body's scope)
```

Scopes are the compound statements (`{ ... }`) of focus files, function
bodies included. A declaration in a `for`, `if` or `while` header gets a scope
of its own covering the whole statement (the translator hoists it into a
block), and a `catch` clause's declaration belongs to the handler's block.
Blocks that desugaring makes at user locations (for tuple assignment, say) are
not scopes. A generator main's scope is its braces, though the translator wraps
the body in a block of its own. Local typedefs are in the scope of their block.
