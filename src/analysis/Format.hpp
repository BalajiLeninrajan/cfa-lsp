#pragma once

#include <climits>
#include <string>
#include <string_view>

namespace cfalsp {

// A formatter that only changes whitespace. It reindents lines by brace
// depth, sets the spacing between tokens on a line, trims trailing
// whitespace and, if asked, fixes the newlines at the end of the file. It never adds, removes or changes a token, so CFA syntax
// that C formatters mangle (?{}, ^?{}, with clauses, sout | x chains) comes
// through as written.
//
// Lines that start a statement or declaration get the indentation of their
// brace depth, plus one level for the body of a `case` label and for the
// body of an `if`, `while`, `for`, `with`, `else` or `do` without braces.
// Other lines (continuations of a statement, arguments split over lines)
// move by as much as the line that started the statement moved, so
// alignment inside a statement is kept. Preprocessor directives keep their
// indentation, and lines inside a block comment move with the line the
// comment starts on.
//
// Within a line, the spacing follows libcfa: one space inside non-empty
// parentheses and none inside `()`, one after a comma or semicolon and none
// before, one between if, for, while, switch or choose and its `(`, and one
// before a `{` that follows a `)` or a name. Other runs of blanks between
// tokens become one space. Directives and the space before or after a
// comment are left alone.
struct FormatOptions {
	int tabSize = 4;
	bool insertSpaces = false;
	bool trimTrailingWhitespace = true;
	bool insertFinalNewline = false;		// only when lastLine is the last line
	bool trimFinalNewlines = false;			// only when lastLine is the last line
};

// `text` formatted. Only lines firstLine..lastLine (0-based, inclusive)
// change; the rest of the text is read for context.
std::string formatText( std::string_view text, const FormatOptions & opts, int firstLine = 0, int lastLine = INT_MAX );

} // namespace cfalsp
