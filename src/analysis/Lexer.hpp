#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace cfalsp {

// A C/Cforall tokenizer for source text (original files or `cfa -E` output).
// It is not a preprocessor: it does not expand macros or evaluate #if.
// Tokens follow the CFA translator's lexer (cforall/src/Parser/lex.ll), so
// CFA operator names such as ?+?, ?{} and ^?{} are single Identifier tokens.

enum class TokKind : unsigned char {
	Identifier,		// identifiers, keywords and CFA operator names (?+?, ?{}, ^?{}, ?`ident)
	Number,			// a pp-number: 0x1fUL, 1.5e-3f, 1_000, ...
	String,			// "..." with an optional u8/u/U/L prefix; may be unterminated
	Char,			// '...' with an optional u8/u/U/L prefix; may be unterminated
	Punct,			// punctuators, longest match
	Comment,		// only when LexOptions::comments is set
	Directive,		// a whole preprocessor directive, only when LexOptions::directives is set
	Other,			// any other byte
};

struct Token {
	TokKind kind = TokKind::Other;
	std::string text;			// spelling with backslash-newline splices removed
	int line = 0, col = 0;		// 0-based start line; col is a byte offset in that physical line
	int endLine = 0, endCol = 0;	// exclusive end; differs from line only for tokens that
								// contain a splice, block comments and directives
	int offset = 0, endOffset = 0;	// byte offsets into the lexed text, [offset, endOffset)
};

struct LexOptions {
	bool comments = false;		// emit Comment tokens instead of dropping them
	bool directives = false;	// emit each directive line (with its continuations) as one
								// Directive token instead of dropping it
	bool cfa = true;			// CFA operator names and punctuators (@=, ~=, -~, \, ...)
};

// Tokenizes `text`. Never throws on malformed input.
std::vector<Token> lex( std::string_view text, const LexOptions & opts = {} );

} // namespace cfalsp
