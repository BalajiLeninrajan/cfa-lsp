# Limits

- A check runs the translator over the file and every header it includes.
  The translator skips the bodies of the functions in libcfa and system
  headers (`skipSystemBodies`), which makes a check of a cs343 assignment 2
  to 4 times faster, but it still parses and resolves every declaration in
  them: about 2 seconds for a small program that includes `fstream.hfa`,
  `string.hfa` and `stdlib.hfa`. `stopAfterResolve` cuts a further part of
  that. [persistent-translator.md](persistent-translator.md) describes how a long-running
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
